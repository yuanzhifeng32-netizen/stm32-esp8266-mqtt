/**
 * @file    esp_net.h
 * @brief   ESP8266 联网框架 —— **用户唯一需要 include 的头文件**
 *
 * ---------------------------------------------------------------------------
 * 30 秒上手（完整 demo 见 examples/stm32f103c8t6）
 * ---------------------------------------------------------------------------
 *   1) 改 library/esp_net_config.h：填 WiFi、MQTT 服务器、命令主题；
 *   2) 移植：实现 esp_port_t 里的几个函数（参考 port/esp_port_stm32f1.c），
 *      并在串口接收中断里把收到的字节喂给 esp_net_input()；
 *   3) 启动：调度器启动前调用 esp_net_init(&你的port)，
 *      然后开一个任务跑 esp_net_task()（内部死循环，负责联网与断线自愈）。
 *
 *   之后你自己的任务想干嘛干嘛，需要联网时：
 *       esp_net_publish("stm32/up", payload, len, 0, 0);   // 上行（线程安全）
 *       esp_net_on_message(my_cb);                          // 收下行
 *       esp_net_var_register("period", &period_s);           // 下行 "period=5" 即可改变量
 *
 * 首发协议栈：MIT
 */

#ifndef ESP_NET_H
#define ESP_NET_H

#include <stdint.h>
#include "esp_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  收到一条下行 PUBLISH 时的回调。
 * @warning topic 与 payload **不是**持久内存：它们指向库内部的接收缓冲，
 *          只在回调执行期间有效，回调里必须马上用完，别保存指针。
 *          回调在联网任务的上下文里同步执行。
 */
typedef void (*esp_net_msg_cb_t)(const char *topic, uint16_t topic_len,
                                 const uint8_t *payload, uint16_t payload_len);

/**
 * @brief  初始化：绑定移植层并启动串口接收。
 * @param  port 指向一个全局 const esp_port_t（生命周期要覆盖整个运行期）
 * @note   必须在 FreeRTOS 调度器启动**之前**调用一次（例如 MX_FREERTOS_Init 里）。
 */
void esp_net_init(const esp_port_t *port);

/**
 * @brief  把从 ESP8266 收到的字节喂给库。
 * @note   **可在中断里调用**（内部是无锁的单生产者环形缓冲）。
 *         典型用法：串口 IDLE 空闲中断里，把 DMA 新到的那一段喂进来。
 */
void esp_net_input(const uint8_t *data, uint16_t len);

/**
 * @brief  联网主任务：连 WiFi → 连 TCP → MQTT CONNECT → SUBSCRIBE → 收发保活，
 *         任何一步失败或掉线都会自动回到起点重来。
 * @note   本函数**不返回**（内部死循环），请直接作为 FreeRTOS 任务入口函数。
 */
void esp_net_task(void *argument);

/**
 * @brief  发布一条消息到指定主题（线程安全：内部入队，由联网任务真正发出）。
 * @param  topic    主题，如 "stm32/up"
 * @param  payload  载荷，可为 NULL（当 len = 0）
 * @param  len      载荷长度（上限 ESP_NET_PUB_PAYLOAD_MAX）
 * @param  qos      0 = 发完不管；1 = 等 PUBACK
 * @param  retain   1 = 保留消息
 * @retval 0 = 已入队；-1 = 参数非法 / 队列已满 / 载荷过长（可稍后重试）
 * @note   未联网时也可以调用，消息会在联网后发出；队列满则返回 -1，由调用者决定重试。
 */
int esp_net_publish(const char *topic, const uint8_t *payload, uint16_t len,
                    uint8_t qos, uint8_t retain);

/**
 * @brief  注册"收到下行 PUBLISH 就回调"的处理函数（可多次调用覆盖）。
 * @param  cb 传 NULL = 取消回调。想按主题做自己的业务就注册它。
 * @note   这只是原始消息的转发；如果只是想让下行文本 "名字=数值" 去改某个变量，
 *         用 esp_net_var_register() 更省事，不必写回调。
 */
void esp_net_on_message(esp_net_msg_cb_t cb);

/**
 * @brief  把一个 int32 变量登记到"下行命令"表：
 *         往命令主题（ESP_NET_TOPIC_CMD）发 "名字=数值" 即可改它。
 * @param  name  变量名（如 "period"）。必须是持久字符串（字面量 / 全局数组），
 *               库只存指针不拷贝。
 * @param  value 指向要改的变量。
 * @retval 0 = 登记成功；-1 = 参数非法或表已满（改 ESP_NET_VAR_MAX）
 * @note   用法：static int32_t period; esp_net_var_register("period", &period);
 *         载荷形如 "period=5;led=1"，分隔符支持 ; , 空格 换行，
 *         数值支持十进制与 0x 十六进制（可带正负号）。
 */
int esp_net_var_register(const char *name, int32_t *value);

/**
 * @brief  当前是否已连上 broker（MQTT ONLINE）。
 * @retval 1 = 在线；0 = 未联网 / 正在重连
 */
uint8_t esp_net_is_online(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_NET_H */