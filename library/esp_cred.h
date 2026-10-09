/**
 * @file    esp_cred.h
 * @brief   WiFi 凭据存储接口（库内部使用，用户无需关心）
 *
 * ---------------------------------------------------------------------------
 * 干什么用的
 * ---------------------------------------------------------------------------
 *   配网（本地 SoftAP 网页 / 云端 MQTT 下发）拿到新的 WiFi 账号密码后，
 *   写到这里保存；下次启动 esp_net.c 先来读，读到有效凭据就用它连网，
 *   读不到就回退到 esp_net_config.h 里的硬编码 WiFi。
 *
 * ---------------------------------------------------------------------------
 * 为什么接口在 library/ 而实现放 port/
 * ---------------------------------------------------------------------------
 *   「存哪儿」是平台相关的（STM32 用内部 Flash 最后一页），所以实现放在
 *   port/esp_cred_<平台>.c；library/ 只认这三个函数，不认识 HAL。
 *   和 esp_port.h 是同一个套路：接口在 library，实现在 port。
 */

#ifndef ESP_CRED_H
#define ESP_CRED_H

#include <stdint.h>
#include "esp_net_config.h"   /* ESP_PROV_SSID_MAX / ESP_PROV_PASS_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 一组 WiFi 凭据（含结尾 '\0'，长度上限见 esp_net_config.h） */
typedef struct
{
    char ssid[ESP_PROV_SSID_MAX];
    char pass[ESP_PROV_PASS_MAX];
} esp_cred_t;

/**
 * @brief  读取已保存的凭据。
 * @param  out  [out] 读到的凭据
 * @retval 0 = 有有效凭据并已填入 out；-1 = 没有 / 参数非法 / 校验不过
 */
int esp_cred_load(esp_cred_t *out);

/**
 * @brief  保存凭据（擦掉存凭据的 Flash 页再写）。
 * @retval 0 = 成功（写完还做了一次读回校验）；-1 = 参数非法或写失败
 */
int esp_cred_save(const char *ssid, const char *pass);

/** @brief 擦除已保存的凭据（下次启动回退到硬编码 WiFi） */
void esp_cred_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_CRED_H */