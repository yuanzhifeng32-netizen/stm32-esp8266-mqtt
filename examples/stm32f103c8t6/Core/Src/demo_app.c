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
    ESP_LOG("  broker : %s:%u   cmd topic: %s\r\n",
            ESP_MQTT_HOST, (unsigned)ESP_MQTT_PORT, ESP_NET_TOPIC_CMD);
    ESP_LOG("  pub    : %s   period: %ld s\r\n",
            DEMO_TOPIC_PUB, (long)s_reportPeriodS);
    ESP_LOG("  build  : %s %s   heap_free: %u B\r\n",
            __DATE__, __TIME__, (unsigned)xPortGetFreeHeapSize());
    ESP_LOG("============================================\r\n");

    /* ---- 业务主循环：每秒跑一次，联网后按周期上报 ---- */
    for (;;)
    {
        char payload[64];
        int  len;

        osDelay(1000U);
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