/**
 * @file    esp_prov.h
 * @brief   WiFi 配网模式（库内部使用，用户无需关心）
 *
 * ---------------------------------------------------------------------------
 * 干什么用的
 * ---------------------------------------------------------------------------
 *   被 esp_net_task 在收到"配网请求"（PB6 触发 / 云端下发）时调用：
 *   让 ESP8266 开一个 SoftAP 热点 + 一个极简 HTTP 服务器，手机连热点后
 *   用浏览器打开 192.168.4.1，就能扫描附近 WiFi 并填 SSID/密码。
 *   提交后调用 esp_cred_save() 写凭据，返回 0，由调用方重启生效。
 *
 *   本文件只调 esp8266_at 的 AT 指令，不认识 HAL / MQTT，平台无关。
 */

#ifndef ESP_PROV_H
#define ESP_PROV_H

#include <stdint.h>
#include "esp_net_config.h"   /* ESP_PROV_ENABLE */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  进入配网模式并阻塞服务，直到用户提交凭据或超时。
 * @retval 0  = 已保存凭据（调用方应重启设备生效）
 *         -1 = 超时 / 失败退出（恢复正常联网流程）
 * @note   内部会切到 AP+STA 模式并开 TCP 服务器；返回前会关掉服务器、
 *         把 ESP8266 恢复成 station 单连模式。
 */
int esp_prov_run(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_PROV_H */