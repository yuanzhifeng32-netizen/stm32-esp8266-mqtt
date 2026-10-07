/**
 * @file    esp_net.c
 * @brief   联网编排实现（见 esp_net.h 的说明）
 *
 * 状态流（任何一步失败都回到起点重来）：
 *
 *   [上电] → 等模块回 OK → ATE0 → CWMODE=1 → CWJAP 连 WiFi
 *          → CIPMUX=0 → CIPCLOSE(清残留) → CIPSTART 连 broker
 *          → (透传模式) CIPMODE=1 + CIPSEND 进透传
 *          → MQTT CONNECT / CONNACK → SUBSCRIBE / SUBACK
 *          → 在线循环：收下行 + 发上行队列 + PINGREQ 保活
 *
 * 首发协议栈：MIT
 */

#include "esp_net.h"
#include "esp_net_config.h"
#include "esp8266_at.h"
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- 私有数据 */

/* 移植层回调（esp_net_init 时绑定） */
static const esp_port_t *s_port;

/* MQTT 收发缓冲。放静态区而不是栈上：一个任务栈只有 2 KB，塞不下几百字节的报文 */
static uint8_t s_mqttTxBuffer[ESP_MQTT_TX_BUFFER_SIZE];
static uint8_t s_mqttRxBuffer[ESP_MQTT_RX_BUFFER_SIZE];

static mqtt_client_t    s_mqtt;
static mqtt_transport_t s_transport;

/* 是否已连上 broker */
static volatile uint8_t s_online;

/* 用户注册的下行消息回调 */
static esp_net_msg_cb_t s_userCb;

/* ---- 上行发送队列 --------------------------------------------------------
   esp_net_publish() 只是把消息拷进这个队列（很快，任何任务都能调），
   真正发出去由联网任务在线循环里做，避免两个任务同时碰 mqtt_client。 */
typedef struct
{
    char     topic[ESP_NET_PUB_TOPIC_MAX];
    uint8_t  payload[ESP_NET_PUB_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint8_t  retain;
} esp_net_pub_msg_t;

static esp_net_pub_msg_t s_pubQueue[ESP_NET_PUB_QUEUE_LEN];
static volatile uint8_t  s_pubHead;
static volatile uint8_t  s_pubTail;

/* ---- 下行命令的变量注册表 ------------------------------------------------
   往命令主题发 "名字=数值"，回调里就在这里按名字找到变量并改值。
   静态数组，不占堆；用户想加一个可改变量，只需再加一行 esp_net_var_register()。 */
typedef struct
{
    const char *name;
    int32_t    *value;
} esp_net_var_t;

static esp_net_var_t s_vars[ESP_NET_VAR_MAX];
static uint8_t       s_varCount;

/* 统一处理"某一步失败就返回"的样板代码，并在日志串口留下失败点 */
#define NET_CHECK(expr)                                                          \
    do {                                                                         \
        esp_err_t net_check_err = (expr);                                        \
        if (net_check_err != ESP_OK)                                             \
        {                                                                        \
            ESP_LOG("[net] %s -> err %d\r\n", #expr, (int)net_check_err);        \
            return net_check_err;                                                \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------ 传输层适配 */

/* 这两个函数就是整套代码"可移植"的接口点：
   MQTT 客户端只认识这两根函数指针，换成 W5500/4G 模组时只需重写它们。 */

static int net_transport_write(const uint8_t *data, uint16_t len)
{
    if (esp_at_send(data, len) != ESP_OK)
    {
        return -1;
    }
    return (int)len;
}

static int net_transport_read(uint8_t *data, uint16_t max_len, uint32_t timeout_ms)
{
    /* 透传模式下对端关 TCP 时模块会吐 "CLOSED"，这个检测已经在 esp8266_at 驱动里
       用逐字节状态机做好了（跨 read 分片也不会漏），上层用 esp_at_link_closed() 查。 */
    return (int)esp_at_read(data, max_len, timeout_ms);
}

/* ------------------------------------------------------------ 下行命令处理 */

/** @brief 命令片段的分隔符：分号/逗号/空白都算一条命令的结束 */
static uint8_t net_is_sep(uint8_t c)
{
    return (uint8_t)((c == ';') || (c == ',') || (c == ' ') ||
                     (c == '\t') || (c == '\r') || (c == '\n'));
}

/**
 * @brief  解析一个 int32：十进制，或 0x 开头十六进制，可带 + / -。
 * @retval 0 = 成功
 */
static int net_parse_i32(const uint8_t *s, uint16_t len, int32_t *out)
{
    uint16_t i    = 0U;
    int32_t  sign = 1;
    int32_t  val  = 0;
    uint8_t  any  = 0U;

    if ((s == NULL) || (len == 0U))
    {
        return -1;
    }

    if (s[i] == '-')
    {
        sign = -1;
        i++;
    }
    else if (s[i] == '+')
    {
        i++;
    }

    if (((uint16_t)(i + 1U) < len) && (s[i] == '0') &&
        ((s[i + 1U] == 'x') || (s[i + 1U] == 'X')))
    {
        i += 2U;
        for (; i < len; i++)
        {
            uint8_t c = s[i];
            int32_t d;

            if ((c >= '0') && (c <= '9'))      { d = (int32_t)(c - '0'); }
            else if ((c >= 'a') && (c <= 'f')) { d = (int32_t)(c - 'a') + 10; }
            else if ((c >= 'A') && (c <= 'F')) { d = (int32_t)(c - 'A') + 10; }
            else                               { return -1; }

            val = (val * 16) + d;
            any = 1U;
        }
    }
    else
    {
        for (; i < len; i++)
        {
            if ((s[i] < '0') || (s[i] > '9'))
            {
                return -1;
            }
            val = (val * 10) + (int32_t)(s[i] - '0');
            any = 1U;
        }
    }

    if (any == 0U)
    {
        return -1;
    }
    *out = sign * val;
    return 0;
}

/** @brief 按名字查表并改值；返回 1 = 找到并已改，0 = 没有这个变量 */
static uint8_t net_var_apply(const uint8_t *name, uint16_t name_len, int32_t value)
{
    uint8_t k;

    for (k = 0U; k < s_varCount; k++)
    {
        if ((strlen(s_vars[k].name) == name_len) &&
            (memcmp(s_vars[k].name, name, name_len) == 0))
        {
            *s_vars[k].value = value;
            ESP_LOG("[net] cmd: %s = %ld\r\n", s_vars[k].name, (long)value);
            return 1U;
        }
    }
    return 0U;
}

/** @brief 把一段 "名字=数值[;名字=数值...]" 载荷逐段解析并写进变量表 */
static void net_apply_commands(const uint8_t *payload, uint16_t payload_len)
{
    uint16_t i = 0U;

    while (i < payload_len)
    {
        uint16_t name_start;
        uint16_t name_len;
        uint16_t val_len;
        int32_t  value;

        while ((i < payload_len) && (net_is_sep(payload[i]) != 0U))
        {
            i++;
        }
        if (i >= payload_len)
        {
            break;
        }

        name_start = i;
        while ((i < payload_len) && (payload[i] != '=') && (net_is_sep(payload[i]) == 0U))
        {
            i++;
        }
        name_len = (uint16_t)(i - name_start);

        if ((i >= payload_len) || (payload[i] != '=') || (name_len == 0U))
        {
            /* 没有 '=' 的片段不是赋值命令，跳过 */
            while ((i < payload_len) && (net_is_sep(payload[i]) == 0U))
            {
                i++;
            }
            continue;
        }
        i++;   /* 跳过 '=' */

        val_len = 0U;
        while ((i < payload_len) && (net_is_sep(payload[i]) == 0U))
        {
            i++;
            val_len++;
        }

        if (net_parse_i32(&payload[i - val_len], val_len, &value) != 0)
        {
            ESP_LOG("[net] cmd: bad value for %.*s\r\n",
                    (int)name_len, (const char *)&payload[name_start]);
            continue;
        }

        if (net_var_apply(&payload[name_start], name_len, value) == 0U)
        {
            ESP_LOG("[net] cmd: unknown var %.*s\r\n",
                    (int)name_len, (const char *)&payload[name_start]);
        }
    }
}

/**
 * @brief  收到下行 PUBLISH 的回调（由 mqtt_poll 在联网任务里同步调用）。
 *         先处理命令主题，再把原始消息转给用户回调。
 */
static void net_on_publish(const char *topic, uint16_t topic_len,
                           const uint8_t *payload, uint16_t payload_len)
{
    ESP_LOG("\r\n[net] down %.*s = %.*s\r\n",
            (int)topic_len, topic, (int)payload_len, (const char *)payload);

    /* 命令主题：解析 "名字=数值" 去改注册过的变量 */
    if ((topic_len == (uint16_t)strlen(ESP_NET_TOPIC_CMD)) &&
        (memcmp(topic, ESP_NET_TOPIC_CMD, topic_len) == 0))
    {
        net_apply_commands(payload, payload_len);
    }

    /* 用户回调：拿到原始 topic / payload 做自己的业务 */
    if (s_userCb != NULL)
    {
        s_userCb(topic, topic_len, payload, payload_len);
    }
}

/* -------------------------------------------------------------- 建链步骤 */

/** @brief 探活：连发几次 AT，任一应答 OK 就认为模块在 AT 指令模式且活着 */
static esp_err_t net_esp_probe(void)
{
    esp_err_t err = ESP_ERR_TIMEOUT;
    uint8_t   i;

    for (i = 0U; (i < 5U) && (err != ESP_OK); i++)
    {
        s_port->delay_ms(200U);
        err = esp_at_cmd("AT", NULL, ESP_AT_CMD_TIMEOUT_MS);
    }
    return err;
}

/**
 * @brief  把模块拉回 AT 指令模式并确认活着——重连的第一步。
 *
 * 顺序很关键：**先退透传再发 AT**。透传模式下串口上跑的是 TCP 裸字节流，
 * 这时候发 "AT" 只会被当成 TCP 数据丢出去，模块根本不解释。
 */
static esp_err_t net_esp_recover(void)
{
    /* ① 还在透传就先 "+++" 退出来 */
    if (esp_at_is_transparent() != 0U)
    {
        (void)esp_at_exit_transparent();
    }

    /* ② 探活：能应答说明模块在 AT 模式且没问题 */
    if (net_esp_probe() == ESP_OK)
    {
        return ESP_OK;
    }

    /* ③ 怎么问都不应答（多半是断链时卡在透传里了）→ 硬件复位，最后一招 */
    ESP_LOG("[net] ESP8266 not responding -> hardware reset\r\n");
    esp_at_hw_reset();
    s_port->delay_ms(ESP_AT_BOOT_WAIT_MS);   /* 等模块启动完（它会吐一段启动日志） */

    if (net_esp_probe() != ESP_OK)
    {
        ESP_LOG("[net] ESP8266 still dead after reset\r\n");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief  诊断 WiFi 是否还连着（决定要不要重连 WiFi）。
 * @retval 1 = 还连着 AP；0 = 没连 AP / 查询失败
 * @note   用 AT+CWJAP? 而不是 AT+CIPSTATUS：后者的 STATUS 数字含义在不同固件版本里
 *         不一样，而 AT+CWJAP? 连上时会回 "+CWJAP:..."，没连回 "No AP"，无歧义。
 */
static uint8_t net_wifi_connected(void)
{
    char resp[192];

    if (esp_at_query("AT+CWJAP?", resp, (uint16_t)sizeof(resp), 2000U) != ESP_OK)
    {
        return 0U;
    }
    return (uint8_t)((strstr(resp, "+CWJAP:") != NULL) ? 1U : 0U);
}

/** @brief 连 WiFi（只在 WiFi 确实断了时才走这条慢路），成功返回 ESP_OK */
static esp_err_t net_join_wifi(void)
{
    char      cmd[128];
    esp_err_t err;

    NET_CHECK(esp_at_cmd("ATE0", NULL, ESP_AT_CMD_TIMEOUT_MS));       /* 关回显 */
    NET_CHECK(esp_at_cmd("AT+CWMODE=1", NULL, ESP_AT_CMD_TIMEOUT_MS)); /* 1 = Station */

    (void)snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"",
                   ESP_WIFI_SSID, ESP_WIFI_PASSWORD);
    ESP_LOG("[net] joining wifi \"%s\" ...\r\n", ESP_WIFI_SSID);

    err = esp_at_cmd(cmd, "OK", ESP_WIFI_JOIN_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        /* 模块失败时会先打印 "+CWJAP:n"，含义：
             1 = 连接超时   2 = 密码错误   3 = 找不到该 AP   4 = 连接失败
           最常见的 3 就是热点没开，或者热点开在 5 GHz（ESP8266 只支持 2.4 GHz）。 */
        ESP_LOG("[net] wifi join failed, err %d\r\n", (int)err);
        ESP_LOG("       hint: +CWJAP:3 = 找不到热点(未开启/是 5GHz)；2 = 密码错误\r\n");
        return err;
    }

    /* 顺手把拿到的 IP 打出来，好确认和 broker 在同一网段。
       AT+CIFSR 的应答是分几段吐出来的，所以读到连续两次 200 ms 没新数据为止。 */
    esp_at_flush();
    esp_at_write((const uint8_t *)"AT+CIFSR\r\n", 10U);
    {
        uint8_t  ip[128];
        uint16_t total = 0U;
        uint8_t  idle  = 0U;

        while ((idle < 2U) && (total < (uint16_t)(sizeof(ip) - 1U)))
        {
            uint16_t n = esp_at_read(&ip[total],
                                     (uint16_t)(sizeof(ip) - 1U - total), 200U);
            if (n == 0U)
            {
                idle++;
            }
            else
            {
                idle   = 0U;
                total  = (uint16_t)(total + n);
            }
        }
        if (total > 0U)
        {
            ip[total] = '\0';
            ESP_LOG("[net] wifi ok: %s", (const char *)ip);
        }
    }
    return ESP_OK;
}

/** @brief 建 TCP 连接，必要时切进透传模式 */
static esp_err_t net_open_tcp(void)
{
    char cmd[96];

    /* 顺序不能反：先 CIPCLOSE 再 CIPMUX。
       模块里还挂着旧 link 时（半开连接），AT+CIPMUX 会回 "link is builded" + ERROR，
       导致这一次重连直接失败、白白浪费一个重试周期。 */
    (void)esp_at_cmd("AT+CIPCLOSE", NULL, ESP_AT_CMD_TIMEOUT_MS);  /* 有就关，忽略返回值 */

    NET_CHECK(esp_at_cmd("AT+CIPMUX=0", NULL, ESP_AT_CMD_TIMEOUT_MS));

#if (ESP_LINK_MODE != ESP_LINK_MODE_TRANSPARENT)
    NET_CHECK(esp_at_cmd("AT+CIPMODE=0", NULL, ESP_AT_CMD_TIMEOUT_MS));
#endif
    /* 透传模式的 AT+CIPMODE=1 交给 esp_at_enter_transparent() 统一设置 */

    (void)snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u",
                   ESP_MQTT_HOST, (unsigned)ESP_MQTT_PORT);
    ESP_LOG("[net] tcp -> %s:%u\r\n", ESP_MQTT_HOST, (unsigned)ESP_MQTT_PORT);

    NET_CHECK(esp_at_cmd(cmd, "CONNECT", ESP_TCP_CONNECT_TIMEOUT_MS));
    ESP_LOG("[net] tcp connected\r\n");

#if (ESP_LINK_MODE == ESP_LINK_MODE_TRANSPARENT)
    NET_CHECK(esp_at_enter_transparent());
    ESP_LOG("[net] transparent mode on\r\n");
#endif

    return ESP_OK;
}

/** @brief MQTT CONNECT + SUBSCRIBE */
static mqtt_status_t net_mqtt_start(void)
{
    const char   *user = (ESP_MQTT_USERNAME[0] != '\0') ? ESP_MQTT_USERNAME : NULL;
    const char   *pass = (ESP_MQTT_PASSWORD[0] != '\0') ? ESP_MQTT_PASSWORD : NULL;
    mqtt_status_t st;

    st = mqtt_connect(&s_mqtt, ESP_MQTT_CLIENT_ID, user, pass,
                      NULL, NULL, ESP_MQTT_KEEPALIVE_S);
    if (st != MQTT_OK)
    {
        ESP_LOG("[net] mqtt CONNECT failed, status %d\r\n", (int)st);
        return st;
    }
    ESP_LOG("[net] mqtt CONNACK ok (client id: %s)\r\n", ESP_MQTT_CLIENT_ID);

    st = mqtt_subscribe(&s_mqtt, ESP_NET_TOPIC_CMD, 0U);
    if (st != MQTT_OK)
    {
        ESP_LOG("[net] mqtt SUBSCRIBE failed, status %d\r\n", (int)st);
        return st;
    }
    ESP_LOG("[net] mqtt subscribed: %s\r\n", ESP_NET_TOPIC_CMD);

    return MQTT_OK;
}

/**
 * @brief  建链流程（掉线后重连也走这里）；0 = 成功
 *
 * 精确恢复的顺序：
 *   ① 退透传 + 探活（透传里发 AT 没人理，必须先 "+++" 出来）
 *   ② 诊断 WiFi 还在不在
 *        在 → 只重开 TCP（快，~2 s）
 *        不在 → 重连 WiFi 再开 TCP（慢，~10 s）
 *   ③ 重开 TCP + MQTT CONNECT/SUBSCRIBE
 */
static int net_link_up(void)
{
    s_online = 0U;
    mqtt_client_reset(&s_mqtt);
    esp_at_clear_link_closed();

    /* ① 回到 AT 指令模式：先退透传，再探活；卡死才硬复位 */
    if (net_esp_recover() != ESP_OK)
    {
        return -1;
    }
    ESP_LOG("[net] ESP8266 alive\r\n");

    /* ② WiFi 诊断：还在就跳过慢吞吞的 CWJAP */
    if (net_wifi_connected() != 0U)
    {
        ESP_LOG("[net] wifi still up -> reopen TCP only\r\n");
    }
    else
    {
        ESP_LOG("[net] wifi down -> rejoin\r\n");
        if (net_join_wifi() != ESP_OK)
        {
            return -1;
        }
    }

    /* ③ 重开 TCP → 进透传 → MQTT */
    if (net_open_tcp() != ESP_OK)
    {
        return -1;
    }
    if (net_mqtt_start() != MQTT_OK)
    {
        /* MQTT 没起来，但模块此时还停在透传里（TCP 是活的，只是 broker 不理）。
           先 "+++" 退出来，下一轮才能直接发 AT 做诊断；否则状态一直是"透传中"，
           每轮都得先退一次透传，日志上就是反复的 "+++"。 */
        if (esp_at_is_transparent() != 0U)
        {
            (void)esp_at_exit_transparent();
        }
        return -1;
    }

    return 0;
}

/* -------------------------------------------------------------- 在线循环 */

/**
 * @brief  把用户排队的上行消息发出去。
 * @retval 0 = 队列已清空；-1 = 链路出错，需要重连
 */
static int net_pump_pub_queue(void)
{
    while (s_pubHead != s_pubTail)
    {
        esp_net_pub_msg_t *m = &s_pubQueue[s_pubHead];
        mqtt_status_t      st;

        st = mqtt_publish(&s_mqtt, m->topic, m->payload, m->payload_len, m->qos, m->retain);
        if (st == MQTT_OK)
        {
            ESP_LOG("[net] up %s -> %.*s\r\n", m->topic, (int)m->payload_len,
                    (const char *)m->payload);
            s_pubHead = (uint8_t)((s_pubHead + 1U) % ESP_NET_PUB_QUEUE_LEN);
            continue;
        }

        if ((st == MQTT_ERR_TRANSPORT) || (st == MQTT_ERR_TIMEOUT))
        {
            return -1;   /* 链路问题：留着这条，重连后再发 */
        }

        /* 报文本身有问题（超缓冲等）：丢掉，免得卡住队列 */
        ESP_LOG("[net] drop queued pub (%s), status %d\r\n", m->topic, (int)st);
        s_pubHead = (uint8_t)((s_pubHead + 1U) % ESP_NET_PUB_QUEUE_LEN);
    }
    return 0;
}

/** @brief 链路已建立后的收发保活；返回即表示需要重连 */
static void net_online_loop(void)
{
    uint32_t lastPing = s_port->tick_ms();

    s_online = 1U;
    ESP_LOG("\r\n[net] ============== ONLINE ==============\r\n");

    for (;;)
    {
        mqtt_message_t msg;

        /* ---- 1) 收下行数据（等 50 ms，有就立刻处理）---- */
        {
            int r = mqtt_poll(&s_mqtt, &msg, 50U);

            if (r < 0)
            {
                ESP_LOG("\r\n[net] transport error -> reconnect\r\n");
                break;
            }
            /* r > 0 的 PUBLISH 已由 net_on_publish() 回调就地处理 */
        }

        /* ---- 2) 对端把 TCP 关了（驱动逐字节匹配到 "CLOSED"）---- */
        if (esp_at_link_closed() != 0U)
        {
            ESP_LOG("\r\n[net] tcp closed by peer -> reconnect\r\n");
            break;
        }

        /* ---- 3) 把用户排队的上行消息发出去 ---- */
        if (net_pump_pub_queue() != 0)
        {
            ESP_LOG("\r\n[net] publish failed -> reconnect\r\n");
            break;
        }

        /* ---- 4) 保活：定期 PINGREQ，超时就认为链路已死 ---- */
#if (ESP_MQTT_PING_PERIOD_MS > 0U)
        {
            uint32_t now = s_port->tick_ms();

            if ((now - lastPing) >= ESP_MQTT_PING_PERIOD_MS)
            {
                lastPing = now;
                if (mqtt_ping(&s_mqtt) != MQTT_OK)
                {
                    ESP_LOG("\r\n[net] keepalive timeout -> reconnect\r\n");
                    break;
                }
            }
        }
#else
        (void)lastPing;
#endif
    }

    s_online = 0U;
}

/* ---------------------------------------------------------------- 公开函数 */

void esp_net_init(const esp_port_t *port)
{
    s_port = port;

    /* 启动串口接收（DMA 循环 + 空闲中断喂数据） */
    esp_at_init(port);

    s_transport.write = net_transport_write;
    s_transport.read  = net_transport_read;

    mqtt_client_init(&s_mqtt, &s_transport,
                     s_mqttTxBuffer, (uint16_t)sizeof(s_mqttTxBuffer),
                     s_mqttRxBuffer, (uint16_t)sizeof(s_mqttRxBuffer));

    /* 收到 PUBLISH 就进 net_on_publish()：命令主题 → 改变量；再转给用户回调 */
    mqtt_set_publish_callback(&s_mqtt, net_on_publish);

    s_pubHead = 0U;
    s_pubTail = 0U;
    s_online  = 0U;

    ESP_LOG("[net] esp_net_init done\r\n");
}

void esp_net_input(const uint8_t *data, uint16_t len)
{
    esp_at_input(data, len);
}

int esp_net_publish(const char *topic, const uint8_t *payload, uint16_t len,
                    uint8_t qos, uint8_t retain)
{
    uint16_t tlen;
    uint8_t  tail;
    uint8_t  next;

    if ((topic == NULL) || (len > ESP_NET_PUB_PAYLOAD_MAX))
    {
        return -1;
    }
    if ((payload == NULL) && (len > 0U))
    {
        return -1;
    }

    tlen = (uint16_t)strlen(topic);
    if ((tlen == 0U) || (tlen >= ESP_NET_PUB_TOPIC_MAX))
    {
        return -1;
    }

    tail = s_pubTail;
    next = (uint8_t)((tail + 1U) % ESP_NET_PUB_QUEUE_LEN);
    if (next == s_pubHead)
    {
        return -1;   /* 队列满，调用者稍后重试 */
    }

    memcpy(s_pubQueue[tail].topic, topic, tlen);
    s_pubQueue[tail].topic[tlen] = '\0';
    if (len > 0U)
    {
        memcpy(s_pubQueue[tail].payload, payload, len);
    }
    s_pubQueue[tail].payload_len = len;
    s_pubQueue[tail].qos         = qos;
    s_pubQueue[tail].retain      = retain;

    s_pubTail = next;   /* 先填好数据再更新 tail，读者才看得到完整消息 */
    return 0;
}

void esp_net_on_message(esp_net_msg_cb_t cb)
{
    s_userCb = cb;
}

int esp_net_var_register(const char *name, int32_t *value)
{
    if ((name == NULL) || (value == NULL) || (s_varCount >= ESP_NET_VAR_MAX))
    {
        return -1;
    }

    s_vars[s_varCount].name  = name;
    s_vars[s_varCount].value = value;
    s_varCount++;
    return 0;
}

uint8_t esp_net_is_online(void)
{
    return s_online;
}

void esp_net_task(void *argument)
{
    (void)argument;

    for (;;)
    {
        if (net_link_up() == 0)
        {
            net_online_loop();

            /* 走到这里说明链路已断（TCP 都死了）。此时还在透传模式，往串口写 MQTT
               DISCONNECT 只会被丢进已死的 TCP，没有任何意义；重连时 mqtt_client_reset()
               会把客户端状态清干净。所以这里什么都不发，直接进下一轮。 */
        }
        else
        {
            ESP_LOG("\r\n[net] link up failed, retry in %u ms\r\n",
                    (unsigned)ESP_NET_RETRY_MS);
        }

        /* 退透传 / 探活 / 硬复位都在下一轮 net_link_up() 的第一步里做 */
        s_port->delay_ms(ESP_NET_RETRY_MS);
    }
}