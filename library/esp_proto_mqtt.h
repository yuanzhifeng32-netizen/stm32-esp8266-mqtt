/**
 * @file    esp_proto_mqtt.h
 * @brief   MQTT 协议实现（esp_proto_t 的一份实现）
 *
 * 用法：esp_net 默认就用它，无需任何代码；只有在做多协议切换时才需要显式引用：
 *     #include "esp_proto_mqtt.h"
 *     esp_net_set_proto(&esp_proto_mqtt);   // 必须在 esp_net_init() 之前
 *
 * 它是 esp_net.c 与 mqtt_client.c 之间的适配层：对外只暴露 esp_proto_t，
 * 内部把 esp_stream_t 转成 mqtt_transport_t，把 MQTT 状态码映射成 esp_proto 约定。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_PROTO_MQTT_H
#define ESP_PROTO_MQTT_H

#include "esp_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** MQTT 3.1.1 协议实例（esp_net 的默认协议） */
extern const esp_proto_t esp_proto_mqtt;

#ifdef __cplusplus
}
#endif

#endif /* ESP_PROTO_MQTT_H */