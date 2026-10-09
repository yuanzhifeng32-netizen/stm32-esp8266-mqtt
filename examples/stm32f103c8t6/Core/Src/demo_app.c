/**
 * @file    demo_app.c
 * @brief   示例业务实现 —— 这就是"用框架写业务"的样子
 *
 * 可以看到：业务代码里完全看不到 ESP8266、AT 指令、MQTT 报文，
 * 只有三个 API：esp_net_is_online() / esp_net_publish() / esp_net_var_register()。
 *
 * 首发协议栈：MIT
 */

#include "demo_app.h"
#include "demo_config.h"

#include "esp_net.h"
#include "esp_net_config.h"
#include "main.h"               /* WIFI_CFG_Pin / HAL_GPIO_ReadPin（PB6 配网按键） */

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"

#include <stdio.h>
#include <string.h>

/* 上报周期（秒）：登记到下行命令表后，发 "period=5" 就能在线改 */
static int32_t s_reportPeriodS = DEMO_REPORT_PERIOD_S;

/**
 * @brief 收到下行消息的回调。
 * @note  框架已经把命令主题里的 "名字=数值" 处理过了（改 s_reportPeriodS），
 *        这里演示"收到任何消息就回一条"的自定义业务。
 */
static void demo_on_message(const char *topic, uint16_t topic_len,
                            const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOG("[demo] message on %.*s, echo it back\r\n", (int)topic_len, topic);

    /* 线程安全：入队后由联网任务发出，本回调在联网任务里执行也不会卡住 */
    (void)esp_net_publish(DEMO_TOPIC_PUB, payload, payload_len, 0U, 0U);
}

void demo_app_task(void *argument)
{
    uint32_t seconds = 0U;
    uint32_t seq     = 0U;
    uint32_t tick100 = 0U;   /* 100 ms 节拍计数，累计 10 次 = 1 秒 */
#if ESP_PROV_ENABLE
    uint32_t holdMs   = 0U;   /* PB6 连续按住的时长（ms） */
    uint8_t  trigLock = 0U;   /* 触发一次后置 1，松开才清零，避免一直按着每 3 s 反复触发 */
    uint8_t  ledOn    = 0U;   /* PC13 配网闪烁的当前相位 */
#endif

    (void)argument;

    /* ---- 注册业务：一个可被下行命令修改的变量 + 一个下行消息回调 ---- */
    (void)esp_net_var_register("period", &s_reportPeriodS);
    esp_net_on_message(demo_on_message);

    /* ---- 上电横幅 ---- */
    ESP_LOG("\r\n");
    ESP_LOG("============================================\r\n");
    ESP_LOG("  stm32103 | STM32F103C8T6 | ESP8266 + MQTT (demo)\r\n");
    ESP_LOG("  USART2 : PA2/PA3 115200 <--DMA ch6--> ESP8266\r\n");
    ESP_LOG("  USART1 : PA9/PA10 115200   debug log\r\n");
#if (ESP_MQTT_TRANSPORT == ESP_MQTT_TRANSPORT_WEBSOCKET)
    ESP_LOG("  broker : ws://%s:%u%s   cmd topic: %s\r\n",
            ESP_MQTT_HOST, (unsigned)ESP_WS_PORT, ESP_WS_PATH, ESP_NET_TOPIC_CMD);
#else
    ESP_LOG("  broker : tcp://%s:%u   cmd topic: %s\r\n",
            ESP_MQTT_HOST, (unsigned)ESP_MQTT_PORT, ESP_NET_TOPIC_CMD);
#endif
    ESP_LOG("  pub    : %s   period: %ld s\r\n",
            DEMO_TOPIC_PUB, (long)s_reportPeriodS);
#if ESP_PROV_ENABLE
    ESP_LOG("  wifi   : hold PB6 %u ms or no creds -> provisioning (AP \"%s\", PC13 blinks)\r\n",
            (unsigned)ESP_PROV_HOLD_MS, ESP_PROV_AP_SSID);
#endif
    ESP_LOG("  build  : %s %s   heap_free: %u B\r\n",
            __DATE__, __TIME__, (unsigned)xPortGetFreeHeapSize());
    ESP_LOG("============================================\r\n");

    /* ---- 业务主循环：100 ms 一拍（顺带轮询配网按键），满 1 秒做一次上报节拍 ---- */
    for (;;)
    {
        char payload[64];
        int  len;

        osDelay(100U);

        /* ---- 配网按键：PB6 连续按住满 ESP_PROV_HOLD_MS 就请求配网 ---- */
#if ESP_PROV_ENABLE
        if (HAL_GPIO_ReadPin(WIFI_CFG_GPIO_Port, WIFI_CFG_Pin) == GPIO_PIN_RESET)
        {
            if (trigLock == 0U)
            {
                holdMs += 100U;
                if (holdMs >= ESP_PROV_HOLD_MS)
                {
                    holdMs   = 0U;
                    trigLock = 1U;   /* 锁住，必须松手后才能再次触发 */
                    ESP_LOG("[demo] PB6 hold -> request wifi config\r\n");
                    esp_net_request_config();
                }
            }
        }
        else
        {
            holdMs   = 0U;
            trigLock = 0U;
        }

        /* ---- 状态灯 PC13：配网模式期间闪烁提示，其余时间熄灭 ---- */
        if (esp_net_is_configuring() != 0U)
        {
            ledOn ^= 1U;
            if (ledOn != 0U) { STATUS_LED_ON(); } else { STATUS_LED_OFF(); }
        }
        else if (ledOn != 0U)
        {
            ledOn = 0U;
            STATUS_LED_OFF();
        }
#endif

        /* ---- 满 1 秒才走一次上报节拍 ---- */
        tick100++;
        if (tick100 < 10U)
        {
            continue;
        }
        tick100 = 0U;
        seconds++;

        if ((esp_net_is_online() == 0U) || (s_reportPeriodS <= 0))
        {
            continue;   /* 没联网 / 周期为 0（下行 "period=0"）就先不发 */
        }
        if ((seconds % (uint32_t)s_reportPeriodS) != 0U)
        {
            continue;
        }

        seq++;
        len = snprintf(payload, sizeof(payload),
                       "{\"uptime\":%lu,\"seq\":%lu}",
                       (unsigned long)seconds, (unsigned long)seq);
        if (len < 0)
        {
            len = 0;
        }

        if (esp_net_publish(DEMO_TOPIC_PUB, (const uint8_t *)payload,
                            (uint16_t)len, 0U, 0U) != 0)
        {
            ESP_LOG("[demo] publish queue full, drop this round\r\n");
        }
    }
}