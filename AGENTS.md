# AGENTS.md — 维护指南

本文件写给**接手这个仓库的 AI / 人**。目标：5 分钟建立正确认知，知道什么能动、什么绝对不能动、以及怎么验证。

人和 AI 的分工：**业务代码要写得人能看懂（`examples/`、`library/esp_net.h` 里的注释就是说明书）；维护规则要写得 AI 能执行（就是本文件）。**

---

## 1. 认知地图

```
用户业务任务 ──调 API──> library/esp_net.h ──> library/esp_net.c ──> esp8266_at / mqtt_client
                                                    │
                                                    └──调函数指针──> library/esp_port.h ──> port/*.c ──> 某平台的 HAL
```

| 文件 | 职责 | 能不能改 |
|---|---|---|
| `library/esp_net.h` | **唯一对外头文件**，7 个 API + 回调类型 | 加 API 才算改这里，动签名是破坏性变更 |
| `library/esp_net_config.h` | **唯一配置入口**（WiFi/broker/主题/缓冲/超时/日志） | 只加宏，**不要**删或改宏名（用户会依赖） |
| `library/esp_net.c` | 编排：建链时序、在线循环、断线自愈、上行队列、下行命令分发 | 核心，改动前先读 §3 的不变量 |
| `library/esp8266_at.[ch]` | 内部：AT 指令驱动 + 自持环形缓冲 | 内部实现，**不要**把它暴露进 `esp_net.h` |
| `library/mqtt_client.[ch]` | 内部：MQTT 3.1.1 报文编解码 | 同上 |
| `library/esp_port.h` | 移植层接口（5 个函数指针） | 加字段要同步改所有 `port/*.c` |
| `port/esp_port_stm32f1.[ch]` | STM32F1 移植实现 | 平台相关，勿把平台代码混进 `library/` |
| `examples/stm32f103c8t6/` | 可跑 demo（CubeMX 骨架 + `demo_app.c` + `freertos.c`） | 业务示例，随便改 |

**数据流（上行）**：业务任务 → `esp_net_publish()` 拷贝入队 → `net_pump_pub_queue()` 在联网任务里取出 → `mqtt_publish()` → `net_transport_write()` → `esp_at_send()` → 串口 → ESP8266。
**数据流（下行）**：串口 IDLE 中断 → `esp_net_input()` → `esp8266_at` 的环形缓冲 → `esp_at_ring_take()` 里跑 `esp_at_watch_byte()`（掉线检测）→ `mqtt_poll()` 解包 → `net_on_publish()` → 先改变量表，再转用户回调。

---

## 2. 唯一的验证方式（改完必须跑）

```powershell
cd examples\stm32f103c8t6
.\build.ps1                      # 必须 -Wall 无警告
.\flash.ps1                      # 期望 ** Verified OK **
```

抓串口看是否真的联网（换成本机实际 COM 口）：

```powershell
$code = @'
import serial, time, sys
s = serial.Serial('COM20', 115200, timeout=1)
t0 = time.time(); out = b''
while time.time() - t0 < 25: out += s.read(4096)
s.close(); sys.stdout.write(out.decode('utf-8','replace'))
'@
$code | D:\Python\python.exe -
```

通过的判据（缺一不可）：

1. `[net] ============== ONLINE ==============` 出现；
2. `[net] up <topic> -> {...}` 按周期重复；
3. 往命令主题发下行命令，日志出现 `[net] cmd: <名字> = <值>` 且行为确实改变；
4. **停掉 broker** 后只出现一次 `[esp] exit transparent (+++)`，随后按 `ESP_NET_RETRY_MS` 干净重试，**不得出现连续的 `+++`**；
5. **重开 broker** 后自动恢复 ONLINE 并继续上报。

本机测试环境：EMQX 在 `D:\emqx-5.0.8-windows-amd64`（`bin\emqx.cmd start|stop`），Windows 移动热点网关 `192.168.137.1`，MQTT 客户端可用 MQTTX 或 python paho。

> 编译产物的内存基线：**FLASH 37892 B / RAM 14728 B**（Debug）。RAM 已到 71.9%，**新增缓冲/任务前先算内存**。

---

## 3. 硬约束（违反必出 bug，每条都有原因）

| # | 约束 | 为什么 |
|---|---|---|
| 1 | **串口接收用 DMA 循环 + IDLE 空闲中断**，并**关掉 DMA 的 TC/HT 中断**（`__HAL_DMA_DISABLE_IT(...DMA_IT_TC\|DMA_IT_HT\|DMA_IT_TE)`） | ESP8266 的 AT 应答常只有几十字节，永远填不满 DMA 缓冲，靠 HT/TC 会一直收不到 |
| 2 | **串口中断优先级 ≥ 5**（`ESP_PORT_IRQ_PRIO`） | `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`，低于它调 FreeRTOS API 会直接断言 |
| 3 | **任务必须静态分配**（`osThreadAttr_t` 里填 `cb_mem`/`stack_mem`） | `configTOTAL_HEAP_SIZE` 只有 **3072 B**，动态创建立刻耗尽堆 |
| 4 | **`esp_at_watch_byte()` 开头的 `if (s_transparent == 0U) return;` 绝对不能删** | 透传模式之外（如 `AT+CIPCLOSE` 的应答）也会出现 `CLOSED`，误判会导致**无限 `+++` 退出透传**的活锁 |
| 5 | **`library/` 内不得出现任何 HAL / 具体 RTOS 的调用** | 分层边界，一旦混入就无法移植到别的平台 |
| 6 | **不联网时 `esp_net_publish()` 也必须能安全调用** | 它是入队语义，由联网任务在恢复后补发 |
| 7 | **`examples/.../build.ps1` 的 `$Sources` / `$Includes` 是显式清单** | 新增 `.c` 必须手动加进去，否则链接 `undefined reference` |
| 8 | **`build.ps1` / `flash.ps1` 必须保存为 UTF-8 with BOM** | Windows PowerShell 5.1 会按 GBK 解析无 BOM 文件，中文注释直接引发语法错误 |
| 9 | **调 OpenOCD 用相对路径** | OpenOCD 对含中文的绝对路径处理不好 |
| 10 | **`Core/` 里只有 `USER CODE BEGIN/END` 段内的代码是安全的** | 其余部分被 CubeMX 重新生成时覆盖；PA8 配置、`printf` 重定向、任务创建都在 USER CODE 段里 |

---

## 4. 常见修改配方

**加一个对外 API**
1. `library/esp_net.h` 加声明 + 中文注释（含 `@retval`）；2. `library/esp_net.c` 加实现（涉及共享状态的要考虑中断/任务并发）；3. 在 `examples/.../demo_app.c` 里用一次；4. 更新根 `README.md` 的 API 表。

**加一个可被下行命令修改的变量**
1. 调大 `ESP_NET_VAR_MAX`；2. 在业务任务里 `esp_net_var_register("名字", &变量)`（名字必须是持久字符串）；3. 无需在业务里写解析代码。
> 解析规则在 `esp_net.c` 的 `net_parse_i32()` / `net_apply_commands()`：支持 `;` `,` 空格 换行分隔，支持十进制与 `0x` 十六进制、正负号。

**移植到新平台**
1. 新建 `port/esp_port_<平台>.c`，实现 `esp_port_t` 的 5 个函数；2. 在串口接收中断里调 `esp_net_input()`；3. **不要**改 `library/` 的任何文件。

**改 MQTT 主题 / 服务器**
只改 `library/esp_net_config.h`。上行主题由业务在 `esp_net_publish()` 时指定（demo 里是 `demo_config.h` 的 `DEMO_TOPIC_PUB`）。

**加一个业务任务**
在 `freertos.c` 里按现有 `demoTask` 的写法加 `StaticTask_t` + `StackType_t[]` + `osThreadAttr_t` 三件套，**并算好 RAM**。

---

## 5. 禁止事项（踩过的坑）

- ❌ 用 `HAL_UART_Receive()` 轮询接收与 DMA 共存 —— 会 `HAL_BUSY`，且曾与 DMA 接收冲突（历史 bug，已删除该路径）。
- ❌ 用 `AT+MQTTCONN` —— 板载 AT 固件是 **v1.2.0.0（2016-07）**，根本没有这条命令。MQTT 必须是本库自己拼的 3.1.1 报文。
- ❌ 把 `esp8266_at.h` / `mqtt_client.h` include 进 `esp_net.h` —— 内部实现不得泄漏到公开头文件。
- ❌ 在中断里调 `printf` —— newlib 的 stdio 不可重入。
- ❌ 提交 `build/` 目录 —— 已在 `.gitignore` 里，别 `git add -f`。
- ❌ 为了「顺手清理」删掉 `library/` 里的模式判断、超时兜底、队列保留逻辑 —— 每一条都是针对实测故障加的（对应 §3 的 #4、§2 的判据 4）。

---

## 6. 已知未解 / 注意点

- **CMake 流程本机跑不通**：CubeMX 生成的 CMake 工程在 ASM 识别阶段崩溃（`0xC0000409`，与 CMake/Ninja 版本组合有关）。主力编译方式是 `build.ps1`；`CMakeLists.txt` 仅作保留，改动 `build.ps1` 的源清单时**也要同步改 `cmake/stm32cubemx/CMakeLists.txt`**，避免两者长期不一致。
- **`Drivers/` 与 `Middlewares/` 是第三方代码**（ST HAL / CMSIS / FreeRTOS），随仓库提交只为「克隆即可编译」。**不要修改它们**，升级请走 CubeMX。
- **工具链是 GCC 5.2.1**（很老），`STM32F103XX_FLASH.ld` 里的 `(READONLY)` 已被删除、`-mthumb` 已补上。用 CubeMX 重新生成会覆盖这两处，需重新改。
- 上行队列满时 `esp_net_publish()` 返回 `-1` 并丢弃本条，属设计行为（不阻塞业务任务）。