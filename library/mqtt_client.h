/**
 * @file    mqtt_client.h
 * @brief   极简 MQTT 3.1.1 客户端（报文编解码 + 最小状态机）
 *
 * ---------------------------------------------------------------------------
 * 为什么不用现成的 MQTT 库？
 * ---------------------------------------------------------------------------
 *   这个"协议包"的目标是**能塞进 MCU**：编解码只用几百行、无动态内存、
 *   无第三方依赖，Flash 占用小。功能上覆盖物联网最常用的子集：
 *
 *     CONNECT / CONNACK        建链与鉴权
 *     PUBLISH / PUBACK         上行（QoS 0 和 1）
 *     SUBSCRIBE / SUBACK       下行订阅（QoS 0）
 *     PINGREQ  / PINGRESP      心跳保活
 *     DISCONNECT               优雅断开
 *
 * ---------------------------------------------------------------------------
 * 可移植的关键：传输层是函数指针
 * ---------------------------------------------------------------------------
 *   MQTT 客户端**完全不认识** ESP8266 / STM32 / HAL，它只调用：
 *       transport->write()  /  transport->read()
 *   所以同一份 mqtt_client.c 可以直接挂到 W5500、4G 模组、LwIP 裸 socket 上 ——
 *   只要写两个几行的适配函数即可。这就是这个包"人人可用"的底气。
 *
 * 首发协议栈：MIT
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 等待 CONNACK / PINGRESP / SUBACK 等应答的超时（可通过编译宏覆盖） */
#ifndef MQTT_CONNACK_TIMEOUT_MS
#define MQTT_CONNACK_TIMEOUT_MS     5000U
#endif
#ifndef MQTT_RESPONSE_TIMEOUT_MS
#define MQTT_RESPONSE_TIMEOUT_MS    5000U
#endif

/** 传输层抽象：底层具体是 ESP8266 透传、还是 W5500 socket，由调用者决定 */
typedef struct
{
    /** 发送数据，返回实际写出的字节数；<0 表示链路错误 */
    int (*write)(const uint8_t *data, uint16_t len);
    /** 接收数据，返回读到的字节数；0 = 超时无数据；<0 = 链路错误 */
    int (*read)(uint8_t *data, uint16_t max_len, uint32_t timeout_ms);
} mqtt_transport_t;

/** 一条收到的 PUBLISH 报文 */
typedef struct
{
    const char    *topic;       /**< 指向客户端接收缓冲，非持久，下次 mqtt_poll 之前有效 */
    uint16_t       topic_len;
    const uint8_t *payload;     /**< 同上，非持久 */
    uint16_t       payload_len;
    uint8_t        qos;
    uint8_t        retain;
} mqtt_message_t;

/**
 * 收到 PUBLISH 时的回调（可选）。
 * @warning topic 与 payload **不是**持久内存：它们指向客户端接收缓冲，
 *          只在回调执行期间有效——回调里必须马上用完，别保存指针。
 *          回调在 mqtt_poll() 调用者的任务上下文里同步执行，不需要额外加锁。
 */
typedef void (*mqtt_publish_cb_t)(const char *topic, uint16_t topic_len,
                                  const uint8_t *payload, uint16_t payload_len);

/** MQTT 客户端返回码 */
typedef enum
{
    MQTT_OK = 0,
    MQTT_ERR_PARAM,
    MQTT_ERR_TRANSPORT,   /**< 底层收发失败（链路断了） */
    MQTT_ERR_TIMEOUT,     /**< 等应答超时 */
    MQTT_ERR_PROTOCOL,    /**< 收到了不该收到的报文 */
    MQTT_ERR_REFUSED,     /**< CONNACK 返回码 != 0（被 broker 拒绝） */
    MQTT_ERR_BUFFER,      /**< 报文超过缓冲区 */
} mqtt_status_t;

/** 客户端实例。请用静态变量或全局变量声明，别放栈上（缓冲会吃掉几百字节） */
typedef struct mqtt_client
{
    const mqtt_transport_t *transport;
    uint8_t  *tx_buffer;
    uint16_t  tx_size;
    uint8_t  *rx_buffer;
    uint16_t  rx_size;
    uint16_t  rx_len;        /**< rx_buffer 里已缓存的字节数 */
    uint16_t  rx_skip;       /**< 已交付给调用者、下轮要丢弃的字节数（PUBLISH 零拷贝） */
    uint16_t  next_packet_id;
    uint8_t   connected;
    mqtt_publish_cb_t on_publish;   /**< 收到 PUBLISH 时回调，可为 NULL */
} mqtt_client_t;

/**
 * @brief  绑定传输层与收发缓冲。
 * @param  tx_buffer/tx_size 发送缓冲（拼报文用，建议 >= 256 B）
 * @param  rx_buffer/rx_size 接收缓冲（一次最多能容纳一个完整报文，建议 >= 256 B）
 */
void mqtt_client_init(mqtt_client_t *client, const mqtt_transport_t *transport,
                      uint8_t *tx_buffer, uint16_t tx_size,
                      uint8_t *rx_buffer, uint16_t rx_size);

/** @brief 清空接收缓冲（链路重建后必须调用，否则残包会污染解析） */
void mqtt_client_reset(mqtt_client_t *client);

/**
 * @brief  注册"收到 PUBLISH 就回调"的处理函数（可多次调用覆盖）。
 * @param  cb 传 NULL = 取消回调；回调里改业务变量是本项目"下行命令"的落点。
 * @note   只是存个函数指针，不影响 CONNECT/SUBSCRIBE 等流程。
 */
void mqtt_set_publish_callback(mqtt_client_t *client, mqtt_publish_cb_t cb);

/**
 * @brief  发送 CONNECT 并等待 CONNACK。
 * @param  username/password  传 NULL = 匿名；will_topic/will_payload 传 NULL = 无遗嘱
 * @param  keepalive_s        心跳周期（秒）
 * @retval MQTT_OK 表示 broker 已接受连接
 */
mqtt_status_t mqtt_connect(mqtt_client_t *client, const char *client_id,
                           const char *username, const char *password,
                           const char *will_topic, const char *will_payload,
                           uint16_t keepalive_s);

/**
 * @brief  发布一条消息。
 * @param  qos 0 = 发完不管；1 = 等 PUBACK（超时会返回 MQTT_ERR_TIMEOUT）
 */
mqtt_status_t mqtt_publish(mqtt_client_t *client, const char *topic,
                           const uint8_t *payload, uint16_t payload_len,
                           uint8_t qos, uint8_t retain);

/** @brief 订阅一个主题并等待 SUBACK */
mqtt_status_t mqtt_subscribe(mqtt_client_t *client, const char *topic, uint8_t qos);

/** @brief 发 PINGREQ 并等待 PINGRESP（保活） */
mqtt_status_t mqtt_ping(mqtt_client_t *client);

/** @brief 发 DISCONNECT（不带然后直接断 TCP） */
mqtt_status_t mqtt_disconnect(mqtt_client_t *client);

/**
 * @brief  非阻塞地取一条下行报文（内部会顺手处理 PINGRESP/SUBACK/PUBACK）。
 * @param  msg         收到 PUBLISH 时填充；可以传 NULL 表示不关心内容
 * @param  timeout_ms  最多等多久
 * @retval  1 = msg 已填充（PUBLISH）
 *          0 = 超时/只处理了控制报文，没有业务数据
 *         -1 = 链路错误，需要重连
 */
int mqtt_poll(mqtt_client_t *client, mqtt_message_t *msg, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */