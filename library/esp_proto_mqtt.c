/**
 * @file    esp_proto_mqtt.c
 * @brief   MQTT 协议实现 —— 把 mqtt_client.c 适配成 esp_proto_t
 *
 * 这一层很薄，只做三件事：
 *   1) 把 esp_stream_t（esp_net 给的字节流）转成 mqtt_transport_t；
 *   2) 把 esp_net 的下行回调转给 mqtt_client 的 publish 回调；
 *   3) 把 mqtt_status_t 映射成 esp_proto 的返回值约定（见 esp_proto.h）。
 *
 * 它**不认识** ESP8266 / AT 指令 / HAL —— 那些都在 esp_stream_t 后面。
 * 本实现只支持一个实例，与 esp_net 当前"同一时刻只跑一个协议"的用法一致。
 *
 * 首发协议栈：MIT
 */

#include "esp_proto_mqtt.h"
#include "mqtt_client.h"
#include "esp_net_config.h"   /* 只为 ESP_LOG */

/* ---------------------------------------------------------------- 实例状态 */

typedef struct
{
    mqtt_client_t       client;
    mqtt_transport_t    transport;
    const esp_stream_t *stream;
    esp_proto_msg_cb_t  userCb;
} esp_proto_mqtt_ctx_t;

static esp_proto_mqtt_ctx_t s_ctx;

/* es_stream_t -> mqtt_transport_t 的转发。
   mqtt_client 只认识这两根函数指针，所以这一层就是它"可移植"的接口点。 */
static int mqtt_transport_write(const uint8_t *data, uint16_t len)
{
    return s_ctx.stream->write(data, len);
}

static int mqtt_transport_read(uint8_t *data, uint16_t max_len, uint32_t timeout_ms)
{
    return s_ctx.stream->read(data, max_len, timeout_ms);
}

/* mqtt_client 的回调签名与 esp_proto_msg_cb_t 完全一致，直接转给用户回调 */
static void mqtt_on_publish(const char *topic, uint16_t topic_len,
                            const uint8_t *payload, uint16_t payload_len)
{
    if (s_ctx.userCb != NULL)
    {
        s_ctx.userCb(topic, topic_len, payload, payload_len);
    }
}

/* ------------------------------------------------------------ esp_proto_t */

static int mqtt_init(void **ctx, const esp_stream_t *st,
                     uint8_t *tx, uint16_t txsz, uint8_t *rx, uint16_t rxsz,
                     esp_proto_msg_cb_t cb)
{
    s_ctx.stream             = st;
    s_ctx.userCb             = cb;
    s_ctx.transport.write    = mqtt_transport_write;
    s_ctx.transport.read     = mqtt_transport_read;

    mqtt_client_init(&s_ctx.client, &s_ctx.transport, tx, txsz, rx, rxsz);
    mqtt_set_publish_callback(&s_ctx.client, mqtt_on_publish);

    *ctx = &s_ctx;
    return 0;
}

static void mqtt_reset(void *ctx)
{
    (void)ctx;
    mqtt_client_reset(&s_ctx.client);
}

static int mqtt_connect_impl(void *ctx, const esp_proto_cfg_t *cfg)
{
    mqtt_status_t st;

    (void)ctx;

    st = mqtt_connect(&s_ctx.client, cfg->client_id, cfg->user, cfg->pass,
                      NULL, NULL, cfg->keepalive_s);
    if (st != MQTT_OK)
    {
        ESP_LOG("[mqtt] CONNECT failed, status %d\r\n", (int)st);
        return -1;
    }
    ESP_LOG("[mqtt] CONNACK ok (client id: %s)\r\n", cfg->client_id);

    if (cfg->sub_topic != NULL)
    {
        st = mqtt_subscribe(&s_ctx.client, cfg->sub_topic, 0U);
        if (st != MQTT_OK)
        {
            ESP_LOG("[mqtt] SUBSCRIBE failed, status %d\r\n", (int)st);
            return -1;
        }
        ESP_LOG("[mqtt] subscribed: %s\r\n", cfg->sub_topic);
    }

    return 0;
}

static int mqtt_publish_impl(void *ctx, const char *topic, const uint8_t *payload,
                             uint16_t payload_len, uint8_t qos, uint8_t retain)
{
    mqtt_status_t st;

    (void)ctx;

    st = mqtt_publish(&s_ctx.client, topic, payload, payload_len, qos, retain);
    if (st == MQTT_OK)
    {
        return 0;
    }
    if ((st == MQTT_ERR_TRANSPORT) || (st == MQTT_ERR_TIMEOUT))
    {
        return -1;   /* 链路问题：让 esp_net 保留这条、重连后再发 */
    }
    return 1;        /* 报文本身有问题（超缓冲等）：丢弃该条，别卡住队列 */
}

static int mqtt_poll_impl(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    /* 传 NULL：收到的 PUBLISH 已由 mqtt_on_publish() 就地转给上层，这里只报状态 */
    return mqtt_poll(&s_ctx.client, NULL, timeout_ms);
}

static int mqtt_ping_impl(void *ctx)
{
    (void)ctx;
    return (mqtt_ping(&s_ctx.client) == MQTT_OK) ? 0 : -1;
}

const esp_proto_t esp_proto_mqtt =
{
    "mqtt",
    mqtt_init,
    mqtt_reset,
    mqtt_connect_impl,
    mqtt_publish_impl,
    mqtt_poll_impl,
    mqtt_ping_impl,
    NULL,   /* disconnect：链路已死时没什么可发，交给重新建链，故留空 */
};