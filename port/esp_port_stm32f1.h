/**
 * @file    esp_port_stm32f1.h
 * @brief   STM32F1 + HAL 的移植层实现（参考实现）
 *
 * 用法：把它当成 esp_net_init() 的参数——
 *     #include "esp_port_stm32f1.h"
 *     esp_net_init(&esp_port_stm32f1);
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_PORT_STM32F1_H
#define ESP_PORT_STM32F1_H

#include "esp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 移植层回调实例：USART2(PA2/PA3) + DMA1_Ch6 循环接收 + 串口 IDLE 中断喂数据 + PA8 复位 */
extern const esp_port_t esp_port_stm32f1;

#ifdef __cplusplus
}
#endif

#endif /* ESP_PORT_STM32F1_H */