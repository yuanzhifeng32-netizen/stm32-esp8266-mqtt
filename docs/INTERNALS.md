# 内部实现与移植详解

> 这份文档给**想改库的人**和**接手维护的 AI**。只想把库用起来，看根目录 [README.md](../README.md) 就够了，不用读这里。
>
> 维护红线 / 验证流程 / 禁止事项在 [AGENTS.md](../AGENTS.md)；改动历史在 [CHANGELOG.md](CHANGELOG.md)。

## 目录

1. [一页原理](#1-一页原理)
2. [分层与文件职责](#2-分层与文件职责)
3. [数据从哪喂进来（移植的关键）](#3-数据从哪喂进来移植的关键)
4. [两种链路模式：透传 vs 分帧](#4-两种链路模式透传-vs-分帧)
5. [断线自愈是怎么做的](#5-断线自愈是怎么做的)
6. [内存布局与基线](#6-内存布局与基线)
7. [常见修改配方](#7-常见修改配方)
8. [FAQ](#8-faq)
9. [已知限制 / 潜在问题](#9-已知限制--潜在问题)

---

## 1. 一页原理

```
        ┌──────────── 上行 ────────────┐
业务任务 ─esp_net_publish()─> 队列 ─> 联网任务 ─s_proto->publish()─> AT 驱动 ─> 串口 ─> ESP8266 ─> broker
 (你的代码)        (拷贝入队，中断里也能调)   (esp_net_task)     (默认 MQTT)

        ┌──────────── 下行 ────────────┐
串口中断 ─esp_net_input()─> 环形缓冲 ─> 联网任务 ─s_proto->poll()─> MQTT 解包
                                                                     │
                                              先按注册表改"名字=数值"变量
                                                                     │
                                                        再转你的 on_message 回调
```

一句话：**库对外只要求两件事** ——
① 有人把串口收到的字节交给 `esp_net_input()`；② 起一个任务跑 `esp_net_task()`。
其余（连 WiFi、连 TCP、MQTT 握手、订阅、心跳、断线重连、命令解析）全在库内部。

---

## 2. 分层与文件职责

| 文件 | 职责 | 改它的规矩 |
|---|---|---|
| `library/esp_net.h` | **唯一对外头文件**：8 个 API + 回调类型 | 只加 API；动签名 = 破坏性变更 |
| `library/esp_net_config.h` | **唯一配置入口**：WiFi / broker / 主题 / 缓冲 / 超时 / 日志 | 只加宏，不改宏名（用户会依赖） |
| `library/esp_net.c` | 编排：建链时序、在线循环、断线自愈、上行队列、下行分发 | 只准调 `esp_proto_t` 表，禁止直接调 `mqtt_*` |
| `library/esp_proto.h` | 协议 vtable（`esp_proto_t`）+ `esp_stream_t` 字节流抽象 | 「换协议栈」的唯一接口点 |
| `library/esp_proto_mqtt.[ch]` | 协议实现：MQTT 3.1.1（包住 `mqtt_client`） | 只认识 `esp_stream_t`，不得 include `esp8266_at.h` |
| `library/esp8266_at.[ch]` | 内部：AT 指令驱动 + 环形缓冲 + 分帧拆包 | 内部实现，不暴露进 `esp_net.h` |
| `library/mqtt_client.[ch]` | 内部：MQTT 3.1.1 报文编解码 | 同上 |
| `library/esp_port.h` | 移植层接口（5 个函数指针） | 加字段要同步改所有 `port/*.c` |
| `port/esp_port_stm32f1.[ch]` | STM32F1 移植实现 | 平台相关，勿混进 `library/` |

**分层铁律**：`library/` 里不出现任何 HAL / 具体 RTOS 调用。所以换 STM32F4 / GD32 / 裸机都只改 `port/`。

---

## 3. 数据从哪喂进来（移植的关键）

**库对"怎么收"完全不做要求**，只要求最终有人调用：

```c
void esp_net_input(const uint8_t *data, uint16_t len);   /* 可在中断里调，无锁 */
```

`esp_net_input()` 在中断里是安全的：内部是**单生产者（中断）→ 单消费者（任务）**环形缓冲，不需要加锁。缓冲满时丢新字节，由上层超时重试。

### 3.1 参考实现：DMA 循环 + IDLE 空闲中断

STM32F1 的实现就在 [port/esp_port_stm32f1.c](../port/esp_port_stm32f1.c)，**中断写在那个文件里**，不在 `Core/Src/stm32f1xx_it.c`（`stm32f1xx_it.c` 里没有 `USART2_IRQHandler` 的同名定义，所以不会重复）：

```
port_init()                        → HAL_UART_Receive_DMA() 开启 DMA 循环接收
                                   → 关掉 DMA 的 TC/HT/TE 中断
                                   → __HAL_UART_ENABLE_IT(UART_IT_IDLE) + NVIC 使能
USART2_IRQHandler()  [port 文件]   → 判 IDLE 标志 → 清标志
                                    → port_pump_from_dma()  取 DMA 新到的一段
                                    → esp_net_input()
```

- DMA 缓冲是 **512 B 循环模式**；`CNDTR` 递减计数，`pos = size - CNDTR` 就是当前写指针，回绕时分两段取。
- **为什么用 IDLE 而不是 DMA 半满/全满中断**：ESP8266 的 AT 应答常常只有几十字节（`OK\r\n`），永远填不满缓冲区，靠 HT/TC 会一直收不到。IDLE 中断在"线路空闲一个字符时间"时触发，短包也能及时喂进库。
- 中断优先级 `ESP_PORT_IRQ_PRIO = 5`，必须 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`，否则调 FreeRTOS API 会直接断言。

### 3.2 支持矩阵：换别的收数据方式怎么接

| 你的接收方式 | 怎么接 | 现成支持？ |
|---|---|---|
| **DMA 循环 + IDLE 中断** | 直接用 `port/esp_port_stm32f1.c` | ✅ 已提供 |
| **只有 IDLE 中断**（不用 DMA，进中断读 DR） | 在 IDLE 分支里把 `USARTx->DR` 读空拼成一段，再调 `esp_net_input()` | ✍️ 自己写，约 10 行 |
| **HAL 回调**（`HAL_UARTEx_RxEventCallback` / `HAL_UART_RxCpltCallback`） | 在回调里直接 `esp_net_input(buf, len)`；若用 `HAL_UARTEx_ReceiveToIdle_DMA()`，`RxEventCallback` 的 `Size` 就是本批长度 | ✍️ 自己写，1 行 |
| **不用中断，纯轮询** | 任务里把每次读到的字节攒成一批，调 `esp_net_input()` | ✍️ 自己写（不推荐，可能丢字节） |

不管用哪种，写完后 **`port_init()` 里不要重复启动接收**，也别让两套机制同时抢同一个串口（历史上 `HAL_UART_Receive()` 轮询和 DMA 混用会 `HAL_BUSY`，已删掉那条路径）。

> 移植层里 `reset_pin` 填 `NULL` 也合法：库会自动跳过"硬件复位"这一招，只靠 AT 指令恢复。

---

## 4. 两种链路模式：透传 vs 分帧

在 `library/esp_net_config.h` 用 `ESP_LINK_MODE` 二选一（编译期）：

| | 透传 `TRANSPARENT`（默认推荐） | 分帧 `FRAME` |
|---|---|---|
| 进链方式 | `AT+CIPMODE=1` + `AT+CIPSEND` 等 `>` | 每次发送 `AT+CIPSEND=<len>` 等 `>` |
| 发送 | 直接写字节（裸 TCP 流） | 先握手等 `>`，再发数据，最后等 `SEND OK` |
| 接收 | 串口上就是裸 TCP 字节流 | 驱动解析 `+IPD,<len>:` 前缀，把 `<len>` 个载荷挑出来 |
| 退出 | 发 `+++` | 不需要 |
| 效率 | 最高 | 多一层握手，慢一点，但每一步能在串口上看清 |
| 适用 | 正式运行 | 排查问题 / 需要可观测性 |

### 4.1 分帧模式的接收拆包状态机

串口上混着两种东西：**AT 应答文本**（`OK` / `>` / `SEND OK`）和 **TCP 载荷**（`+IPD,<len>:` + 数据）。
驱动在**收到字节的那一刻**就分流（`esp8266_at.c` 的 `at_ipd_feed()`），状态机三段：

```
IPD_SEEK  逐字节找 "+IPD,"            → 命中后进 IPD_LEN
IPD_LEN   累积十进制长度，遇到 ':'    → 进 IPD_DATA（长度 0 的空帧忽略）
IPD_DATA  把接下来 <len> 个字节搬进数据缓冲 → 满 len 后回到 IPD_SEEK
```

不是帧头的字节会被当作 AT 文本吐回去（`at_rx_push()`），`s_ipdHold[5]` 暂存"疑似头部"的候选前缀，匹配失败就整段回吐，不会丢字。

**两条独立缓冲**：

- `s_rxRing[ESP_NET_RX_BUFFER_SIZE]` —— 只放 AT 应答文本，`esp_at_read()` / `esp_at_expect()` 用它匹配 `OK` / `>`。
- `s_dataRing[ESP_AT_DATA_BUFFER_SIZE]` —— 只放 TCP 载荷，`esp_at_read_data()` 取走，交给 MQTT 解包。

### 4.2 上行等 `>` 与下行数据为什么不会打架（重点）

用户的担心：「发送数据要等 `>`，这段时间到达的下行数据会不会被吃掉？」

关键有两条：

1. **两条缓冲物理隔离**：等 `>` 时 `esp_at_expect()` 只从 AT 文本缓冲里读；此刻到达的下行 `+IPD` 载荷已经被状态机分流进数据缓冲，**根本没进 AT 缓冲**，所以不会被当成应答文本吃掉。
2. **发 AT 指令时只清 AT 文本缓冲**：`esp_at_cmd()` 开头调用的是 `at_flush_rx()`（只清 `s_rxRing`），**不是** `esp_at_flush()`。后者会连数据缓冲和拆包状态一起清。
   > 这条是实测踩出来的：如果发指令时顺手把数据缓冲清了，那条"已到达但还没轮到 MQTT 解包"的下行消息就被静默丢掉。同理拆包状态机也不能复位——发指令的瞬间可能正好有一帧 `+IPD` 收到一半。
   > `esp_at_flush()`（全清）只留给**重建链路**用：模块复位、退出透传、`esp_at_init()`。

结论：上行握手与下行接收互不干扰；代价只是分帧模式下每次发送要等一个 `>` 往返（`AT+CIPSEND=<len>` 的超时设为 5000 ms）。

---

## 5. 断线自愈是怎么做的

库判断"链路断了"有四个来源，任何一个命中都会回到起点重连（间隔 `ESP_NET_RETRY_MS`）：

| 机制 | 在哪里 | 说明 |
|---|---|---|
| 透传模式下匹配 `CLOSED` | `esp_at_watch_byte()` | 对端关掉 TCP 时模块会退回 AT 模式并吐 `CLOSED`。逐字节状态机匹配，跨多次 read 也能认出来 |
| 发送失败 | `net_pump_pub_queue()` | `s_proto->publish()` 返回 `<0` 视为链路错，触发重连 |
| keepalive 超时 | `net_online_loop()` | 超过周期收不到任何报文就重连 |
| 硬件复位兜底 | `esp_at_hw_reset()` | 拉低使能脚 `ESP_AT_RESET_LOW_MS` 再拉高，状态全清 |

> **`esp_at_watch_byte()` 开头的 `if (s_transparent == 0U) return;` 绝对不能删**。AT 指令模式下 `CLOSED` 是正常应答文本（例如重连时 `AT+CIPCLOSE` 关旧连接就会回一行 `CLOSED`）；不判模式会被误当"对端断了"，导致每轮重连都 `+++` 退透传 —— 就是那个"无限 `+++`"的活锁。

优化点：重连时若 WiFi 还连着，只重开 TCP，不做多余的 WiFi 重连。

---

## 6. 内存布局与基线

全部**静态分配、零 `malloc`**（F103C8T6 只有 20 KB RAM，`configTOTAL_HEAP_SIZE` 仅 3072 B，任务也必须用 `StaticTask_t` 静态创建）。

主要静态缓冲（大小都在 `esp_net_config.h`）：

| 缓冲 | 默认大小 | 用途 |
|---|---|---|
| `ESP_MQTT_TX_BUFFER_SIZE` | 256 B | 拼 MQTT 发送报文 |
| `ESP_MQTT_RX_BUFFER_SIZE` | 256 B | MQTT 收包组帧 |
| `ESP_NET_RX_BUFFER_SIZE` | 512 B | 库接收环形缓冲（AT 应答文本） |
| `ESP_AT_DATA_BUFFER_SIZE` | 512 B | **仅分帧**：TCP 载荷专用缓冲 |
| `ESP_NET_PUB_QUEUE_LEN` × 单条 | 4 × (32+128) | 上行发布队列 |

编译产物基线（Debug）：

| 版本 | FLASH | RAM |
|---|---|---|
| 初始基线 | 37896 B | 14728 B |
| P1 协议解耦 | 38544 B | 14776 B |
| + 分帧接收修复（含 512 B 数据缓冲） | 39444 B | 15312 B (74.77%) |

> RAM 已到 ~75%，**新增缓冲 / 任务前先算内存**。

---

## 7. 常见修改配方

**加一个 `.c` 文件**：写进 `examples/stm32f103c8t6/Makefile.user` 的 `C_SOURCES`（**不要**写进 `Makefile`，它会被 CubeMX 覆盖）。

**加一个对外 API**：`esp_net.h` 加声明 + 注释（含 `@retval`）→ `esp_net.c` 实现（注意中断/任务并发）→ 在 `demo_app.c` 用一次 → 更新根 `README.md` 的 API 表。

**加一个可被下行命令修改的变量**：调大 `ESP_NET_VAR_MAX` → 业务里 `esp_net_var_register("名字", &变量)`（名字必须是持久字符串）。解析规则在 `esp_net.c` 的 `net_parse_i32()` / `net_apply_commands()`：分隔符 `;` `,` 空格 换行，数值支持十进制与 `0x` 十六进制、正负号。

**换 / 加协议栈（例如将来 HTTP）**：
1. 新建 `library/esp_proto_<协议>.c`，实现一份 `esp_proto_t`（照抄 `esp_proto_mqtt.c` 的形状）；
2. 协议里**只准**用 `esp_stream_t` 的 `write` / `read`，**不要** include `esp8266_at.h`；
3. 加进 `Makefile.user` 的 `C_SOURCES`；
4. 使用方在 `esp_net_init()` **之前**调 `esp_net_set_proto(&esp_proto_<协议>)`；
5. `esp_net.c` **一行都不用改**。

> 返回值约定见 `esp_proto.h`：`<0` 一律当链路错误（触发重连）；`publish` 的 `>0` 表示报文本身有问题（丢该条、不断链）。

**移植到新平台**：新建 `port/esp_port_<平台>.c` 实现 `esp_port_t` 的 5 个函数 → 在串口接收处调 `esp_net_input()`（见 §3）→ **不改 `library/` 任何文件**。

---

## 8. FAQ

**Q：连不上，日志卡在哪一步？**
A：打开 `esp_net_config.h` 的 `ESP_LOG_AT_TRAFFIC`，原始 AT 应答会全部镜像到日志串口，一眼看出卡在 WiFi 还是 TCP。还不行就打开 demo 的 `APP_BRIDGE_DEBUG_ENABLE`，USART1↔USART2 变成原始透传线，用串口助手手动敲 AT 定位。

**Q：为什么不用 `AT+MQTTCONN`？**
A：板载 AT 固件是 v1.2.0.0（2016-07），没有这条命令（要 ≥ v1.7.0）。所以 MQTT 3.1.1 跑在 TCP 之上由本库自己拼报文，反而更可控、不挑固件。

**Q：`[mqtt] CONNECT failed, status 3` 是什么？**
A：`status` 是库内部的 `mqtt_status_t`，`3` = `MQTT_ERR_TIMEOUT`，**不是** broker 返回码。分帧模式下出现过，根因是驱动没解析 `+IPD,`（已修复）；若再现，先查 `ESP_LOG_AT_TRAFFIC` 里 `+IPD` 是否被正确拆开。

**Q：一直打印 `+++`，无限循环？**
A：透传模式下把 AT 应答里的 `CLOSED` 误判成掉线。见 §5 的红线，别删 `esp_at_watch_byte()` 的模式判断。

**Q：`esp_net_publish()` 返回 `-1`？**
A：队列满（调大 `ESP_NET_PUB_QUEUE_LEN`）或载荷超长（调大 `ESP_NET_PUB_PAYLOAD_MAX`）。属设计行为，不阻塞业务任务。

**Q：RAM 不够 / 不想用 malloc？**
A：全程零 `malloc`，缓冲大小都在 `esp_net_config.h`；任务用 `StaticTask_t` 静态创建。

---

## 9. 已知限制 / 潜在问题

- **`ESP_LINK_MODE` 是编译期二选一**，没有运行期自动切换。分帧修复后两种模式代码都在，但**最近一次硬件回归只覆盖了分帧模式**，透传模式是等价替换（编译通过）后未重跑硬件。
- **分帧模式下行缓冲 `ESP_AT_DATA_BUFFER_SIZE` 满时丢新字节**。若下行突发流量大，调大它。
- **环形缓冲是单生产者单消费者**。若你想从多个任务同时调 `esp_net_input()`，需要自己加锁——正常用法（只有中断喂数据）不需要。
- **`esp_at_expect()` 的应答累积缓冲 512 B**（`ESP_AT_RESP_BUFFER_SIZE`），超长应答会被截断，仅影响解析，不影响数据通路。
- **构建系统**：`Makefile` + `Makefile.user` + `cubemx-fix.ps1` 三件套。CubeMX 重新生成会覆盖 `Makefile` 和链接脚本，跑 `build.cmd` 会自动修补（细节见 [examples/stm32f103c8t6/README.md](../examples/stm32f103c8t6/README.md) 与 [AGENTS.md](../AGENTS.md) §3 #11）。
- **工具链是 GCC 5.2.1（很老）**：链接脚本里的 `(READONLY)` 已删、`-mthumb` 写在 `Makefile` 的 `MCU` 里；CubeMX 重新生成后由 `cubemx-fix.ps1` 自动补回。