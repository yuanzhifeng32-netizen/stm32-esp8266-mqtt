# esp-net-framework — ESP8266 联网框架（STM32 / 裸机 / RTOS 通用）

把「串口 + ESP8266 + MQTT」封装成一个小库。**你只负责把串口收到的字节喂给库**，联网、断线重连、心跳、收发消息全由库自己搞定；你自己的任务该干嘛干嘛，要发数据就调 `esp_net_publish()`。

> **只改一个文件**：[library/esp_net_config.h](library/esp_net_config.h)（WiFi / broker / 主题 / 缓冲 / 开关）。
> **只看一张表**：[library/esp_net.h](library/esp_net.h) 的 8 个 API。**不看内部逻辑也能用。**

| 想干什么 | 去哪 |
|---|---|
| 用起来 | 本文件（1 分钟读完） |
| 看原理 / 移植 / FAQ / 潜在问题 | [docs/INTERNALS.md](docs/INTERNALS.md) |
| 看改了什么 | [docs/CHANGELOG.md](docs/CHANGELOG.md) |
| 接手改代码（红线 / 验证流程） | [AGENTS.md](AGENTS.md) |

---

## 1 分钟上手（3 步）

### ① 改配置 —— 只改 `library/esp_net_config.h`

```c
#define ESP_WIFI_SSID        "你的WiFi"      /* 必须 2.4 GHz，ESP8266 不支持 5 GHz */
#define ESP_WIFI_PASSWORD    "你的密码"
#define ESP_MQTT_HOST        "192.168.137.1" /* 你的 MQTT 服务器 IP */
#define ESP_MQTT_PORT        1883U
#define ESP_MQTT_CLIENT_ID   "stm32_esp01"   /* 同一 broker 上必须唯一 */
#define ESP_NET_TOPIC_CMD    "stm32/down"    /* 库自动订阅它收下行命令 */
#define ESP_LINK_MODE        ESP_LINK_MODE_TRANSPARENT  /* 或 ESP_LINK_MODE_FRAME */
```

### ② 移植 —— 实现 `esp_port_t` 的 5 个函数，并在串口接收处喂数据

STM32 用户直接用现成的 [port/esp_port_stm32f1.c](port/esp_port_stm32f1.c)。核心就两件事：

```c
/* 中断里把"这一批新到的字节"喂给库（库内部无锁环形缓冲，中断里调用安全） */
void USART2_IRQHandler(void)          /* 中断写在这里，不在 stm32f1xx_it.c */
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_IDLE) != RESET) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart2);
        esp_net_input(new_data_ptr, new_len);   /* ← 就这一句 */
    }
}
```

> 库**不关心**你怎么收，只要求有人调 `esp_net_input()`。参考实现是「DMA 循环 + IDLE 空闲中断」；其它方式怎么接见下表。

### ③ 起任务 —— 一个跑网络，一个做你自己的事

```c
esp_net_init(&esp_port_stm32f1);              /* 调度器启动前，绑定移植层 + 启动接收 */
osThreadNew(esp_net_task, NULL, &netAttr);    /* ★ 联网任务（内部死循环，不用你管） */

void my_task(void *arg)                        /* 你自己的任务：想干嘛干嘛 */
{
    esp_net_var_register("period", &my_period);   /* 之后发 "period=5" 就能改它 */
    esp_net_on_message(my_on_message);            /* 想收下行就注册回调 */
    for (;;) {
        if (esp_net_is_online()) {
            const char *json = "{\"temp\":25}";
            esp_net_publish("stm32/up", (const uint8_t *)json, strlen(json), 0, 0);
        }
        osDelay(1000);
    }
}
```

完整可运行版本见 [demo_app.c](examples/stm32f103c8t6/Core/Src/demo_app.c)。

---

## API 一览（就这 8 个）

| 函数 | 作用 | 在哪调 |
|---|---|---|
| `void esp_net_init(const esp_port_t *port)` | 绑定移植层、启动串口接收 | 调度器启动**前**一次 |
| `void esp_net_set_proto(const esp_proto_t *proto)` | 换协议栈（默认 MQTT） | 可选，`esp_net_init()` **前** |
| `void esp_net_input(const uint8_t *data, uint16_t len)` | 把收到的字节喂给库 | **中断里** |
| `void esp_net_task(void *argument)` | 联网主任务（连接 + 断线自愈） | 作为任务入口，**不返回** |
| `int esp_net_publish(topic, payload, len, qos, retain)` | 发布（线程安全，内部入队） | 任意任务 / 中断 |
| `void esp_net_on_message(cb)` | 注册下行消息回调 | 你的任务初始化时 |
| `int esp_net_var_register("name", &int32_var)` | 注册可被下行命令修改的变量 | 你的任务初始化时 |
| `uint8_t esp_net_is_online(void)` | 当前是否已连上 broker | 任意任务 |

**返回值**：`esp_net_publish()` 返回 `0` = 已入队，`-1` = 队列满 / 参数非法（稍后重试）；`esp_net_var_register()` 返回 `-1` = 表满（调大 `ESP_NET_VAR_MAX`）。

**下行命令**：往 `ESP_NET_TOPIC_CMD` 发文本即可，分隔符 `;` `,` 空格 换行，数值支持十进制与 `0x` 十六进制：

```
period=5            → 把注册过的 "period" 改成 5
period=5;led=1      → 一次改多个
```

库会**先按注册表解析改变量，再把原始消息转发给你的 `on_message` 回调**。

---

## 数据从哪喂进来（关键）

库对接收方式无要求，只要求有人调 `esp_net_input()`：

| 你的接收方式 | 怎么接 | 现成支持？ |
|---|---|---|
| **DMA 循环 + IDLE 空闲中断** | 直接用 [port/esp_port_stm32f1.c](port/esp_port_stm32f1.c) | ✅ 已提供 |
| **只有 IDLE 中断**（不用 DMA） | IDLE 分支里读空 `USARTx->DR` 拼成一段，调 `esp_net_input()` | ✍️ 自己写 |
| **HAL 回调**（`HAL_UARTEx_RxEventCallback` 等） | 回调里 `esp_net_input(buf, len)` | ✍️ 自己写 |
| **不用中断，轮询** | 任务里攒一批再喂 | ✍️ 自己写（不推荐） |

> 为什么参考实现用 IDLE 而不用 DMA 半满中断：ESP8266 的 AT 应答常常只有几十字节（`OK\r\n`），永远填不满 DMA 缓冲，靠 HT/TC 会一直收不到数据。

---

## 移植到别的平台

库本体**一行都不用改**，只做两件事：

1. **实现 `esp_port_t`**（见 [esp_port.h](library/esp_port.h) 注释）：`init` / `uart_write` / `reset_pin`（不接就填 `NULL`）/ `delay_ms` / `tick_ms`。
2. **在串口接收处调 `esp_net_input()`**（见上表）。

然后把 `library/` 里的 `.c` 加进编译、`-I` 指向 `library/`，就完了。

---

## 硬件接线（STM32F103C8T6 + ESP-01）

| STM32 | ESP8266 | 说明 |
|---|---|---|
| PA2 (USART2_TX) | RXD | 必须交叉 |
| PA3 (USART2_RX) | TXD | 必须交叉 |
| PA8 | EN / CH_PD | 固件控制使能，**上电必须拉高**；掉线时库会拉低复位（不接也行，填 `NULL`） |
| PA9 (USART1_TX) | — | 调试日志 printf 输出（115200） |
| 3V3 | VCC | **必须 3.3 V**，建议单独供电，峰值 300 mA+ |
| GND | GND | 共地 |

---

## Demo：STM32F103C8T6 一键跑起来

见 [examples/stm32f103c8t6/README.md](examples/stm32f103c8t6/README.md)。

```bat
cd examples\stm32f103c8t6
build.cmd          :: 编译（Debug），产物在 build\Debug\
flash.cmd          :: ST-Link 烧录（含校验与复位）
```

串口 1（PA9/PA10，115200）会打印联网过程与每次上报：

```
[net] ============== ONLINE ==============
[net] up stm32/up -> {"uptime":10,"seq":1}
[net] down stm32/down = period=5
[net] cmd: period = 5
```

---

## License

[MIT](LICENSE) —— 随便用，随便改，保留版权声明即可。