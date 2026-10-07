/**
 * @file    esp8266_at.h
 * @brief   ESP8266 AT 指令驱动（库内部使用，用户无需关心）
 *
 * ---------------------------------------------------------------------------
 * 收发模型
 * ---------------------------------------------------------------------------
 *   发送：直接调 port->uart_write()（阻塞式）。
 *   接收：port 层每收到一批字节就调 esp_at_input() 喂进库自持的**环形缓冲**，
 *         上层用 esp_at_read() 按需取走。环形缓冲是"单生产者(中断)+单消费者(任务)"，
 *         不需要加锁。
 *
 * ---------------------------------------------------------------------------
 * 两种链路模式（见 esp_net_config.h 的 ESP_LINK_MODE）
 * ---------------------------------------------------------------------------
 *   透传 TRANSPARENT：进透传后 esp_at_read/esp_at_send 直接读写 TCP 裸字节流。
 *   分帧 FRAME      ：发送自动加 AT+CIPSEND=<len> 握手。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP8266_AT_H
#define ESP8266_AT_H

#include <stdint.h>
#include "esp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 驱动返回码 */
typedef enum
{
    ESP_OK = 0,        /**< 成功 */
    ESP_ERR_TIMEOUT,   /**< 等待超时 */
    ESP_ERR_FAIL,      /**< 模块回了 ERROR / FAIL（指令不合法或参数错误） */
    ESP_ERR_PARAM,     /**< 入参非法 */
} esp_err_t;

/**
 * @brief  初始化驱动：记录移植层、启动串口 DMA 接收。
 * @param  port 移植层回调集合（生命周期覆盖整个运行期）
 * @note   必须在调度器启动前调用（由 esp_net_init 转调）。
 */
void esp_at_init(const esp_port_t *port);

/**
 * @brief  把从 ESP8266 收到的字节喂进驱动环形缓冲。
 * @note   **可在中断里调用**；缓冲满时丢弃新字节。
 */
void esp_at_input(const uint8_t *data, uint16_t len);

/** @brief 丢弃缓冲里所有未读数据（发 AT 指令前先清场，避免读到上一条的残留） */
void esp_at_flush(void);

/**
 * @brief  读取模块发来的数据。
 * @retval 实际读到的字节数；0 表示超时没数据
 */
uint16_t esp_at_read(uint8_t *dst, uint16_t max_len, uint32_t timeout_ms);

/** @brief 直接把裸字节写到串口（不做任何 AT 封装） */
void esp_at_write(const uint8_t *data, uint16_t len);

/** @brief 等待模块的应答文本出现；expect 传 NULL 表示只等 "OK" */
esp_err_t esp_at_expect(const char *expect, uint32_t timeout_ms);

/** @brief 发一条 AT 指令（自动补 \r\n）并等待应答 */
esp_err_t esp_at_cmd(const char *cmd, const char *expect, uint32_t timeout_ms);

/** @brief 发一条"查询类"AT 指令，并把模块的原始应答文本取回来（供调用者 strstr 解析） */
esp_err_t esp_at_query(const char *cmd, char *out, uint16_t out_size, uint32_t timeout_ms);

/** @brief 把一段数据发到当前 TCP 连接上（链路模式自适应） */
esp_err_t esp_at_send(const uint8_t *data, uint16_t len);

/** @brief 进入透传模式（AT+CIPMODE=1 + AT+CIPSEND 等到 ">"） */
esp_err_t esp_at_enter_transparent(void);

/** @brief 退出透传模式（发 "+++"）；重连前必须先调用 */
esp_err_t esp_at_exit_transparent(void);

/** @brief 当前是否处于透传模式 */
uint8_t esp_at_is_transparent(void);

/** @brief 透传模式下对端是否关闭过 TCP（收到过模块吐出的 "CLOSED"） */
uint8_t esp_at_link_closed(void);

/** @brief 清掉断链标志（重新建链前调用） */
void esp_at_clear_link_closed(void);

/**
 * @brief  用使能脚硬复位 ESP8266（拉低 ESP_AT_RESET_LOW_MS 再拉高）。
 * @note   port->reset_pin 为 NULL 时本函数什么都不做。
 */
void esp_at_hw_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP8266_AT_H */