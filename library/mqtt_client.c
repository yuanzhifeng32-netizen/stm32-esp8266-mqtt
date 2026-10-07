/**
 * @file    mqtt_client.c
 * @brief   极简 MQTT 3.1.1 客户端实现（见 mqtt_client.h 的说明）
 *
 * 报文格式速查（固定头 = 1 字节类型/标志 + 1~4 字节"剩余长度"变长整数）：
 *
 *   CONNECT   0x10 | 协议名"MQTT" 版本4 | flags 1B | keepalive 2B | payload(...)
 *   CONNACK   0x20 | 0x02 | 会话标志 1B | 返回码 1B        （返回码 0 = 接受）
 *   PUBLISH   0x30 | 主题(2B长度+内容) [报文ID 2B] | 载荷
 *   SUBSCRIBE 0x82 | 报文ID 2B | 主题(2B长度+内容) | QoS 1B
 *   PINGREQ   0xC0 / PINGRESP 0xD0 / DISCONNECT 0xE0     （都只有固定头 2 字节）
 *
 * 首发协议栈：MIT
 */

#include "mqtt_client.h"

#include <string.h>

/* ------------------------------------------------------------ 报文类型常量 */

#define MQTT_PKT_CONNECT      0x10U
#define MQTT_PKT_CONNACK      0x20U
#define MQTT_PKT_PUBLISH      0x30U
#define MQTT_PKT_PUBACK       0x40U
#define MQTT_PKT_SUBSCRIBE    0x80U
#define MQTT_PKT_SUBACK       0x90U
#define MQTT_PKT_PINGREQ      0xC0U
#define MQTT_PKT_PINGRESP     0xD0U
#define MQTT_PKT_DISCONNECT   0xE0U

/* ---------------------------------------------------------------- 工具函数 */

/** @brief 把 "剩余长度" 编成 MQTT 的变长整数（每字节 7 位，最高位表示还有后续） */
static uint8_t mqtt_encode_length(uint8_t *buf, uint32_t len)
{
    uint8_t n = 0U;

    do
    {
        uint8_t digit = (uint8_t)(len % 128U);
        len /= 128U;
        if (len > 0U)
        {
            digit |= 0x80U;
        }
        buf[n++] = digit;
    } while ((len > 0U) && (n < 4U));

    return n;
}

/**
 * @brief  从缓冲里解析固定头，得出"剩余长度"与固定头总字节数。
 * @retval >0 = 固定头字节数（含类型字节）；0 = 还没收全或数据非法
 */
static uint16_t mqtt_parse_header(const uint8_t *buf, uint16_t len, uint32_t *remaining)
{
    uint32_t value = 0U;
    uint32_t mult  = 1U;
    uint16_t i     = 1U;

    while (i < len)
    {
        uint8_t byte = buf[i];

        value += (uint32_t)(byte & 0x7FU) * mult;
        mult *= 128U;
        i++;

        if ((byte & 0x80U) == 0U)
        {
            *remaining = value;
            return i;
        }
        if (i > 4U)
        {
            return 0U;   /* 变长长度最多 4 字节，超了说明字节流已经错位 */
        }
    }
    return 0U;           /* 长度字段还没收全 */
}

/** @brief 往 buf[*pos] 写一个 16 位大端整数 */
static void mqtt_put_u16(uint8_t *buf, uint16_t *pos, uint16_t value)
{
    buf[(*pos)++] = (uint8_t)(value >> 8);
    buf[(*pos)++] = (uint8_t)(value & 0xFFU);
}

/** @brief 往 buf[*pos] 写"2 字节长度 + 内容"形式的字符串 */
static void mqtt_put_string(uint8_t *buf, uint16_t *pos, const char *str, uint16_t len)
{
    mqtt_put_u16(buf, pos, len);
    memcpy(&buf[*pos], str, len);
    *pos = (uint16_t)(*pos + len);
}

/* ------------------------------------------------------------ 接收层 */

/** @brief 取一批字节追加到接收缓冲；返回读到的字节数，-1 = 缓冲已满/链路错误 */
static int mqtt_read_some(mqtt_client_t *client, uint32_t timeout_ms)
{
    int n;

    if (client->rx_len >= client->rx_size)
    {
        return -1;
    }

    n = client->transport->read(client->rx_buffer + client->rx_len,
                                (uint16_t)(client->rx_size - client->rx_len),
                                timeout_ms);
    if (n > 0)
    {
        client->rx_len = (uint16_t)(client->rx_len + (uint16_t)n);
    }
    return n;
}

/** @brief 从接收缓冲头部丢掉 total 个字节 */
static void mqtt_consume(mqtt_client_t *client, uint16_t total)
{
    if (total >= client->rx_len)
    {
        client->rx_len = 0U;
        return;
    }
    client->rx_len = (uint16_t)(client->rx_len - total);
    memmove(client->rx_buffer, client->rx_buffer + total, client->rx_len);
}

/**
 * @brief  等到接收缓冲里有一个完整报文。
 * @retval  0 = 完整报文就绪（total/hdr/type 已填充）
 *          1 = 超时
 *         -1 = 链路错误 / 报文超过缓冲
 */
static int mqtt_wait_packet(mqtt_client_t *client, uint32_t timeout_ms,
                            uint16_t *total, uint16_t *hdr, uint8_t *type)
{
    uint32_t waited = 0U;

    /* 上一条 PUBLISH 已交付给调用者（零拷贝），这里才真正丢弃 */
    if (client->rx_skip > 0U)
    {
        mqtt_consume(client, client->rx_skip);
        client->rx_skip = 0U;
    }

    for (;;)
    {
        uint32_t remaining = 0U;
        uint16_t h = mqtt_parse_header(client->rx_buffer, client->rx_len, &remaining);

        if (h > 0U)
        {
            uint32_t sum = (uint32_t)h + remaining;

            if (sum > (uint32_t)client->rx_size)
            {
                /* 单个报文比缓冲还大，无法恢复，只能清空重来 */
                mqtt_client_reset(client);
                return -1;
            }
            if (sum <= (uint32_t)client->rx_len)
            {
                *total = (uint16_t)sum;
                *hdr   = h;
                *type  = client->rx_buffer[0];
                return 0;
            }
        }

        if (waited >= timeout_ms)
        {
            return 1;
        }

        {
            uint32_t slice = 100U;
            int n;

            if ((timeout_ms - waited) < slice)
            {
                slice = timeout_ms - waited;
            }
            n = mqtt_read_some(client, slice);
            if (n < 0)
            {
                return -1;
            }
            waited += slice;
        }
    }
}

/** @brief 回一个 PUBACK（仅下行 QoS1 才需要） */
static void mqtt_send_puback(mqtt_client_t *client, uint16_t packet_id)
{
    uint8_t pkt[4];

    pkt[0] = MQTT_PKT_PUBACK;
    pkt[1] = 0x02U;
    pkt[2] = (uint8_t)(packet_id >> 8);
    pkt[3] = (uint8_t)(packet_id & 0xFFU);

    (void)client->transport->write(pkt, (uint16_t)sizeof(pkt));
}

/* ---------------------------------------------------------------- 公开函数 */

void mqtt_client_init(mqtt_client_t *client, const mqtt_transport_t *transport,
                      uint8_t *tx_buffer, uint16_t tx_size,
                      uint8_t *rx_buffer, uint16_t rx_size)
{
    if (client == NULL)
    {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->transport      = transport;
    client->tx_buffer      = tx_buffer;
    client->tx_size        = tx_size;
    client->rx_buffer      = rx_buffer;
    client->rx_size        = rx_size;
    client->next_packet_id = 1U;
}

void mqtt_client_reset(mqtt_client_t *client)
{
    client->rx_len  = 0U;
    client->rx_skip = 0U;
}

void mqtt_set_publish_callback(mqtt_client_t *client, mqtt_publish_cb_t cb)
{
    if (client != NULL)
    {
        client->on_publish = cb;
    }
}

mqtt_status_t mqtt_connect(mqtt_client_t *client, const char *client_id,
                           const char *username, const char *password,
                           const char *will_topic, const char *will_payload,
                           uint16_t keepalive_s)
{
    uint16_t cid_len;
    uint16_t user_len = 0U;
    uint16_t pass_len = 0U;
    uint16_t will_topic_len = 0U;
    uint16_t will_payload_len = 0U;
    uint8_t  flags = 0x02U;   /* bit1 = Clean Session */
    uint32_t payload_len;
    uint32_t remaining;
    uint16_t pos;
    uint32_t total;
    uint16_t total_len = 0U;
    uint16_t hdr_len = 0U;
    uint8_t  type = 0U;
    int      r;

    if ((client == NULL) || (client->transport == NULL) || (client_id == NULL))
    {
        return MQTT_ERR_PARAM;
    }

    /* MQTT-3.1.2-22：密码标志置位时用户名标志必须也置位，否则视为无密码 */
    if ((password != NULL) && (username == NULL))
    {
        password = NULL;
    }

    cid_len     = (uint16_t)strlen(client_id);
    payload_len = 2U + (uint32_t)cid_len;

    if (will_topic != NULL)
    {
        will_topic_len   = (uint16_t)strlen(will_topic);
        will_payload_len = (will_payload != NULL) ? (uint16_t)strlen(will_payload) : 0U;
        flags |= 0x04U;  /* Will Flag；bit3/bit4 = 遗嘱 QoS 0 + 不保留 */
        payload_len += 2U + (uint32_t)will_topic_len + 2U + (uint32_t)will_payload_len;
    }
    if (username != NULL)
    {
        user_len    = (uint16_t)strlen(username);
        flags      |= 0x80U;
        payload_len += 2U + (uint32_t)user_len;
    }
    if (password != NULL)
    {
        pass_len    = (uint16_t)strlen(password);
        flags      |= 0x40U;
        payload_len += 2U + (uint32_t)pass_len;
    }

    /* 变量头固定 10 字节：6(协议名) + 1(版本) + 1(flags) + 2(keepalive) */
    remaining = 10U + payload_len;
    if ((uint32_t)client->tx_size < (5U + remaining))
    {
        return MQTT_ERR_BUFFER;
    }

    /* ---- 固定头 ---- */
    client->tx_buffer[0] = MQTT_PKT_CONNECT;
    pos = (uint16_t)(1U + mqtt_encode_length(&client->tx_buffer[1], remaining));

    /* ---- 变量头 ---- */
    mqtt_put_string(client->tx_buffer, &pos, "MQTT", 4U);
    client->tx_buffer[pos++] = 0x04U;                 /* 协议级别 4 = MQTT 3.1.1 */
    client->tx_buffer[pos++] = flags;
    mqtt_put_u16(client->tx_buffer, &pos, keepalive_s);

    /* ---- 载荷 ---- */
    mqtt_put_string(client->tx_buffer, &pos, client_id, cid_len);
    if (will_topic != NULL)
    {
        mqtt_put_string(client->tx_buffer, &pos, will_topic, will_topic_len);
        mqtt_put_string(client->tx_buffer, &pos, will_payload, will_payload_len);
    }
    if (username != NULL)
    {
        mqtt_put_string(client->tx_buffer, &pos, username, user_len);
    }
    if (password != NULL)
    {
        mqtt_put_string(client->tx_buffer, &pos, password, pass_len);
    }

    if (client->transport->write(client->tx_buffer, pos) != (int)pos)
    {
        return MQTT_ERR_TRANSPORT;
    }

    /* ---- 等 CONNACK ---- */
    r = mqtt_wait_packet(client, MQTT_CONNACK_TIMEOUT_MS, &total_len, &hdr_len, &type);
    if (r < 0)
    {
        return MQTT_ERR_TRANSPORT;
    }
    if (r > 0)
    {
        return MQTT_ERR_TIMEOUT;
    }
    if ((type & 0xF0U) != MQTT_PKT_CONNACK)
    {
        mqtt_consume(client, total_len);
        return MQTT_ERR_PROTOCOL;
    }

    total = (uint32_t)hdr_len + 2U;   /* 需要拿到返回码，即 body[1] */
    if ((uint32_t)client->rx_len < total)
    {
        mqtt_consume(client, total_len);
        return MQTT_ERR_PROTOCOL;
    }

    {
        uint8_t return_code = client->rx_buffer[hdr_len + 1U];

        mqtt_consume(client, total_len);

        if (return_code != 0U)
        {
            client->connected = 0U;
            return MQTT_ERR_REFUSED;
        }
    }

    client->connected = 1U;
    return MQTT_OK;
}

mqtt_status_t mqtt_publish(mqtt_client_t *client, const char *topic,
                           const uint8_t *payload, uint16_t payload_len,
                           uint8_t qos, uint8_t retain)
{
    uint16_t topic_len;
    uint32_t remaining;
    uint16_t pos;
    uint16_t packet_id = 0U;

    if ((client == NULL) || (topic == NULL))
    {
        return MQTT_ERR_PARAM;
    }
    if ((payload == NULL) && (payload_len > 0U))
    {
        return MQTT_ERR_PARAM;
    }
    if (qos > 1U)
    {
        qos = 0U;   /* 这个极简实现只支持 QoS 0/1 */
    }

    topic_len = (uint16_t)strlen(topic);
    remaining = 2U + (uint32_t)topic_len + (uint32_t)payload_len;
    if (qos > 0U)
    {
        remaining += 2U;
    }
    if ((uint32_t)client->tx_size < (5U + remaining))
    {
        return MQTT_ERR_BUFFER;
    }

    client->tx_buffer[0] = (uint8_t)(MQTT_PKT_PUBLISH | ((qos & 0x03U) << 1) | (retain ? 0x01U : 0x00U));
    pos = (uint16_t)(1U + mqtt_encode_length(&client->tx_buffer[1], remaining));

    mqtt_put_string(client->tx_buffer, &pos, topic, topic_len);

    if (qos > 0U)
    {
        packet_id = client->next_packet_id++;
        if (client->next_packet_id == 0U)
        {
            client->next_packet_id = 1U;
        }
        mqtt_put_u16(client->tx_buffer, &pos, packet_id);
    }

    if (payload_len > 0U)
    {
        memcpy(&client->tx_buffer[pos], payload, payload_len);
        pos = (uint16_t)(pos + payload_len);
    }

    if (client->transport->write(client->tx_buffer, pos) != (int)pos)
    {
        return MQTT_ERR_TRANSPORT;
    }

    if (qos == 0U)
    {
        return MQTT_OK;
    }

    /* QoS1：等 PUBACK */
    {
        uint32_t deadline = MQTT_RESPONSE_TIMEOUT_MS;

        while (deadline > 0U)
        {
            uint16_t total_len = 0U;
            uint16_t hdr_len = 0U;
            uint8_t  type = 0U;
            int r = mqtt_wait_packet(client, 100U, &total_len, &hdr_len, &type);

            if (r < 0)
            {
                return MQTT_ERR_TRANSPORT;
            }
            if (r == 0)
            {
                uint8_t hi = (uint8_t)(type & 0xF0U);

                if (hi == MQTT_PKT_PUBACK)
                {
                    mqtt_consume(client, total_len);
                    return MQTT_OK;
                }
                if (hi == MQTT_PKT_PUBLISH)
                {
                    /* 留给 mqtt_poll 处理，别在这里丢掉 */
                    client->rx_skip = total_len;
                    return MQTT_OK;
                }
                mqtt_consume(client, total_len);
            }
            deadline = (deadline > 100U) ? (deadline - 100U) : 0U;
        }
    }
    return MQTT_ERR_TIMEOUT;
}

mqtt_status_t mqtt_subscribe(mqtt_client_t *client, const char *topic, uint8_t qos)
{
    uint16_t topic_len;
    uint32_t remaining;
    uint16_t pos;
    uint16_t packet_id;
    uint32_t deadline;

    if ((client == NULL) || (topic == NULL))
    {
        return MQTT_ERR_PARAM;
    }

    topic_len = (uint16_t)strlen(topic);
    remaining = 2U + 2U + (uint32_t)topic_len + 1U;
    if ((uint32_t)client->tx_size < (5U + remaining))
    {
        return MQTT_ERR_BUFFER;
    }

    packet_id = client->next_packet_id++;
    if (client->next_packet_id == 0U)
    {
        client->next_packet_id = 1U;
    }

    client->tx_buffer[0] = MQTT_PKT_SUBSCRIBE | 0x02U;   /* 固定头低 4 位必须是 0b0010 */
    pos = (uint16_t)(1U + mqtt_encode_length(&client->tx_buffer[1], remaining));

    mqtt_put_u16(client->tx_buffer, &pos, packet_id);
    mqtt_put_string(client->tx_buffer, &pos, topic, topic_len);
    client->tx_buffer[pos++] = (uint8_t)(qos & 0x03U);

    if (client->transport->write(client->tx_buffer, pos) != (int)pos)
    {
        return MQTT_ERR_TRANSPORT;
    }

    deadline = MQTT_RESPONSE_TIMEOUT_MS;
    while (deadline > 0U)
    {
        uint16_t total_len = 0U;
        uint16_t hdr_len = 0U;
        uint8_t  type = 0U;
        int r = mqtt_wait_packet(client, 100U, &total_len, &hdr_len, &type);

        if (r < 0)
        {
            return MQTT_ERR_TRANSPORT;
        }
        if (r == 0)
        {
            uint8_t hi = (uint8_t)(type & 0xF0U);

            if (hi == MQTT_PKT_SUBACK)
            {
                mqtt_consume(client, total_len);
                return MQTT_OK;
            }
            if (hi == MQTT_PKT_PUBLISH)
            {
                client->rx_skip = total_len;   /* 留给 mqtt_poll */
                return MQTT_OK;
            }
            mqtt_consume(client, total_len);
        }
        deadline = (deadline > 100U) ? (deadline - 100U) : 0U;
    }
    return MQTT_ERR_TIMEOUT;
}

mqtt_status_t mqtt_ping(mqtt_client_t *client)
{
    uint8_t  pkt[2];
    uint32_t deadline;

    if ((client == NULL) || (client->transport == NULL))
    {
        return MQTT_ERR_PARAM;
    }

    pkt[0] = MQTT_PKT_PINGREQ;
    pkt[1] = 0x00U;

    if (client->transport->write(pkt, 2U) != 2)
    {
        return MQTT_ERR_TRANSPORT;
    }

    deadline = MQTT_RESPONSE_TIMEOUT_MS;
    while (deadline > 0U)
    {
        uint16_t total_len = 0U;
        uint16_t hdr_len = 0U;
        uint8_t  type = 0U;
        int r = mqtt_wait_packet(client, 100U, &total_len, &hdr_len, &type);

        (void)hdr_len;

        if (r < 0)
        {
            return MQTT_ERR_TRANSPORT;
        }
        if (r == 0)
        {
            uint8_t hi = (uint8_t)(type & 0xF0U);

            if (hi == MQTT_PKT_PINGRESP)
            {
                mqtt_consume(client, total_len);
                return MQTT_OK;
            }
            if (hi == MQTT_PKT_PUBLISH)
            {
                client->rx_skip = total_len;   /* 留给 mqtt_poll */
                return MQTT_OK;
            }
            mqtt_consume(client, total_len);
        }
        deadline = (deadline > 100U) ? (deadline - 100U) : 0U;
    }
    return MQTT_ERR_TIMEOUT;
}

mqtt_status_t mqtt_disconnect(mqtt_client_t *client)
{
    uint8_t pkt[2];

    if ((client == NULL) || (client->transport == NULL))
    {
        return MQTT_ERR_PARAM;
    }

    pkt[0] = MQTT_PKT_DISCONNECT;
    pkt[1] = 0x00U;

    client->connected = 0U;
    return (client->transport->write(pkt, 2U) == 2) ? MQTT_OK : MQTT_ERR_TRANSPORT;
}

int mqtt_poll(mqtt_client_t *client, mqtt_message_t *msg, uint32_t timeout_ms)
{
    if ((client == NULL) || (client->transport == NULL))
    {
        return -1;
    }

    for (;;)
    {
        uint16_t total_len = 0U;
        uint16_t hdr_len = 0U;
        uint8_t  type = 0U;
        uint8_t  hi;
        const uint8_t *body;
        int r = mqtt_wait_packet(client, timeout_ms, &total_len, &hdr_len, &type);

        if (r < 0)
        {
            return -1;
        }
        if (r > 0)
        {
            return 0;   /* 超时，没有业务数据 */
        }

        hi   = (uint8_t)(type & 0xF0U);
        body = &client->rx_buffer[hdr_len];

        switch (hi)
        {
        case MQTT_PKT_PUBLISH:
        {
            uint32_t remaining = (uint32_t)total_len - hdr_len;
            uint16_t topic_len;
            uint16_t offset;
            uint8_t  qos = (uint8_t)((type >> 1) & 0x03U);
            uint16_t packet_id = 0U;

            if (remaining < 2U)
            {
                mqtt_consume(client, total_len);
                break;
            }

            topic_len = (uint16_t)(((uint16_t)body[0] << 8) | (uint16_t)body[1]);
            offset    = (uint16_t)(2U + topic_len);

            if (qos > 0U)
            {
                if (remaining < (uint32_t)offset + 2U)
                {
                    mqtt_consume(client, total_len);
                    break;
                }
                packet_id = (uint16_t)(((uint16_t)body[offset] << 8) | (uint16_t)body[offset + 1U]);
                offset = (uint16_t)(offset + 2U);
            }

            if ((uint32_t)offset > remaining)
            {
                mqtt_consume(client, total_len);
                break;
            }

            if (msg != NULL)
            {
                msg->topic       = (const char *)(body + 2U);
                msg->topic_len   = topic_len;
                msg->payload     = body + offset;
                msg->payload_len = (uint16_t)(remaining - (uint32_t)offset);
                msg->qos         = qos;
                msg->retain      = (uint8_t)(type & 0x01U);
            }

            if (qos == 1U)
            {
                mqtt_send_puback(client, packet_id);
            }

            /* 回调必须在 rx_skip 丢弃之前调用：topic/payload 指向的就是接收缓冲，
               此刻数据还在。回调里可以按主题分发、改业务变量。 */
            if (client->on_publish != NULL)
            {
                client->on_publish((const char *)(body + 2U), topic_len,
                                   (const uint8_t *)(body + offset),
                                   (uint16_t)(remaining - (uint32_t)offset));
            }

            /* 零拷贝：缓冲区先不整理，等下一轮 mqtt_wait_packet 开头再丢 */
            client->rx_skip = total_len;
            return 1;
        }

        case MQTT_PKT_PINGRESP:
        case MQTT_PKT_PUBACK:
        case MQTT_PKT_SUBACK:
            /* 都是无需业务处理的控制报文，直接吃掉 */
            mqtt_consume(client, total_len);
            break;

        default:
            /* 未知/暂不支持的报文：丢弃，避免解析卡死 */
            mqtt_consume(client, total_len);
            break;
        }

        /* 已经处理掉一个控制报文，但还没到超时，继续看有没有 PUBLISH */
    }
}