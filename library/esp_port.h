/**
 * @file    esp_port.h
 * @brief   移植层接口 —— 本库与具体 MCU / HAL 之间**唯一**的边界
 *
 * ---------------------------------------------------------------------------
 * 怎么用（移植到新平台只做两件事）
 * ---------------------------------------------------------------------------
 *   1) 实现下面这个 esp_port_t 里的几个函数（都是几行代码）；
 *   2) 在自己的串口接收中断（推荐 IDLE 空闲中断）里把收到的字节喂给库：
 *          esp_net_input(data, len);
 *
 *   库内部完全不认识 HAL、不认识某种 RTOS，只通过这里注册的函数指针去碰硬件。
 *   参考实现见 port/esp_port_stm32f1.c（STM32F1 + HAL + FreeRTOS）。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_PORT_H
#define ESP_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  移植层回调集合。用结构体一次传进来，库内部零 RAM 开销。
 * @note   请在全局定义一个 const 实例（如参考实现的 esp_port_stm32f1），
 *         然后把它的地址传给 esp_net_init()。
 */
typedef struct
{
    /**
     * @brief 硬件初始化：启动 UART 的 DMA 循环接收，并打开"收到一批数据就通知库"的
     *        中断（STM32 上用串口 IDLE 空闲中断最合适）。
     * @note  由 esp_net_init() 调用一次，必须能在 FreeRTOS 调度器启动**之前**跑完，
     *        否则上电后 ESP8266 吐的第一批数据会丢。
     */
    void (*init)(void);

    /** @brief 把一段字节发给 ESP8266（阻塞发送即可，115200 下 256 B 约 22 ms） */
    void (*uart_write)(const uint8_t *data, uint16_t len);

    /**
     * @brief 控制 ESP8266 的使能 / 复位脚。
     * @param level 1 = 使能（高电平），0 = 复位（低电平）
     * @note  不接这根线时请填 NULL，库会自动跳过"硬件复位"这一招。
     */
    void (*reset_pin)(uint8_t level);

    /** @brief 毫秒级延时。库用它做超时轮询与 "+++" 前后的静默 */
    void (*delay_ms)(uint32_t ms);

    /** @brief 单调递增的毫秒时基（如 HAL_GetTick / xTaskGetTickCount） */
    uint32_t (*tick_ms)(void);

    /**
     * @brief 复位整个 MCU（配网保存凭据后重启生效用）。
     * @note  不接这根线时填 NULL，库会跳过"保存后自动重启"这一步
     *        （改由用户自己重启设备）。参考实现：NVIC_SystemReset()。
     */
    void (*system_reset)(void);
} esp_port_t;

#ifdef __cplusplus
}
#endif

#endif /* ESP_PORT_H */