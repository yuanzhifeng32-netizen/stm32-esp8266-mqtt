/**
 * @file    esp_proto.h
 * @brief   协议接口（vtable）—— 「换协议栈」的接口点（一般业务无需关心）
 *
 * ---------------------------------------------------------------------------
 * 为什么要有这一层？
 * ---------------------------------------------------------------------------
 *   在它出现之前，esp_net.c 里直接写死了 MQTT：
 *       #include "mqtt_client.h"
 *       mqtt_connect() / mqtt_subscribe() / mqtt_poll() / mqtt_ping() / mqtt_publish()
 *   想换成 HTTP 就要重写 esp_net.c 的建链时序与在线循环。
 *
 *   现在 esp_net.c 只认识下面这张 esp_proto_t 函数表：
 *       esp_net.c ──调 vtable──> esp_proto_mqtt.c ──调 mqtt_xxx──> mqtt_client.c
 *   而协议实现本身也不认识 ESP8266 —— 它只拿到一个 esp_stream_t（write/read），
 *   于是「换协议」= 换一张表，「换硬件」= 换 esp_port_t，两边互不影响。
 *
 *   当前实现：esp_proto_mqtt.c（MQTT 3.1.1，包住 library/mqtt_client.c）。
 *   将来新增 HTTP 只需再实现一份 esp_proto_t，不改 esp_net.c。
 *
 * ---------------------------------------------------------------------------
 * 返回值约定（esp_net.c 依赖它来决定"重连"还是"丢弃这一条"）
 * ---------------------------------------------------------------------------
 *   所有方法： 0 = 成功；< 0 = **链路错误**（esp_net 会跳出在线循环去重连）。
 *   publish 额外用 > 0 表示"报文本身有问题"（如超出缓冲）——丢弃该条但**不断链**，
 *   避免一条坏报文把整个队列卡死（对应 esp_net.c 里 net_pump_pub_queue 的分支）。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_PROTO_H
#define ESP_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  字节流抽象：协议实现唯一的"出口"。
 * @note   与 mqtt_client.h 里的 mqtt_transport_t **同形**，所以适配层只是类型转换，
 *         没有额外开销。换硬件（W5500 / 4G 模组 / LwIP 裸 socket）时重写这两个函数即可。
 */
typedef struct
{
    /** 发送数据，返回实际写出的字节数；<0 表示链路错误 */
    int (*write)(const uint8_t *data, uint16_t len);
    /** 接收数据，返回读到的字节数；0 = 超时无数据；<0 = 链路错误 */
    int (*read)(uint8_t *data, uint16_t max_len, uint32_t timeout_ms);
} esp_stream_t;

/**
 * @brief  建链参数。由 esp_net.c 从 esp_net_config.h 的宏组装后传进来，
 *         这样协议实现不必认识那些宏，ESP_MQTT_HOST 之类的名字也就只留在配置里。
 */
typedef struct
{
    const char *host;        /**< 服务器地址（TCP 连接用） */
    uint16_t    port;        /**< 服务器端口 */
    const char *client_id;   /**< 客户端标识（HTTP 协议可忽略） */
    const char *user;        /**< 用户名，NULL = 匿名 */
    const char *pass;        /**< 密码，NULL = 无 */
    const char *sub_topic;   /**< 要订阅的主题；NULL = 不订阅 */
    uint16_t    keepalive_s; /**< 心跳周期（秒） */
    uint32_t    ping_period_ms; /**< 实际发保活报文的周期；0 = 关闭保活 */
    uint32_t    recv_timeout_ms; /**< 单次 poll 的阻塞上限 */
} esp_proto_cfg_t;

/**
 * @brief  收到一条下行业务消息时的回调（由协议实现转调）。
 * @warning topic 与 payload **不是**持久内存：只在回调执行期间有效，
 *          回调里必须马上用完，别保存指针。回调在联网任务上下文里同步执行。
 */
typedef void (*esp_proto_msg_cb_t)(const char *topic, uint16_t topic_len,
                                   const uint8_t *payload, uint16_t payload_len);

/**
 * @brief  协议实现接口。请用全局 const 实例（形如 esp_proto_mqtt）暴露给 esp_net。
 */
typedef struct esp_proto
{
    /** 名字，仅用于日志，例如 "mqtt" */
    const char *name;

    /**
     * @brief 绑定传输层与收发缓冲，并记下下行回调。
     * @param  ctx  [out] 由实现填充的上下文指针（通常是它内部的静态实例地址）
     * @param  tx/rx 静态收发缓冲（由 esp_net.c 提供，长度 txsz/rxsz）
     * @retval 0 = 成功
     */
    int (*init)(void **ctx, const esp_stream_t *st,
                uint8_t *tx, uint16_t txsz, uint8_t *rx, uint16_t rxsz,
                esp_proto_msg_cb_t cb);

    /** @brief 清空连接状态（链路重建前调用，避免上一轮的残包污染解析） */
    void (*reset)(void *ctx);

    /** @brief 建链（含订阅）；0 = 已就绪 */
    int (*connect)(void *ctx, const esp_proto_cfg_t *cfg);

    /** @brief 发布一条消息；0 = 成功，<0 = 链路错误，>0 = 报文问题（丢弃该条） */
    int (*publish)(void *ctx, const char *topic, const uint8_t *payload,
                   uint16_t payload_len, uint8_t qos, uint8_t retain);

    /** @brief 非阻塞取一条下行消息；>0 = 有业务消息（已回调），0 = 无，<0 = 链路错误 */
    int (*poll)(void *ctx, uint32_t timeout_ms);

    /** @brief 发一次保活报文；0 = 成功（对端还在），<0 = 链路已死 */
    int (*ping)(void *ctx);

    /** @brief 优雅断开（可为 NULL：链路已死时通常没什么可发的） */
    void (*disconnect)(void *ctx);
} esp_proto_t;

#ifdef __cplusplus
}
#endif

#endif /* ESP_PROTO_H */