/**
 * @file    esp_net_config.h
 * @brief   本框架**唯一**的配置入口 —— 换 WiFi / 换服务器 / 调缓冲 / 开关调试，只改这里
 *
 * ---------------------------------------------------------------------------
 * 分层（照这个边界读代码就不会乱）
 * ---------------------------------------------------------------------------
 *   library/esp_net.h         ← 「用户唯一的头文件」：8 个 API + 回调类型
 *   library/esp_net.c         ← 「联网编排」：连 WiFi→TCP、断线自愈、命令分发（不认识具体协议）
 *   library/esp_proto.h       ← 「协议接口」：换协议栈（MQTT / 将来 HTTP）的接口点
 *   library/esp_proto_mqtt.[ch] ← 「协议实现」：默认的 MQTT 3.1.1
 *   library/esp8266_at.[ch]   ← 「内部」：ESP8266 AT 指令驱动（不对外暴露）
 *   library/mqtt_client.[ch]  ← 「内部」：MQTT 3.1.1 报文编解码（不对外暴露）
 *   library/esp_port.h        ← 「移植层」：库与硬件之间唯一的边界
 *   library/esp_net_config.h  ← 你在这里改参数
 *
 * ---------------------------------------------------------------------------
 * 为什么自己拼 MQTT 报文？
 * ---------------------------------------------------------------------------
 *   板载 ESP8266 的 AT 固件是 v1.2.0.0（2016-07），里面没有 AT+MQTTCONN
 *   （那要固件 >= v1.7.0）。所以 MQTT 跑在 TCP 之上由本库自己实现，反而更可控。
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_NET_CONFIG_H
#define ESP_NET_CONFIG_H

#include <stdio.h>

/* ==========================================================================
 * 1. WiFi 接入点（必须是 2.4 GHz —— ESP8266 不支持 5 GHz）
 * ========================================================================== */
#define ESP_WIFI_SSID               "yzf"
#define ESP_WIFI_PASSWORD           "12345678"
/* 连 AP 最长等待时间；家用路由 DHCP 一般 3~8 s，冷启动留足余量 */
#define ESP_WIFI_JOIN_TIMEOUT_MS    20000U

/* ==========================================================================
 * 2. MQTT 服务器（本机 EMQX 默认监听 1883）
 * ========================================================================== */
#define ESP_MQTT_HOST               "192.168.137.1"
#define ESP_MQTT_PORT               1883U
/* TCP 连接建立超时 */
#define ESP_TCP_CONNECT_TIMEOUT_MS  10000U

/* 客户端标识：同一个 broker 上必须唯一，否则会把前一个连接顶掉 */
#define ESP_MQTT_CLIENT_ID          "stm32_esp01"
/* 用户名 / 密码留空 = 匿名登录（EMQX 默认允许） */
#define ESP_MQTT_USERNAME           ""
#define ESP_MQTT_PASSWORD           ""

/* 命令订阅主题：库会自动订阅它，并把收到的 "名字=数值" 解析后改注册过的变量。
   上行发布主题由用户调用 esp_net_publish() 时自己指定，不在这里配。 */
#define ESP_NET_TOPIC_CMD           "stm32/down"

/* 心跳周期（秒）。broker 在 1.5 倍时间内收不到任何报文就判定掉线 */
#define ESP_MQTT_KEEPALIVE_S        60U
/* 实际发 PINGREQ 的周期，取心跳的一半比较稳妥；0 = 关闭保活 */
#define ESP_MQTT_PING_PERIOD_MS     30000U

/* 整条链路建不起来（WiFi 掉了 / broker 关了）时的重试间隔 */
#define ESP_NET_RETRY_MS            5000U

/* ==========================================================================
 * 2b. 传输方式：直连 TCP，还是 MQTT over WebSocket
 * ========================================================================== */
/* 两者对上层完全一样（都是 MQTT），区别只在"字节怎么送到服务器"：
 *   TCP        ：AT+CIPSTART="TCP" 直连 broker 的 1883（最简单）
 *   WEBSOCKET  ：先跟服务器做一次 HTTP Upgrade 握手，之后在 TCP 上跑带帧格式的
 *                WebSocket 字节流（很多云平台只开放 ws://8083 而不开放裸 1883）。**默认**
 * 只支持**明文** ws:// —— 板载 AT 固件 v1.2.0.0 不支持 TLS socket，wss:// 用不了。 */
#define ESP_MQTT_TRANSPORT_TCP       0
#define ESP_MQTT_TRANSPORT_WEBSOCKET 1

/* 默认走 MQTT over WebSocket（连 ws://8083）。想直连裸 TCP（1883）就改成 ESP_MQTT_TRANSPORT_TCP */
#define ESP_MQTT_TRANSPORT           ESP_MQTT_TRANSPORT_WEBSOCKET

/* WebSocket 端口与路径：EMQX 默认监听 8083，路径 /mqtt。
   host 复用上面的 ESP_MQTT_HOST，这里只管端口和路径。 */
#define ESP_WS_PORT                  8083U
#define ESP_WS_PATH                  "/mqtt"
/* WebSocket 握手（HTTP Upgrade）超时 */
#define ESP_WS_HANDSHAKE_TIMEOUT_MS  5000U

/* WebSocket 相关缓冲（都是静态分配）：
   - TX：发一帧时给载荷做掩码（客户端发出的帧**必须**加掩码）用的临时区
   - RX：拆一帧后攒下的载荷；一帧可能比 MQTT 单次读取大，所以要兜住一整帧
   握手响应复用 RX 缓冲，不额外占用。 */
#define ESP_WS_TX_BUFFER_SIZE        320U
#define ESP_WS_RX_BUFFER_SIZE        512U

/* ==========================================================================
 * 3. 缓冲大小（都是静态分配，按需调大即可）
 * ========================================================================== */
/* MQTT 收发缓冲：一个 CONNECT 报文约 30 B，留 256 B 足够；
   想发更长的 payload（JSON / 固件块）就调大 TX。 */
#define ESP_MQTT_TX_BUFFER_SIZE     256U
#define ESP_MQTT_RX_BUFFER_SIZE     256U

/* 库自持的接收环形缓冲：port 层每收到一批字节就喂进来，库在这里排队等 AT/MQTT 消费。
   建议 >= MQTT_RX_BUFFER_SIZE + 一帧的长度，512 B 足够。 */
#define ESP_NET_RX_BUFFER_SIZE      512U

/* 【仅分帧模式使用】TCP 载荷专用环形缓冲。
   分帧模式下串口上混着两种数据：AT 应答文本（"OK" / ">" / "SEND OK"）和
   TCP 数据（"+IPD,<len>:" 后面跟 <len> 个字节）。驱动会在收到字节时就把两者
   拆开：AT 文本进 ESP_NET_RX_BUFFER_SIZE 那条，TCP 载荷进这一条。
   物理隔离的意义：库发上行时"等 > 提示符"不会把同时到达的下行数据吞掉。
   透传模式不使用本缓冲（那时串口上只有裸 TCP 字节流）。 */
#define ESP_AT_DATA_BUFFER_SIZE     512U

/* 用户调用 esp_net_publish() 时，消息先进这个发送队列，由联网任务排队发出去。
   这样任何任务/中断都能安全发布，不需要加锁。 */
#define ESP_NET_PUB_QUEUE_LEN       4U
/* 单条上行消息的主题长度与载荷长度上限（超出则丢弃并返回 -1） */
#define ESP_NET_PUB_TOPIC_MAX       32U
#define ESP_NET_PUB_PAYLOAD_MAX     128U

/* 下行命令的变量注册表容量（多一个可改变量就 +1） */
#define ESP_NET_VAR_MAX             8U

/* ==========================================================================
 * 4. ESP8266 链路模式（二选一，用 ESP_LINK_MODE 切换）
 * ========================================================================== */
/* 透传模式（推荐）：AT+CIPMODE=1 + AT+CIPSEND 之后，串口上跑的字节就是 TCP 字节流，
   读写都是裸数据，效率最高。退出透传要发 "+++"。 */
#define ESP_LINK_MODE_TRANSPARENT   1
/* 分帧模式：每次发送前 AT+CIPSEND=<len>，等 ">" 再发数据；接收靠 "+IPD,<len>:" 前缀。
   多一层握手、慢，但每一步都能在串口上看见，适合排查问题。 */
#define ESP_LINK_MODE_FRAME         0

#define ESP_LINK_MODE               ESP_LINK_MODE_TRANSPARENT

/* ==========================================================================
 * 5. 超时 / 时序
 * ========================================================================== */
/* 单条 AT 指令的默认超时 */
#define ESP_AT_CMD_TIMEOUT_MS       1000U
/* 发 "+++" 退出透传前 / 后的静默时间（模块要求，不能省） */
#define ESP_AT_ESCAPE_GUARD_MS      20U
/* 硬件复位时 PA8 拉低的时长 */
#define ESP_AT_RESET_LOW_MS         300U
/* 硬件复位后等模块启动完再发 AT */
#define ESP_AT_BOOT_WAIT_MS         1500U

/* ==========================================================================
 * 6. 调试开关（日志默认走 printf → 由移植层决定输出到哪个串口）
 * ========================================================================== */
/* 应用日志总开关：关掉后 printf 全部编掉 */
#define ESP_LOG_ENABLE              1

/* 把 ESP8266 的原始 AT 应答也镜像出来，调试 AT 流程时一眼可见；量产可关 */
#define ESP_LOG_AT_TRAFFIC          1

#if ESP_LOG_ENABLE
#define ESP_LOG(...)                printf(__VA_ARGS__)
#else
#define ESP_LOG(...)                do { } while (0)
#endif

#endif /* ESP_NET_CONFIG_H */