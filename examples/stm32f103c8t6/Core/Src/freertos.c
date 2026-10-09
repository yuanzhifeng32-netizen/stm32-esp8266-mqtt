/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "usart.h"              /* huart1 / huart2 / hdma_usart1_rx / hdma_usart2_rx */
#include "demo_config.h"        /* demo 自己的开关（框架参数见 library/esp_net_config.h） */
#include "esp_net.h"            /* 框架 API：esp_net_init / esp_net_task ... */
#include "esp_net_config.h"     /* 框架参数（ESP_PROV_ENABLE / ESP_LOG ...） */
#include "esp_port_stm32f1.h"   /* 移植层实例 esp_port_stm32f1 */
#include "demo_app.h"           /* demo_app_task() */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 联网任务栈：512 字 = 2048 字节。
   这个任务里要跑 snprintf、strstr、MQTT 报文解析，留足余量。
   注意 FreeRTOS 堆（configTOTAL_HEAP_SIZE）只有 3072 B，且已被 defaultTask /
   timer 任务占去大半，所以本任务必须**静态分配**（见下面的 StaticTask_t）。 */
#define NET_TASK_STACK_WORDS     512U
/* demo 任务栈：256 字 = 1024 字节，只做 snprintf + 入队，够用 */
#define DEMO_TASK_STACK_WORDS    256U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* ---- 联网任务（静态分配的栈 + TCB）---- */
static StaticTask_t netTaskCb;
static StackType_t  netTaskStack[NET_TASK_STACK_WORDS];

osThreadId_t netTaskHandle;
const osThreadAttr_t netTask_attributes = {
  .name = "netTask",
  .cb_mem = &netTaskCb,
  .cb_size = sizeof(netTaskCb),
  .stack_mem = &netTaskStack[0],
  .stack_size = sizeof(netTaskStack),
  .priority = (osPriority_t) osPriorityNormal,
};

/* ---- demo 业务任务（静态分配；业务与联网任务分开，互不干扰）---- */
static StaticTask_t demoTaskCb;
static StackType_t  demoTaskStack[DEMO_TASK_STACK_WORDS];

osThreadId_t demoTaskHandle;
const osThreadAttr_t demoTask_attributes = {
  .name = "demoTask",
  .cb_mem = &demoTaskCb,
  .cb_size = sizeof(demoTaskCb),
  .stack_mem = &demoTaskStack[0],
  .stack_size = sizeof(demoTaskStack),
  .priority = (osPriority_t) osPriorityNormal,
};

#if APP_BRIDGE_DEBUG_ENABLE
/* ============================================================================
   以下是「备用调试方案」专用的数据：USART1 ↔ USART2 原始双向透传。
   联网任务跑不通时，把 demo_config.h 里的 APP_BRIDGE_DEBUG_ENABLE 改成 1，
   就可以用串口助手手工敲 AT 指令直接跟 ESP8266 对话。
   ========================================================================= */

/* 串口2 的 DMA 循环接收缓冲（ESP8266 → 电脑）；rx2LastPos 记录上次取走的写指针 */
static uint8_t  rx2DmaBuffer[BRIDGE_RX_BUFFER_SIZE];
static uint16_t rx2LastPos = 0U;

#if BRIDGE_TO_ESP_ENABLE
/* 串口1 的 DMA 循环接收缓冲（电脑 → ESP8266）；rx1LastPos 同上 */
static uint8_t  rx1DmaBuffer[BRIDGE_RX_BUFFER_SIZE];
static uint16_t rx1LastPos = 0U;
#endif

#if BRIDGE_OUTPUT_HEX
/* 十六进制打印用的行缓冲（每行 16 字节 × 3 字符 + CRLF） */
static char bridgeHexLine[3U * 16U + 2U];
#endif

#if (BRIDGE_REPORT_PERIOD_MS > 0U)
static uint32_t debugReportCount = 0U;
static uint32_t debugLastReport  = 0U;
#endif
static uint32_t rx2TotalBytes    = 0U;
static uint32_t rx2BurstCount    = 0U;
#if BRIDGE_TO_ESP_ENABLE
static uint32_t rx1TotalBytes    = 0U;
static uint32_t rx1BurstCount    = 0U;
#endif
#endif /* APP_BRIDGE_DEBUG_ENABLE */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

#if APP_BRIDGE_DEBUG_ENABLE
static void BridgeWriteUart(UART_HandleTypeDef *dst, const uint8_t *p, uint16_t len, uint8_t hex);
static void BridgePump(DMA_HandleTypeDef *hdma, const uint8_t *buf, uint16_t size,
                       uint16_t *lastPos, UART_HandleTypeDef *dst, uint8_t hex,
                       uint32_t *totalBytes, uint32_t *burstCount);
static void StartBridgeDebugLoop(void);
#endif
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* 关闭 stdio 缓冲：printf 就不会去 malloc（链接脚本里的堆只有 0x200） */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* ---- 启动串口接收 ----
     放在这里（调度器启动之前）是为了不让上电后最早到的数据丢掉。 */
#if APP_BRIDGE_DEBUG_ENABLE
  /* 备用调试方案：两个方向的原始透传，两条 DMA 循环接收都要开 */
  if (HAL_UART_Receive_DMA(&huart2, rx2DmaBuffer, BRIDGE_RX_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
#if BRIDGE_TO_ESP_ENABLE
  if (HAL_UART_Receive_DMA(&huart1, rx1DmaBuffer, BRIDGE_RX_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
#endif
  /* 本项目靠任务轮询写指针取数据，不用 DMA 中断；关掉，免得中断标志悬空没人处理。 */
  __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
  CLEAR_BIT(huart2.Instance->CR3, USART_CR3_EIE);
#if BRIDGE_TO_ESP_ENABLE
  __HAL_DMA_DISABLE_IT(&hdma_usart1_rx, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
  CLEAR_BIT(huart1.Instance->CR3, USART_CR3_EIE);
#endif
#else
  /* 正常联网模式：绑定移植层 + 启动串口接收（DMA 循环 + 空闲中断喂数据） */
  esp_net_init(&esp_port_stm32f1);
#if ESP_PROV_ENABLE
  /* 上电触发配网：若此刻 PB6 已按住（低电平），开机就直接进配网，不再尝试旧 WiFi。
     放在 esp_net_init() **之后**：init 里会把配网标志清零，这里再置位才有效。 */
  if (HAL_GPIO_ReadPin(WIFI_CFG_GPIO_Port, WIFI_CFG_Pin) == GPIO_PIN_RESET)
  {
    HAL_Delay(30U);   /* 简单消抖：30 ms 后仍为低才算真按下 */
    if (HAL_GPIO_ReadPin(WIFI_CFG_GPIO_Port, WIFI_CFG_Pin) == GPIO_PIN_RESET)
    {
      ESP_LOG("[net] PB6 held at boot -> provisioning\r\n");
      esp_net_request_config();
    }
  }
#endif
#endif
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* 联网任务（框架内部死循环：连 WiFi→TCP→MQTT→收发保活，断线自愈） */
  netTaskHandle = osThreadNew(esp_net_task, NULL, &netTask_attributes);
#if !APP_BRIDGE_DEBUG_ENABLE
  /* demo 业务任务：与联网任务解耦，自己做自己的周期上报 */
  demoTaskHandle = osThreadNew(demo_app_task, NULL, &demoTask_attributes);
#endif
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

#if APP_BRIDGE_DEBUG_ENABLE
/* ============================================================================
   备用调试方案：串口2(PA2/PA3) ↔ 串口1(PA9/PA10) 原始双向透传
     方向 A：ESP8266 发 → 串口2 RX(DMA1_Ch6) → 串口1 TX(PA9)，在电脑上看
     方向 B：电脑发   → 串口1 RX(DMA1_Ch5) → 串口2 TX(PA2)，即给 ESP8266 发 AT 指令
   ========================================================================= */

/**
  * @brief  把一段数据写到目标串口。
  * @param  dst: 目标串口（&huart1 = 发给电脑，&huart2 = 发给 ESP8266）
  * @param  p: 数据首地址   len: 字节数
  * @param  hex: 仅在 BRIDGE_OUTPUT_HEX=1 时有效：1=转成十六进制打印，0=原样
  */
static void BridgeWriteUart(UART_HandleTypeDef *dst, const uint8_t *p, uint16_t len, uint8_t hex)
{
#if BRIDGE_OUTPUT_HEX
  if (hex != 0U)
  {
    static const char hexDigits[] = "0123456789ABCDEF";
    char *out = bridgeHexLine;
    uint8_t n = 0U;
    uint16_t i;

    for (i = 0U; i < len; i++)
    {
      *out++ = hexDigits[(p[i] >> 4) & 0x0FU];
      *out++ = hexDigits[p[i] & 0x0FU];
      *out++ = ' ';
      if (++n == 16U)
      {
        *out++ = '\r';
        *out++ = '\n';
        HAL_UART_Transmit(dst, (uint8_t *)bridgeHexLine, (uint16_t)(out - bridgeHexLine), 1000U);
        out = bridgeHexLine;
        n = 0U;
      }
    }
    if (n > 0U)
    {
      *out++ = '\r';
      *out++ = '\n';
      HAL_UART_Transmit(dst, (uint8_t *)bridgeHexLine, (uint16_t)(out - bridgeHexLine), 1000U);
    }
    return;
  }
#else
  (void)hex;
#endif
  /* 原样转发：整段一次性发出去，比逐字节 printf 快得多 */
  HAL_UART_Transmit(dst, (uint8_t *)p, len, 1000U);
}

/**
  * @brief  取走"某个 DMA 循环缓冲里这段时间新到的"数据，转发到目标串口。
  * @note   CNDTR 从 size 递减到 0 再自动重装，所以 pos = size - CNDTR 就是
  *         DMA 当前的写指针（0 .. size-1）；[lastPos, pos) 即新到的数据，
  *         pos < lastPos 说明回绕了，分两段取。
  */
static void BridgePump(DMA_HandleTypeDef *hdma, const uint8_t *buf, uint16_t size,
                       uint16_t *lastPos, UART_HandleTypeDef *dst, uint8_t hex,
                       uint32_t *totalBytes, uint32_t *burstCount)
{
  uint16_t pos = (uint16_t)(size - __HAL_DMA_GET_COUNTER(hdma));

  if (pos == *lastPos)
  {
    return;
  }

  if (pos > *lastPos)
  {
    /* 没回绕：取 [lastPos, pos) */
    BridgeWriteUart(dst, &buf[*lastPos], (uint16_t)(pos - *lastPos), hex);
    if (totalBytes != NULL)
    {
      *totalBytes += (uint32_t)(pos - *lastPos);
    }
  }
  else
  {
    /* 回绕了：先取到缓冲末尾，再从头取到 pos */
    BridgeWriteUart(dst, &buf[*lastPos], (uint16_t)(size - *lastPos), hex);
    if (pos > 0U)
    {
      BridgeWriteUart(dst, buf, pos, hex);
    }
    if (totalBytes != NULL)
    {
      *totalBytes += (uint32_t)(size - *lastPos) + (uint32_t)pos;
    }
  }
  *lastPos = pos;
  if (burstCount != NULL)
  {
    (*burstCount)++;
  }
}

/**
  * @brief  透传调试主循环（不返回）
  *         1) 串口2 经 DMA 循环收到的 ESP8266 数据 → 串口1，在电脑上看；
  *         2) 电脑从串口1 发的字节 → 串口2 TX，即给 ESP8266 发 AT 指令；
  *         3) 可选：周期心跳。
  */
static void StartBridgeDebugLoop(void)
{
  /* ---------------- 上电横幅 ---------------- */
  printf("\r\n");
  printf("============================================\r\n");
  printf("  stm32103 | STM32F103C8T6 | uart bridge (debug fallback)\r\n");
  printf("  USART2 : PA2(TX) / PA3(RX) <--DMA ch6--> USART1\r\n");
#if BRIDGE_TO_ESP_ENABLE
  printf("  USART1 : PA9(TX) / PA10(RX) <--DMA ch5--> USART2  (双向透传)\r\n");
#else
  printf("  USART1 : PA9(TX) / PA10(RX)  115200 8-N-1  (单向转发)\r\n");
#endif
  printf("  build  : %s %s\r\n", __DATE__, __TIME__);
  printf("  SYSCLK : %lu Hz   heap_free: %u B\r\n",
         (unsigned long)HAL_RCC_GetSysClockFreq(),
         (unsigned int)xPortGetFreeHeapSize());
  printf("  tip    : 用串口助手直接敲 AT 指令（如 AT+GMR）\r\n");
  printf("============================================\r\n");

  /* ---------------- 主循环 ---------------- */
  for (;;)
  {
    /* ---- 1) 串口2 → 串口1：ESP8266 的输出转发给电脑 ---- */
    BridgePump(&hdma_usart2_rx, rx2DmaBuffer, BRIDGE_RX_BUFFER_SIZE, &rx2LastPos,
               &huart1, BRIDGE_OUTPUT_HEX, &rx2TotalBytes, &rx2BurstCount);

    /* ---- 2) 串口1 → 串口2：电脑发来的按键/AT 指令转发给 ESP8266 ---- */
#if BRIDGE_TO_ESP_ENABLE
    BridgePump(&hdma_usart1_rx, rx1DmaBuffer, BRIDGE_RX_BUFFER_SIZE, &rx1LastPos,
               &huart2, 0U, &rx1TotalBytes, &rx1BurstCount);
#endif

    /* ---- 3) 周期心跳（BRIDGE_REPORT_PERIOD_MS = 0 时整段不编译）---- */
#if (BRIDGE_REPORT_PERIOD_MS > 0U)
    if ((HAL_GetTick() - debugLastReport) >= BRIDGE_REPORT_PERIOD_MS)
    {
      debugLastReport = HAL_GetTick();
      debugReportCount++;
      printf("\r\n[%6lu ms] alive #%lu | usart2_rx=%lu B / %lu bursts"
#if BRIDGE_TO_ESP_ENABLE
             " | usart1_rx=%lu B / %lu bursts"
#endif
             " | heap_free=%u B | tasks=%u\r\n",
             (unsigned long)HAL_GetTick(),
             (unsigned long)debugReportCount,
             (unsigned long)rx2TotalBytes,
             (unsigned long)rx2BurstCount,
#if BRIDGE_TO_ESP_ENABLE
             (unsigned long)rx1TotalBytes,
             (unsigned long)rx1BurstCount,
#endif
             (unsigned int)xPortGetFreeHeapSize(),
             (unsigned int)uxTaskGetNumberOfTasks());
    }
#endif

    osDelay(BRIDGE_POLL_MS);
  }
}
#endif /* APP_BRIDGE_DEBUG_ENABLE */

/* USER CODE END Application */