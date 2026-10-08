# AGENTS.md — 维护指南

本文件写给**接手这个仓库的 AI / 人**。目标：5 分钟建立正确认知，知道什么能动、什么绝对不能动、以及怎么验证。

人和 AI 的分工：**业务代码要写得人能看懂（`examples/`、`library/esp_net.h` 里的注释就是说明书）；维护规则要写得 AI 能执行（就是本文件）。**

配套文档：**原理 / 移植 / FAQ / 潜在问题** → `docs/INTERNALS.md`；**改动历史** → `docs/CHANGELOG.md`；**给用户看的 1 分钟速览** → `README.md`。本文件只管「什么能动 / 不能动 / 怎么验证」。

---

## 1. 认知地图

```
用户业务任务 ──调 API──> library/esp_net.h ──> library/esp_net.c ──调 esp_proto_t 表──> esp_proto_mqtt.c ──> mqtt_client
                                                    │   （协议实现只认识 esp_stream_t 的 write/read，不认识 ESP8266）
                                                    ├──> esp8266_at（建链：AT 指令 / 透传 / 分帧）
                                                    └──调函数指针──> library/esp_port.h ──> port/*.c ──> 某平台的 HAL
```

| 文件 | 职责 | 能不能改 |
|---|---|---|
| `library/esp_net.h` | **唯一对外头文件**，8 个 API + 回调类型 | 加 API 才算改这里，动签名是破坏性变更 |
| `library/esp_net_config.h` | **唯一配置入口**（WiFi/broker/主题/传输方式/缓冲/超时/日志） | 只加宏，**不要**删或改宏名（用户会依赖） |
| `library/esp_net.c` | 编排：建链时序、在线循环、断线自愈、上行队列、下行命令分发 | 核心，改动前先读 §3 的不变量；**只调 `esp_proto_t` 表，禁止直接调 `mqtt_*`** |
| `library/esp_proto.h` | 协议接口（vtable）+ `esp_stream_t` 字节流抽象 | 「换协议栈」的唯一接口点；改它要同步改所有协议实现 |
| `library/esp_proto_mqtt.[ch]` | 协议实现：MQTT 3.1.1（默认，包住 `mqtt_client`） | 只认识 `esp_stream_t`，**不得** include `esp8266_at.h`，也不得被暴露进 `esp_net.h` |
| `library/esp_http.[ch]` | 内部：极简 HTTP/1.1 客户端（拼请求 / 收响应 / 解析状态码与头） | 内部实现，WebSocket 握手的地基；通用，可复用做 REST |
| `library/esp_ws.[ch]` | 内部：WebSocket 客户端（HTTP Upgrade 握手 + 帧封/拆），**在 `esp_stream_t` 外再包一层** | 内部实现，**不得**暴露进 `esp_net.h`；不得 include `esp8266_at.h` |
| `library/esp8266_at.[ch]` | 内部：AT 指令驱动 + 环形缓冲；分帧模式在此解析 `+IPD,<len>:`（AT 文本与 TCP 载荷分两条缓冲） | 内部实现，**不要**把它暴露进 `esp_net.h` |
| `library/mqtt_client.[ch]` | 内部：MQTT 3.1.1 报文编解码 | 同上 |
| `library/esp_port.h` | 移植层接口（5 个函数指针） | 加字段要同步改所有 `port/*.c` |
| `port/esp_port_stm32f1.[ch]` | STM32F1 移植实现（**`USART2_IRQHandler` 在这里**） | 平台相关，勿把平台代码混进 `library/` |
| `docs/INTERNALS.md` | 原理 / 移植 / FAQ / 潜在问题（给想深入的人） | 随代码更新 |
| `docs/CHANGELOG.md` | 改动历史（改了什么 / 为什么 / 验证 / 内存） | **每次实质改动追加一条** |
| `examples/stm32f103c8t6/` | 可跑 demo（CubeMX 骨架 + `demo_app.c` + `freertos.c`） | 业务示例，随便改 |
| `examples/.../Makefile` | 构建主体（CubeMX「Makefile」工具链的形状） | 可由 CubeMX 重新生成覆盖 |
| `examples/.../Makefile.user` | **我们自加的源文件与包含路径 + 头文件依赖** | 新增 `.c` / `-I` 写这里；末尾三处不可删（见 §3 #11），不受 CubeMX 影响 |
| `examples/.../cubemx-fix.ps1` | CubeMX 重新生成后的自动修补（`build.cmd` 会调） | 靠锚点行 `vpath %.c` 定位，改锚点前先确认 CubeMX 模板没变 |

**数据流（上行）**：业务任务 → `esp_net_publish()` 拷贝入队 → `net_pump_pub_queue()` 在联网任务里取出 → `s_proto->publish()`（默认 `esp_proto_mqtt.c`）→ `net_transport_write()` → `esp_at_send()` → 串口 → ESP8266。
**数据流（下行）**：串口 IDLE 中断 → `esp_net_input()` → `esp8266_at` 的环形缓冲（分帧模式先经 `at_ipd_feed()` 把 `+IPD` 载荷分流到数据缓冲）→ `esp_at_ring_take()` 里跑 `esp_at_watch_byte()`（掉线检测）→ `s_proto->poll()` 解包（默认 MQTT，走 `esp_at_read_data()`）→ `net_on_publish()` → 先改变量表，再转用户回调。

---

## 2. 唯一的验证方式（改完必须跑）

```powershell
cd examples\stm32f103c8t6
build.cmd                        # 或 mingw32-make -j8；必须 -Wall 无警告
mingw32-make flash               # 编译并烧录，期望 ** Verified OK **
```

> **刚用 CubeMX 重新生成过代码**：先跑 `build.cmd`（它内部会先执行 `cubemx-fix.ps1`，
> 自动补回被覆盖的 `-include Makefile.user` 与链接脚本的 `(READONLY)`）。若绕过
> `build.cmd` 直接用 `mingw32-make`，请先手动 `powershell -File .\cubemx-fix.ps1` 一次，
> 否则链接报 `undefined reference to esp_net_*`。

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

> 编译产物的内存基线：**Debug 默认配置（透传链路 + MQTT over WebSocket）：FLASH 44572 B / RAM 15632 B（76.33%）**；改 `ESP_MQTT_TRANSPORT = ESP_MQTT_TRANSPORT_TCP` 直连 1883 时为 **FLASH 38752 B / RAM 14792 B（72.23%）**。RAM 已到 ~76%，**新增缓冲/任务前先算内存**。
> （历史：初始 37896/14728 → P1 协议解耦 38544/14776 → 分帧修复 39444/15312 → P2 MQTT over WebSocket 见上，详见 `docs/CHANGELOG.md`。）

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
| 7 | **新增 `.c` 必须写进 `examples/.../Makefile.user`**，不要写进 `Makefile` | 漏加会链接 `undefined reference`；而写进 `Makefile` 的内容会在下次 CubeMX 重新生成时静默丢失 |
| 8 | **`flash.ps1` 必须保存为 UTF-8 with BOM**；`Makefile` / `.cmd` **必须不带 BOM** | PowerShell 5.1 按 GBK 解析无 BOM 的 `.ps1`，中文注释直接语法错误；反过来 BOM 会被 `make` / `cmd` 当成内容，导致 "missing separator" 之类错误 |
| 9 | **调 OpenOCD 用相对路径** | OpenOCD 对含中文的绝对路径处理不好 |
| 10 | **`Core/` 里只有 `USER CODE BEGIN/END` 段内的代码是安全的** | 其余部分被 CubeMX 重新生成时覆盖；PA8 配置、`printf` 重定向、任务创建都在 USER CODE 段里 |
| 11 | **`Makefile.user` 末尾三处不可删**：`%.o: CFLAGS += -MMD -MP`、`.DEFAULT_GOAL := all`、`-include $(wildcard $(BUILD_DIR)/*.d)` | ① 不加 `-MMD`：改 `.h`（最常改的 `esp_net_config.h`）不触发重编，会静默烧进旧固件；② 不设 `.DEFAULT_GOAL`：`Makefile.user` 在 `all:` 之前被 include，`.d` 里第一条规则会抢走默认目标，`mingw32-make` 从此不再执行 `all`（只打印某 `.o` is up to date）；③ 不 include `.d`：前两条都白做 |
| 12 | **发 AT 指令只准用 `at_flush_rx()`，禁止用 `esp_at_flush()`**（`esp8266_at.c`）；`esp_at_flush()` 只留给重建链路（复位 / 退透传 / init） | 分帧模式下 `esp_at_flush()` 会连 TCP 载荷缓冲和拆包状态一起清。发送要等 `>`，等待期间到达的下行数据已在数据缓冲里，被清掉 = 静默丢下行。`at_flush_rx()` 只清 AT 文本缓冲，保持上行握手与下行接收隔离 |

---

## 4. 常见修改配方

**加一个 .c 文件**
加到 `examples/stm32f103c8t6/Makefile.user` 的 `C_SOURCES`（**不要**写进 `Makefile`），然后 `mingw32-make -j8` 确认能编过、内存基线没异常增长。

**加一个对外 API**
1. `library/esp_net.h` 加声明 + 中文注释（含 `@retval`）；2. `library/esp_net.c` 加实现（涉及共享状态的要考虑中断/任务并发）；3. 在 `examples/.../demo_app.c` 里用一次；4. 更新根 `README.md` 的 API 表。

**加一个可被下行命令修改的变量**
1. 调大 `ESP_NET_VAR_MAX`；2. 在业务任务里 `esp_net_var_register("名字", &变量)`（名字必须是持久字符串）；3. 无需在业务里写解析代码。
> 解析规则在 `esp_net.c` 的 `net_parse_i32()` / `net_apply_commands()`：支持 `;` `,` 空格 换行分隔，支持十进制与 `0x` 十六进制、正负号。

**移植到新平台**
1. 新建 `port/esp_port_<平台>.c`，实现 `esp_port_t` 的 5 个函数；2. 在串口接收中断里调 `esp_net_input()`；3. **不要**改 `library/` 的任何文件。

**换 / 加一个协议栈（例如将来加 HTTP）**
1. 新建 `library/esp_proto_<协议>.c`，实现一份 `esp_proto_t`（照抄 `esp_proto_mqtt.c` 的形状）；2. 协议里**只准**用 `esp_stream_t` 的 `write` / `read`，不要 include `esp8266_at.h`；3. 加进 `Makefile.user` 的 `C_SOURCES`；4. 使用方在 `esp_net_init()` **之前**调 `esp_net_set_proto(&esp_proto_<协议>)`；5. `library/esp_net.c` **一行都不用改**（它只调这张表）。
> 返回值约定见 `esp_proto.h`：`<0` 一律当链路错误（触发重连），`publish` 的 `>0` 表示报文问题（丢该条、不断链）。

**改 MQTT 主题 / 服务器**
只改 `library/esp_net_config.h`。上行主题由业务在 `esp_net_publish()` 时指定（demo 里是 `demo_config.h` 的 `DEMO_TOPIC_PUB`）。

**加一个业务任务**
在 `freertos.c` 里按现有 `demoTask` 的写法加 `StaticTask_t` + `StackType_t[]` + `osThreadAttr_t` 三件套，**并算好 RAM**。

---

## 5. 禁止事项（踩过的坑）

- ❌ 用 `HAL_UART_Receive()` 轮询接收与 DMA 共存 —— 会 `HAL_BUSY`，且曾与 DMA 接收冲突（历史 bug，已删除该路径）。
- ❌ 用 `AT+MQTTCONN` —— 板载 AT 固件是 **v1.2.0.0（2016-07）**，根本没有这条命令。MQTT 必须是本库自己拼的 3.1.1 报文。
- ❌ 把 `esp8266_at.h` / `mqtt_client.h` include 进 `esp_net.h` —— 内部实现不得泄漏到公开头文件。
- ❌ 在 `esp_net.c` 里直接调 `mqtt_*`（或任何具体协议） —— 协议必须走 `esp_proto_t` 表（默认 `esp_proto_mqtt.c`），否则 HTTP / 别的协议插不进来。
- ❌ 让协议实现 include `esp8266_at.h` —— 协议只认 `esp_stream_t` 的 write/read，碰了 AT 指令就和 ESP8266 绑死，换硬件（W5500 / 4G / 裸 socket）就废了。
- ❌ 在中断里调 `printf` —— newlib 的 stdio 不可重入。
- ❌ 提交 `build/` 目录 —— 已在 `.gitignore` 里，别 `git add -f`。
- ❌ 为了「顺手清理」删掉 `library/` 里的模式判断、超时兜底、队列保留逻辑 —— 每一条都是针对实测故障加的（对应 §3 的 #4、§2 的判据 4）。
- ❌ 删掉 `Makefile.user` 末尾的 `-MMD -MP` / `.DEFAULT_GOAL := all` / `-include $(wildcard $(BUILD_DIR)/*.d)` —— 见 §3 #11，删了会静默出错（改了配置不重编，或 `mingw32-make` 干脆不再执行 `all`）。
- ❌ 手工「补回」CubeMX 覆盖掉的内容 —— 交给 `cubemx-fix.ps1`（`build.cmd` 会自动调），手工改容易漏、也容易和锚点脱节。

---

## 6. 已知未解 / 注意点

- **构建是 Makefile**：`.ioc` 的 `ProjectManager.TargetToolchain` 已是 `Makefile`（原先选错工具链生成成了 CMake），`CMakeLists.txt` / `CMakePresets.json` / `cmake/` 已**全部删除**。现在只有一套构建：`Makefile`（CubeMX 管）+ `Makefile.user`（我们管）+ `cubemx-fix.ps1`（自动修补）。
  **CubeMX 重新生成会覆盖 `Makefile` 和 `STM32F103XX_FLASH.ld`**，但**不用手工补**：跑 `build.cmd` 即可，它会先执行 `cubemx-fix.ps1`，把 `-include Makefile.user`（插在 `vpath %.c` 之前）和段属性 `(READONLY)` 两处补回。细节见 `examples/stm32f103c8t6/README.md` §3.3。
  `cubemx-fix.ps1` 靠锚点行 `vpath %.c` 定位；万一 CubeMX 模板变了导致锚点找不到，脚本会**直接报错中止**（而不是静默编出坏产物），届时改脚本里的锚点即可。
  退化项（CubeMX 生成后，不影响能不能编过，只是不好用）：产物落到扁平的 `build\` 而非 `build\Debug`/`build\Release`，`clean` 用 `-rm -fR`（本机 MinGW 没有 `rm`，会打印一条错误但不中断）；`flash.ps1` 两种目录都能找到固件。
- **本机 `C:\MinGW\bin` 里只有 `make`，没有 `sh` / `rm`**，所以 `Makefile` 的建目录与 `clean` 写成 cmd 兼容形式（`if not exist ... mkdir` / `rmdir /S /Q`），**不能**照抄 CubeMX 原生 Makefile 里的 `-rm -fR`。改这两条规则时要保持 cmd 兼容。
- **`Makefile` 不随编译选项自动失效**：目标文件以 `Makefile` / `Makefile.user` 为依赖，改这两个文件会触发全量重编；但用命令行变量（如 `TOOLCHAIN=...`）临时改选项不会。Debug / Release 各走 `build/Debug`、`build/Release`，互不干扰。
- **`Drivers/` 与 `Middlewares/` 是第三方代码**（ST HAL / CMSIS / FreeRTOS），随仓库提交只为「克隆即可编译」。**不要修改它们**，升级请走 CubeMX。
- **工具链是 GCC 5.2.1**（很老）：`STM32F103XX_FLASH.ld` 里的 `(READONLY)` 已删除，`-mthumb` 已写进 `Makefile` 的 `MCU`。CubeMX 重新生成会覆盖这两个文件：`(READONLY)` 由 `cubemx-fix.ps1` 自动删回；`-mthumb` 若丢失（自写 `arm-none-eabi-gcc` 时）按 `examples/.../README.md` §3.4 ① 确认。
- 上行队列满时 `esp_net_publish()` 返回 `-1` 并丢弃本条，属设计行为（不阻塞业务任务）。