/**
 * @file    esp_ws.h
 * @brief   WebSocket 客户端（库内部使用）—— 在 esp_stream_t 上再包一层 esp_stream_t
 *
 * ---------------------------------------------------------------------------
 * 原理（一句话看懂 WebSocket）
 * ---------------------------------------------------------------------------
 *   WebSocket 不是新端口、也不是新协议栈，它只是"借一次 HTTP 请求把 TCP 升级":
 *
 *     客户端 ──GET /mqtt  Upgrade: websocket  Sec-WebSocket-Key: <随机>──> 服务器
 *     客户端 <──HTTP/1.1 101 Switching Protocols  Sec-WebSocket-Accept:<校验>── 服务器
 *     ── 之后这条 TCP 上跑的就是 WebSocket 帧（二进制/文本/ping/close）──>
 *
 *   所以本模块 = ①做一次 Upgrade 握手（HTTP 部分交给 esp_http.c）
 *               ②握手成功后，把上层给的字节用"帧"包起来发、收到的帧拆开再往上交。
 *
 * ---------------------------------------------------------------------------
 * 它怎么插进现有架构（关键）
 * ---------------------------------------------------------------------------
 *   协议层（MQTT）只认识 esp_stream_t 的 write/read。本模块就**伪装成一个
 *   esp_stream_t**：写的时候做帧封装并加掩码，读的时候拆帧并校验。
 *   于是 esp_net.c 只要把 esp_ws_stream() 的结果交给 MQTT，MQTT 一行都不用改，
 *   它根本不知道底下跑的是 TCP 还是 WebSocket。
 *
 *      esp_net.c ──> esp_ws_stream() ──(帧封装/拆封)──> esp_stream_t ──> ESP8266
 *                                     握手只在建链时做一次
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_WS_H
#define ESP_WS_H

#include <stdint.h>
#include "esp_proto.h"   /* 只用它的 esp_stream_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  绑定底层字节流（通常是 esp_net.c 里那根 TCP 流）。
 * @param  base 底层 write/read；生命周期覆盖整个运行期
 * @note   必须在 esp_net_init() 里、esp_ws_connect() 之前调用一次。
 */
void esp_ws_init(const esp_stream_t *base);

/**
 * @brief  取"包了一层 WebSocket 的字节流"，交给协议实现用。
 * @retval 一个 esp_stream_t*（内部静态实例）；无论握手是否完成都可以先拿到它，
 *         只有 esp_ws_connect() 成功后才真正收发数据。
 */
const esp_stream_t *esp_ws_stream(void);

/**
 * @brief  跟服务器做 WebSocket 握手（HTTP Upgrade）。
 * @param  host       服务器地址
 * @param  port       服务器端口（EMQX 默认 8083）
 * @param  path       路径（EMQX 默认 "/mqtt"）
 * @param  timeout_ms 握手超时
 * @retval 0 = 握手成功（101 + Sec-WebSocket-Accept 校验通过）；-1 = 失败
 * @note   调用前底层 TCP 必须已连上（即 esp_net.c 已经 AT+CIPSTART 成功）。
 */
int esp_ws_connect(const char *host, uint16_t port, const char *path,
                   uint32_t timeout_ms);

/**
 * @brief  清空帧解析状态（链路重建前调用，避免上一轮的残帧污染）。
 */
void esp_ws_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_WS_H */