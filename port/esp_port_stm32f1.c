/**
 * @file    esp_port_stm32f1.c
 * @brief   移植层参考实现：STM32F1 + HAL + FreeRTOS
 *
 * ---------------------------------------------------------------------------
 * 硬件约定（按自己的板子改 gpio.c / usart.c 即可）
 * ---------------------------------------------------------------------------
 *   USART2 : PA2(TX) / PA3(RX) 115200 8-N-1  <--> ESP8266
 *   接收   : DMA1_Channel6 循环(Circular)模式往 512 B 缓冲里绕圈写；
 *            再用**串口 IDLE 空闲中断**把新到的那一段喂给 esp_net_input()。
 *            为什么不用 DMA 半满/全满中断？因为 "OK\r\n" 这种短应答不到 256 B，
 *            等 DMA 计数到一半才中断的话，上层会一直等不到数据。
 *   发送   : 阻塞式 HAL_UART_Transmit（115200 下 256 B 约 22 ms，可接受）。
 *   PA8    : ESP8266 使能/复位脚（高电平使能）。
 *
 * 首发协议栈：MIT
 */

#include "esp_port_stm32f1.h"
#include "esp_net.h"

#include "usart.h"
#include "main.h"
#include "cmsis_os.h"

/* DMA 循环接收缓冲。512 B @115200 约 44 ms 写满一圈，远大于正常收包间隔 */
#define ESP_PORT_DMA_RX_SIZE    512U
/* 串口中断优先级：必须 >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(5)，
   保证中断里不打断 FreeRTOS 的临界区（本 ISR 不调 RTOS API，取 5 更保险） */
#define ESP_PORT_IRQ_PRIO       5U

/* ---------------------------------------------------------------- 私有数据 */

static uint8_t  s_dmaRx[ESP_PORT_DMA_RX_SIZE];
static uint16_t s_dmaLastPos;   /* 上次已喂给库的写指针位置（读游标） */

/**
 * @brief  把 DMA 缓冲里新到的数据喂给库。
 * @note   CNDTR 从 size 递减到 0 再自动重装，所以 pos = size - CNDTR 就是当前写指针。
 *         pos < lastPos 说明回绕了，分两段取。
 */
static void port_pump_from_dma(void)
{
    uint16_t pos = (uint16_t)(ESP_PORT_DMA_RX_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart2_rx));

    if (pos >= ESP_PORT_DMA_RX_SIZE)
    {
        pos = 0U;   /* CNDTR == 0 时整圈写完，写指针回到 0 */
    }
    if (pos == s_dmaLastPos)
    {
        return;
    }

    if (pos > s_dmaLastPos)
    {
        /* 没回绕：取 [lastPos, pos) */
        esp_net_input(&s_dmaRx[s_dmaLastPos], (uint16_t)(pos - s_dmaLastPos));
    }
    else
    {
        /* 回绕了：先取到缓冲末尾，再从头取到 pos */
        esp_net_input(&s_dmaRx[s_dmaLastPos], (uint16_t)(ESP_PORT_DMA_RX_SIZE - s_dmaLastPos));
        if (pos > 0U)
        {
            esp_net_input(s_dmaRx, pos);
        }
    }
    s_dmaLastPos = pos;
}

/* ---------------------------------------------------------- esp_port_t 实现 */

/** @brief 硬件初始化：启动 USART2 的 DMA 循环接收并打开 IDLE 空闲中断 */
static void port_init(void)
{
    s_dmaLastPos = 0U;

    if (HAL_UART_Receive_DMA(&huart2, s_dmaRx, ESP_PORT_DMA_RX_SIZE) != HAL_OK)
    {
        Error_Handler();
    }

    /* 本项目靠空闲中断取数据，不用 DMA 的 TC/HT。
       HAL_UART_Receive_DMA 内部会打开 DMA 的 TC/HT/错误中断并把串口 EIE 置 1，
       这里统一清掉，免得中断标志悬空没人处理。 */
    __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE);
    CLEAR_BIT(huart2.Instance->CR3, USART_CR3_EIE);

    /* 打开串口 IDLE 空闲中断：一帧收完（线路空闲一个字符时间）就触发 */
    __HAL_UART_ENABLE_IT(&huart2, UART_IT_IDLE);
    HAL_NVIC_SetPriority(USART2_IRQn, ESP_PORT_IRQ_PRIO, 0U);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/** @brief 把一段字节发给 ESP8266 */
static void port_uart_write(const uint8_t *data, uint16_t len)
{
    /* 100 ms 超时而不是 HAL_MAX_DELAY：串口异常时不能把任务卡死 */
    (void)HAL_UART_Transmit(&huart2, (uint8_t *)data, len, 100U);
}

/** @brief 控制 PA8：1 = 使能（高），0 = 复位（低） */
static void port_reset_pin(uint8_t level)
{
    HAL_GPIO_WritePin(ESP_EN_GPIO_Port, ESP_EN_Pin,
                      (level != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/** @brief 毫秒延时（RTOS 下让出 CPU；必须在调度器启动后调用） */
static void port_delay_ms(uint32_t ms)
{
    osDelay(ms);
}

/** @brief 毫秒时基 */
static uint32_t port_tick_ms(void)
{
    return HAL_GetTick();
}

/* 移植层实例：用户把这个地址传给 esp_net_init() */
const esp_port_t esp_port_stm32f1 = {
    port_init,
    port_uart_write,
    port_reset_pin,
    port_delay_ms,
    port_tick_ms
};

/* ---------------------------------------------------------------- 中断服务 */

/**
 * @brief  USART2 中断：只处理 IDLE（空闲）事件，把新到的数据喂给库。
 * @note   Core/Src/stm32f1xx_it.c 里没有同名函数，所以在这里定义不会重复。
 *         这里不调用 HAL_UART_IRQHandler()：我们不用 HAL 的收发中断机制，
 *         只借 IDLE 标志当"一帧收完了"的信号。
 */
void USART2_IRQHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_IDLE) != RESET)
    {
        __HAL_UART_CLEAR_IDLEFLAG(&huart2);   /* 读 SR + DR 清标志 */
        port_pump_from_dma();
    }
}