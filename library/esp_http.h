/**
 * @file    esp_http.h
 * @brief   极简 HTTP/1.1 客户端（库内部使用；WebSocket 握手的地基）
 *
 * ---------------------------------------------------------------------------
 * 它是干嘛的？
 * ---------------------------------------------------------------------------
 *   只做三件最小的事，够 MCU 用、无 malloc、无第三方依赖：
 *     1) 拼一个 HTTP 请求文本（esp_http_build）
 *     2) 从一个 esp_stream_t 上把响应收全（esp_http_recv）
 *     3) 解析状态码 / 取某个响应头（esp_http_status / esp_http_header）
 *
 *   目前唯一的调用者是 esp_ws.c 的 WebSocket 握手；但它本身是通用的，
 *   将来想加 REST（GET / POST 上报数据）直接复用这几个函数即可，不必重写。
 *
 * ---------------------------------------------------------------------------
 * 与 HTTP 里"连接"的关系
 * ---------------------------------------------------------------------------
 *   HTTP 是无状态的：一次请求 = 拼文本 → 发 → 收 → 解析，就完了。
 *   WebSocket 则是"用一次 HTTP 请求把 TCP 升级成双向帧通道"（101 Switching
 *   Protocols），之后就不再是 HTTP 了 —— 那部分在 esp_ws.c 里。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_HTTP_H
#define ESP_HTTP_H

#include <stdint.h>
#include "esp_proto.h"   /* 只用它的 esp_stream_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  拼一个 HTTP/1.1 请求文本（不含请求体）。
 * @param  method        如 "GET" / "POST"
 * @param  path          如 "/mqtt"
 * @param  host          服务器地址（写进 Host 头）
 * @param  port          服务器端口（写进 Host 头）
 * @param  extra_headers 附加头，一整段、每行以 "\r\n" 结尾（可传 NULL）
 * @param  out           输出缓冲
 * @param  out_size      输出缓冲大小
 * @retval >0 = 请求文本长度（字节）；<0 = 参数非法或缓冲不够
 * @note   生成的文本形如：
 *           GET /mqtt HTTP/1.1\r\nHost: 1.2.3.4:8083\r\n<extra_headers>\r\n
 */
int esp_http_build(const char *method, const char *path,
                   const char *host, uint16_t port, const char *extra_headers,
                   char *out, uint16_t out_size);

/**
 * @brief  从字节流上收一个 HTTP 响应头（读到空行 "\r\n\r\n" 为止）。
 * @param  st         底层字节流
 * @param  buf        输出缓冲（会补 '\0' 方便 strstr）
 * @param  buf_size   缓冲大小
 * @param  timeout_ms 单次读取的等待上限
 * @retval >0 = 收到的字节数；<0 = 出错（超时无数据 / 缓冲不够 / 链路错误）
 * @note   只收**响应头**，不处理响应体。WebSocket 握手只需要头。
 */
int esp_http_recv(const esp_stream_t *st, char *buf, uint16_t buf_size,
                  uint32_t timeout_ms);

/**
 * @brief  从响应文本里取状态码。
 * @retval 200 / 101 ...；<0 = 不是合法的 HTTP 状态行
 */
int esp_http_status(const char *resp);

/**
 * @brief  从响应文本里取一个响应头的值（大小写不敏感）。
 * @param  resp      esp_http_recv 收到的响应文本
 * @param  name      头名，如 "Sec-WebSocket-Accept"
 * @param  out       输出缓冲（会补 '\0'）
 * @param  out_size  输出缓冲大小
 * @retval >0 = 值的长度；<0 = 没找到 / 参数非法
 */
int esp_http_header(const char *resp, const char *name,
                    char *out, uint16_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* ESP_HTTP_H */