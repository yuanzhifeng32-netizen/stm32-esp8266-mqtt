# esp-net-framework — ESP8266 联网框架（STM32 / 裸机 / RTOS 通用）

把「串口 + ESP8266 + MQTT」这套联网逻辑封装成一个小库：**你只管把串口收到的字节喂给库，其余联网、断线重连、心跳、收发消息都由库自己搞定**。你自己的任务该干嘛干嘛，需要发数据时调一个 `esp_net_publish()` 就行。

- 库内部**完全不认识 HAL、不认识某个 RTOS**，只通过一个 5 个函数指针的移植层碰硬件；
- 对外**只有 7 个 API**，看一眼 [demo 源码](examples/stm32f103c8t6/Core/Src/demo_app.c) 就会用；
- 自带**断线自愈**：WiFi 掉、broker 关、设备被顶号，都能自己重连回来；
- 上行发布**线程安全**（内部队列），任何任务/中断都能发；
- 下行命令 `名字=数值` 一次注册，在线改变量，不用写解析代码；
- 全中文注释，逻辑分层清晰：**想改业务只看 demo，想改行为只看一个 config 文件**。

> 为什么自己拼 MQTT 报文？板载 ESP8266 的 AT 固件是 v1.2.0.0（2016-07），里面**没有** `AT+MQTTCONN`（要固件 ≥ v1.7.0）。所以 MQTT 3.1.1 跑在 TCP 之上由本库自己实现——反而更可控、不挑固件。

---

## 目录结构

```
.
├─ library/                     ← 框架本体（平台无关，可直接拷进任何工程）
│   ├─ esp_net.h                ★ 用户唯一需要 include 的头文件（7 个 API）
│   ├─ esp_net.c                联网编排：连 WiFi→TCP→MQTT、断线自愈、命令分发
│   ├─ esp_net_config.h         ★ 唯一配置入口：WiFi / broker / 主题 / 缓冲 / 日志
│   ├─ esp_port.h               ★ 移植层接口（库与硬件之间唯一的边界）
│   ├─ esp8266_at.c/.h          内部：ESP8266 AT 指令驱动（透传 + 分帧两种模式）
│   └─ mqtt_client.c/.h         内部：MQTT 3.1.1 报文编解码
│
├─ port/                        ← 各平台移植实现（挑一个抄，或自己写新的）
│   └─ esp_port_stm32f1.c/.h    STM32F1 + HAL + FreeRTOS：USART DMA 循环 + IDLE 中断
│
└─ examples/
    └─ stm32f103c8t6/           ← 完整可跑 demo（CubeMX 工程 + 一键编译/烧录脚本 + 说明.md）
        ├─ Core/Src/demo_app.c  ★ 业务示例：周期上报 + 下行命令（先看这个）
        ├─ build.cmd / flash.cmd
        └─ 说明.md              编译烧录细节 / 排错 FAQ
```

分层边界（照这个读代码就不会乱）：

```
你的业务任务 ──调 API──> esp_net.h ──> esp_net.c ──> esp8266_at / mqtt_client
                                          │
                                          └──调函数指针──> esp_port_t ──> 你的 HAL
```

---

## 快速开始（3 步）

### 1. 改配置

只改 [library/esp_net_config.h](library/esp_net_config.h)：

```c
#define ESP_WIFI_SSID        "你的WiFi"      /* 必须是 2.4 GHz，ESP8266 不支持 5 GHz */
#define ESP_WIFI_PASSWORD    "你的密码"
#define ESP_MQTT_HOST        "192.168.137.1" /* 你的 MQTT 服务器 IP */
#define ESP_MQTT_PORT        1883U
#define ESP_MQTT_CLIENT_ID   "stm32_esp01"   /* 同一 broker 上必须唯一 */
#define ESP_NET_TOPIC_CMD    "stm32/down"    /* 库自动订阅它收下行命令 */
```

### 2. 移植：实现 `esp_port_t` 的 5 个函数，并把串口数据喂给库

参考 [port/esp_port_stm32f1.c](port/esp_port_stm32f1.c)（已实现好，STM32 用户直接用）。核心只有两件事：

```c
/* ① 串口开 DMA 循环接收 + 打开 IDLE 空闲中断 */
HAL_UART_Receive_DMA(&huart2, s_dmaRx, sizeof s_dmaRx);
__HAL_UART_ENABLE_IT(&huart2, UART_IT_IDLE);

/* ② 中断里把「这一批新到的字节」喂给库（库内部是无锁环形缓冲，中断里调用安全） */
void USART2_IRQHandler(void)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_IDLE) != RESET) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart2);
        esp_net_input(new_data_ptr, new_len);   /* ← 就这一句 */
    }
}
```

> 用 **IDLE 空闲中断**而不是 DMA 半满/全满中断：ESP8266 的 AT 应答常常只有几十字节，永远填不满缓冲区；IDLE 中断在「总线安静下来」时触发，短包也能及时喂进库。

### 3. 起两个任务：一个跑网络，一个做你自己的事

```c
/* 调度器启动之前 */
esp_net_init(&esp_port_stm32f1);              /* 绑定移植层 + 启动串口接收 */

osThreadNew(esp_net_task, NULL, &netAttr);    /* ★ 联网任务（内部死循环，不用你管） */

/* 你自己的任务：想干嘛干嘛 */
void my_task(void *arg)
{
    esp_net_var_register("period", &my_period);   /* 以后发 "period=5" 就能改它 */
    esp_net_on_message(my_on_message);            /* 想收下行就注册回调 */

    for (;;) {
        if (esp_net_is_online() && my_period > 0) {
            const char *json = "{\"temp\":25}";
            esp_net_publish("stm32/up", (const uint8_t *)json, strlen(json), 0, 0);
        }
        osDelay(1000);
    }
}
```

完整可运行版本见 [demo_app.c](examples/stm32f103c8t6/Core/Src/demo_app.c)。

---

## API 一览（就这 6 个）

| 函数 | 作用 | 在哪调 |
|---|---|---|
| `void esp_net_init(const esp_port_t *port)` | 绑定移植层、启动串口接收 | 调度器启动**前**调用一次 |
| `void esp_net_input(const uint8_t *data, uint16_t len)` | 把 ESP8266 收到的字节喂给库 | **中断里**（IDLE 空闲中断） |
| `void esp_net_task(void *argument)` | 联网主任务，负责连接 + 断线自愈 | 作为 FreeRTOS 任务入口，**不返回** |
| `int esp_net_publish(topic, payload, len, qos, retain)` | 发布消息（线程安全，内部入队） | 你的任意任务/中断 |
| `void esp_net_on_message(cb)` | 注册下行消息回调 | 你的任务初始化时 |
| `int esp_net_var_register("name", &int32_var)` | 注册可被下行命令修改的变量 | 你的任务初始化时 |
| `uint8_t esp_net_is_online(void)` | 当前是否已连上 broker | 你的任意任务 |

**返回值约定**：`esp_net_publish()` 返回 `0` = 已入队、`-1` = 队列满或参数非法（稍后重试即可）；`esp_net_var_register()` 返回 `-1` = 表满（调大 `ESP_NET_VAR_MAX`）。

### 下行命令格式

往命令主题（`ESP_NET_TOPIC_CMD`）发文本即可，分隔符支持 `;` `,` 空格 换行，数值支持十进制与 `0x` 十六进制：

```
period=5            → 把注册过的 "period" 改成 5
period=5;led=1      → 一次改多个
```

库里会自动：**先按注册表解析并改变量，再把原始消息转发给你的 `on_message` 回调**。

---

## 硬件接线（以 STM32F103C8T6 + ESP-01 为例）

| STM32 | ESP8266 | 说明 |
|---|---|---|
| PA2 (USART2_TX) | RXD | 必须交叉 |
| PA3 (USART2_RX) | TXD | 必须交叉 |
| PA8 | EN / CH_PD | 固件控制使能，**上电必须拉高**，掉线时库会拉低复位 |
| PA9 (USART1_TX) | — | 调试日志 printf 输出（115200） |
| 3V3 | VCC | ESP8266 **必须 3.3 V**，且建议单独供电，峰值电流可到 300 mA+ |
| GND | GND | 共地 |

> 不接 PA8 那根线也行：把 `esp_port_t.reset_pin` 填 `NULL`，库会自动跳过「硬件复位」这一招，只靠 AT 指令恢复。

---

## 移植到别的平台

只做两件事，库本体一行都不用改：

1. **实现 `esp_port_t`**（参考 [esp_port.h](library/esp_port.h) 注释）：
   - `init` —— 串口开 DMA 循环接收 + 打开「收到一批就通知库」的中断
   - `uart_write` —— 发一段字节给 ESP8266
   - `reset_pin` —— 控制使能/复位脚（不接就填 `NULL`）
   - `delay_ms` / `tick_ms` —— 毫秒延时与时基
2. **在串口接收中断里调 `esp_net_input()`** 把字节喂进来。

然后把 `library/` 里的 `.c` 加进编译、`-I` 指向 `library/`，就完了。HTTP、裸机、别的 RTOS 都一样。

---

## 常见问题

**Q：模块的 AT 固件太老，没有 `AT+MQTTCONN`？**
A：这正是本库自己拼 MQTT 报文的理由，任何 AT 固件都能用，不依赖 `AT+MQTTCONN`。

**Q：连不上，日志卡在哪一步？**
A：把 `esp_net_config.h` 里 `ESP_LOG_AT_TRAFFIC` 打开，ESP8266 的原始 AT 应答会全部镜像到日志串口，一眼能看出是卡在 WiFi 还是 TCP。仍连不上的话，打开 demo 里的 `APP_BRIDGE_DEBUG_ENABLE`（见 [demo_config.h](examples/stm32f103c8t6/Core/Inc/demo_config.h)），USART1↔USART2 会变成一根原始透传线，可以用串口助手手工敲 AT 指令定位。

**Q：一直打印 `+++` 退出透传，无限循环？**
A：这是「透传模式下把 AT 应答里的 `CLOSED` 误判成掉线」的经典坑。库已在内部分模式处理（只在透传模式才认 `CLOSED`），并在 MQTT 启动失败时主动退出透传防止叠加。**别把库里的 `esp_at_watch_byte()` 模式判断删掉。**

**Q：发布返回 `-1`？**
A：要么队列满（`ESP_NET_PUB_QUEUE_LEN` 调大，或发送太频繁），要么载荷超长（`ESP_NET_PUB_PAYLOAD_MAX` 调大）。

**Q：RAM 不够用 / 不想用 malloc？**
A：库和 demo **全程零 `malloc`**，所有缓冲都是静态分配（大小都在 `esp_net_config.h` 里）。demo 在 20 KB RAM 的 F103C8T6 上跑 FreeRTOS，两个任务也用 `StaticTask_t` 静态创建。

---

## Demo：STM32F103C8T6 一键跑起来

见 [examples/stm32f103c8t6/说明.md](examples/stm32f103c8t6/说明.md)。最快路径：

```bat
cd examples\stm32f103c8t6
build.cmd          :: 编译，产物在 build\Debug\
flash.cmd          :: ST-Link 烧录（含校验与复位）
```

串口 1（PA9/PA10，115200）会打印联网过程与每次上报，例如：

```
[net] ============== ONLINE ==============
[net] up stm32/up -> {"uptime":10,"seq":1}
[net] down stm32/down = period=5
[net] cmd: period = 5
```

---

## License

[MIT](LICENSE) —— 随便用，随便改，保留版权声明即可。