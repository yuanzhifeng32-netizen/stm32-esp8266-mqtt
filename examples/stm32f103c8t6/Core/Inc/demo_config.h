/**
 * @file    demo_config.h
 * @brief   ★ demo 自己的配置（框架参数在 library/esp_net_config.h）
 *
 * 这里只放"这个示例业务"相关的开关；WiFi / MQTT 服务器等框架参数
 * 都集中在 library/esp_net_config.h，改那里即可。
 *
 * 首发协议栈：MIT
 */

#ifndef DEMO_CONFIG_H
#define DEMO_CONFIG_H

/* ==========================================================================
 * 1. 本示例业务：周期性上报
 * ========================================================================== */
/* 上行主题（订阅的命令主题在 esp_net_config.h 的 ESP_NET_TOPIC_CMD） */
#define DEMO_TOPIC_PUB          "stm32/up"
/* 默认上报周期（秒）。可用下行命令在线改：往 ESP_NET_TOPIC_CMD 发 "period=5"；
   发 "period=0" 就停掉上报。 */
#define DEMO_REPORT_PERIOD_S    10

/* ==========================================================================
 * 2. 备用调试方案（联网任务跑不通时用）
 *
 *   打开后不再跑联网任务，USART1 与 USART2 变成一根**原始双向透传线**：
 *   电脑键盘敲的字符直接进 ESP8266，ESP8266 吐的字节直接上电脑。
 *   用途：用串口助手手工敲 AT 指令定位问题。
 *   注意：1 = 走透传调试；0 = 正常运行 esp_net 联网任务。
 * ========================================================================== */
#define APP_BRIDGE_DEBUG_ENABLE     0

/* ---- 下面是透传调试方案自己的参数，只在上面的开关 = 1 时才编译 ---- */
/* DMA 循环接收缓冲大小（字节），越大越抗突发 */
#define BRIDGE_RX_BUFFER_SIZE       512U
/* 转发任务轮询周期（ms）。512 B @115200 约 44 ms 写满一圈，2 ms 有 20 倍余量 */
#define BRIDGE_POLL_MS              2U
/* 0 = 原样转发（终端直接看文本）；1 = 转成十六进制打印（看二进制协议） */
#define BRIDGE_OUTPUT_HEX           0
/* 1 = 电脑从 USART1 发的字节转给 USART2；0 = 只保留 USART2 → USART1 单向 */
#define BRIDGE_TO_ESP_ENABLE        1
/* 透传调试时的心跳周期（ms），0 = 关闭 */
#define BRIDGE_REPORT_PERIOD_MS     1000U

#endif /* DEMO_CONFIG_H */