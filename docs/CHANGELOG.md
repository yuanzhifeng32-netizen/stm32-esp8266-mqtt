# 更新日志

> 格式：`日期 — 版本/阶段 — 一句话`，下面列**改了什么 / 为什么 / 验证结果 / 内存变化**。
> 更细的维护红线见 [AGENTS.md](../AGENTS.md)，原理见 [INTERNALS.md](INTERNALS.md)。

---

## 2026-10-09 — 默认公网服务器 + PC13 配网灯 + 无凭据开机自动配网

**目标**：让设备"开箱即用"——默认直接连公网云服务器，没配过网就自动进配网（LED 提示），配好 WiFi 后走公网。

**改动**：

- `library/esp_net_config.h`
  - `ESP_MQTT_HOST` 由局域网 IP 改为**默认公网隧道** `"ko4rl4997501.vicp.fun"`（本机 EMQX 的 ws 口 8083 经 HTTP 隧道映射到公网 80）；`ESP_WS_PORT` `8083 → 80`。想走本地只改 host / 端口两行，`ESP_MQTT_PORT`(1883) 保留给裸 TCP 直连。
  - 新增 `ESP_PROV_AUTO_BOOT`（默认 1）：Flash 没存过凭据时开机自动进配网。
- `library/esp_net.[ch]`
  - `esp_net_init()` 记下 `s_credOk`（Flash 是否命中凭据）。
  - `esp_net_task()` 首轮自检：无凭据且 `ESP_PROV_AUTO_BOOT` 开 → 自动置 `s_configReq` 进配网。
  - 进入/退出配网维护 `s_inProv`；**新增第 12 个 API `esp_net_is_configuring()`** 供业务查状态点灯。
- `examples/.../Core/Inc/main.h` + `Core/Src/gpio.c`：新增 **PC13 状态灯**（Blue Pill 板载 LED，低电平点亮），默认熄灭。
- `examples/.../Core/Src/demo_app.c`：主循环 100 ms 一拍，配网期间翻转 PC13（闪烁），退出配网熄灭；横幅 broker 行按传输方式打印。
- 精简：核对 `esp_net_config.h` 全部宏，确认均有引用，**未删任何宏**（遵守"只加不删"约定）。

**内存**：FLASH 55760 → **56152 B (85.68%)**，RAM 不变 **18096 B (88.36%)**，编译零 `-Wall` 警告。

**验证**：全量重编零警告；`mingw32-make flash` → `** Verified OK **`。⚠️ 板端端到端待复测（默认连公网隧道 / 无凭据进配网 / PC13 闪烁）。

---

## 2026-10-09 — 修复：配网"保存并重启"（改为直接扫描表单正文，绕开 HTTP 分帧）

**问题**：上一版加了旁路回调后，`[prov rx]` 里仍只能看到 POST 的 **body**（`ssid=…&pass=…`），看不到含 `+IPD,<id>,<len>:POST /save` 的请求头，设备依然不重启。

**决断**：历次日志反复证明——**表单正文 `ssid=..&pass=..` 每次都稳定到达**（它在请求最末尾），只有前缀（请求行 + 多数请求头 + `+IPD` 帧头）会在发 AT 命令等应答时被吃掉。与其继续死磕 `+IPD` / HTTP 分帧，不如**直接在原始字节流里找表单字段**。

**修复**：

- `library/esp_prov.c`：新增 `prov_scan_byte()` 凭据扫描器——逐字节匹配 `ssid=` / `pass=` / `end=1` 三个标记，命中即取字段值（含 URL 解码 `%XX`、`+`→空格），三标记到齐即置 `s_scanReady`。凡进入 `prov_feed_byte()` 的字节（主动读 + 旁路回调两条路）都会过一遍扫描器。主循环检测到 `s_scanReady` 就**直接 `esp_cred_save()` 并 break**，对本路径完全**不依赖 +IPD / 跨帧 / link id**。
- 表单新增隐藏字段 `<input type="hidden" name="end" value="1">` 作结束哨兵（放在 pass 之后；否则最后一个字段没有终止符，不知何时收尾）。
- 原 HTTP 解析路径保留，二者互不影响：完整请求能拼齐时走原路，拼不齐时扫描器兜底。

**内存**：新增扫描缓冲 `s_scanSsid[33]` + `s_scanPass[65]` 等约 112 B。FLASH 54852 → **55760 B (85.08%)**，RAM 17984 → **18096 B (88.36%)**，编译零 `-Wall` 警告。

**验证**：⚠️ 板端待复跑。期望：点"保存并重启" → `[prov] saved ssid="…" (via scan)` → `[prov] credentials saved, reboot to apply` → 重启后用新 WiFi 联网。

**注意**：RAM 已到 **88.4%**（20 KB 只剩 ~2.4 KB），新增缓冲前务必先算内存。

---

## 2026-10-09 — 修复：配网页"保存并重启"没反应（AT 应答读取吃掉 HTTP 请求）

**问题**：手机连上 `STM32-Setup` 热点后网页能开、`GET /` 与 `GET /scan` 都正常，但点"保存并重启"无任何反应——不重启。串口 `[prov rx …]` 里能看到 POST 的 **body**（`ssid=…&pass=…`），却看不到请求头，也没有 `[prov] req(…) link N: POST /save` 这行。

**根因**：配网用 raw 接收，上网数据（`+IPD,<id>,<len>:…`）和 AT 应答**共用同一条接收环** `s_rxRing`。而发 AT 命令要走应答读取这条路径：

1. `esp_at_cmd()` 开头的 `at_flush_rx()` 会**直接清空**接收环；
2. `esp_at_expect()` 一路读环匹配 `OK` / `>` / `SEND OK`，**把期间到达的字节当噪声丢弃**。

配网发响应（`AT+CIPSEND` 等 `>` / 等 `SEND OK` / `AT+CIPCLOSE`）以及 `AT+CWLAP` 扫描期间，浏览器发来的**下一个请求**正好落在这条路径上，整段头被吃掉，只剩尾巴留在环里被后续解析看到。解析器因此永远匹配不到 `+IPD,`，`POST /save` 丢失 → 不保存、不重启。放大缓冲、把累计长度静态化都治不了这个（那是之前两轮的误判）。

**修复**：

- `library/esp8266_at.[ch]`：新增 `esp_at_set_raw_sink(fn)` 旁路回调。raw 模式下，**凡是被 AT 应答路径吃掉的字节**（`at_flush_rx()` 清环前、`esp_at_expect()` 读到的每一段）都先喂给该回调，而不是丢弃。
- `library/esp_prov.c`：把 `+IPD,<id>,<len>:` 解析抽成 `prov_feed_byte()`，**同时被两条路喂入**——① 主动读（`prov_pump`，`esp_at_read`）；② AT 应答旁路（回调）。两条路共享同一状态机，所以"发响应的同时到达的下一个请求"也能完整拼出来；用 `s_reqReady` 标志交给主循环，处理完上一请求再取走。

**第二处根因（上一版补丁引入的回归，已修）**：为"已凑齐的请求别被新字节覆盖"在 `prov_feed_byte()` 开头加了 `if (s_reqReady) return;` 守卫。但这个守卫**把新请求整帧丢掉**：发 `/scan` 响应期间，浏览器常先发 favicon / 重复 `/scan`，它一凑齐就把 `s_reqReady` 置 1；紧接着真正的 `POST /save` 帧到达，被守卫直接丢弃 → 只剩尾巴、永不重启。修法：**去掉守卫**，改成在 `PS_LEN` 匹配到 `:` 进入 `PS_DATA` 时判断——若上一个请求已 ready，则**新帧优先**（清 `s_reqLen`/`s_reqReady` 从头拼这个新请求）；只有"未拼完且 link 相同"的半包才保留跨帧续拼。

**顺带**：删除 `prov_close()` 及其在三个 handler 里的全部调用——响应头已带 `Connection: close`，多余的 `AT+CIPCLOSE` 只增加一段"等应答"窗口，反而更容易吃掉下一个请求。

其余加固保留：`s_req` 512 → **1024 B**；累计长度/当前 link id 静态化（跨帧拼包）；请求拼装期间刷新静默计时；`GET /scan` 的 JSON 补 `; charset=utf-8`；配网横幅 `CONFIG MODE (reqbuf 1024B)`。

**内存**：本修复**不需要额外缓冲**（旁路回调直接复用 `s_req`）。FLASH 54736 → **54852 B (83.70%)**（删掉 `prov_close` 后回落 ~96 B），RAM 17976 → **17984 B (87.81%)**，编译零 `-Wall` 警告。

**验证**：⚠️ 板端待复跑。期望：`[prov] req(NNN) link N: POST /save …` → `[prov] saved ssid="…"` → `[prov] credentials saved, reboot to apply` → 重启后用新 WiFi 联网。

**注意**：RAM 已到 **87.8%**（20 KB 只剩 ~2.5 KB），新增缓冲前务必先算内存。

**关于中文乱码**：串口日志里的 `灏忕背yu7` 是**串口终端按 GBK 解码 UTF-8 字节**的显示假象（`灏忕背` 正是 `小米` 的 UTF-8 字节被 GBK 解读的结果），模块透传、浏览器提交、`prov_form_get` 解码、Flash 存储全程都是 UTF-8，功能上没坏；只需在浏览器端确认中文 SSID 显示正常。

---

## 2026-10-09 — WiFi 配网（本地按键 + 云端下发，凭据存 Flash）

**目标**：设备换 WiFi / 首次部署时不必改代码重烧固件。两条路：① 本地按键触发开热点 + 微型网页配网；② 云端发 MQTT 消息远程下发 WiFi。

**新增文件**：

- `library/esp_cred.h` + `port/esp_cred_stm32f1.c` —— WiFi 凭据持久化（接口在库、实现在 port）。STM32F1 用 `HAL_FLASH` 存**最后一页** `0x0800FC00`（1 KB，F103C8T6 是 64 KB 芯片，第 63 页），布局 `magic + ssid_len + pass_len + cksum + ssid[33] + pass[65]`，写后读回 `strcmp` 校验。
- `library/esp_prov.[ch]` —— 本地配网核心：`AT+CWMODE=3` + `AT+CWSAP` 开 SoftAP → `AT+CIPMUX=1` + `AT+CIPSERVER=1,80` 开 TCP 服务 → 自己解析 `+IPD,<id>,<len>:` → 极简 HTTP 路由：`GET /`（网页）、`GET /scan`（`AT+CWLAP` 结果转 JSON）、`POST /save`（写凭据）。网页含 JS `fetch('/scan')` 自动扫描下拉。

**改动**：

- `library/esp_port.h` + `port/esp_port_stm32f1.c` —— `esp_port_t` 新增 `system_reset` 函数指针（配网保存后重启生效；实现为 `NVIC_SystemReset()`）。
- `library/esp8266_at.[ch]` —— 新增 `esp_at_set_raw_rx(on)`（旁路 `+IPD` 分流，把字节全交 AT 文本环给 esp_prov 自己解析）与 `esp_at_now_ms()`。
- `library/esp_net_config.h` —— 新增第 7 节配网宏：`ESP_PROV_ENABLE` / `ESP_PROV_AP_SSID`(STM32-Setup) / `ESP_PROV_AP_PASSWORD`(12345678) / `ESP_PROV_AP_CHANNEL` / `ESP_PROV_HTTP_PORT` / `ESP_PROV_HOLD_MS` / `ESP_PROV_TIMEOUT_MS` / `ESP_PROV_SCAN_TIMEOUT_MS` / `ESP_PROV_SSID_MAX` / `ESP_PROV_PASS_MAX` / `ESP_CRED_FLASH_PAGE` / `ESP_NET_TOPIC_WIFI`。**只加宏，未删改旧宏。**
- `library/esp_net.[ch]` —— 启动时优先读 Flash 凭据决定 WiFi（读不到回退配置宏）；`net_on_publish` 处理 `stm32/wifi` 主题（`ssid,pass` → 存 Flash → 重启）；联网任务循环顶部检查配网请求并调 `esp_prov_run()`；新增 3 个 API `esp_net_request_config()` / `esp_net_wifi_set()` / `esp_net_reboot()`（8 → 11 个）。
- `examples/.../Core/Inc/main.h` + `Core/Src/gpio.c` —— PB6 上拉输入（配网按键）。
- `examples/.../Core/Src/freertos.c` —— `esp_net_init()` 后检测 PB6：上电时已按住则直接进配网（放在 init 之后，避免被 init 里的标志清零覆盖）。
- `examples/.../Core/Src/demo_app.c` —— 主循环改 100 ms 一拍，轮询 PB6 连续按住满 `ESP_PROV_HOLD_MS` 触发配网。
- `Makefile.user` —— 加入 `esp_prov.c` / `esp_cred_stm32f1.c`。

**验证**：编译零 `-Wall` 警告。FLASH 44604 → **54156 B (82.64%)**，RAM 15632 → **17456 B (85.23%)**（+1.8 KB，主要是配网静态缓冲 `s_req[512]` / 扫描 `resp[512]` / `json[640]`）。⚠️ **硬件端到端未验证**：需实测 AT 固件 v1.2.0.0 的 `AT+CWSAP` / `AT+CWLAP` / `AT+CIPSEND=<id>,<len>` 行为，以及 `CWMODE=3` 下扫描是否受限（回退：先 `CWMODE=1` 扫描再 `CWMODE=3` 开热点）。

**注意**：RAM 已到 **85%**，新增缓冲/任务前必须先算内存。

---

## 2026-10-08 — 修复：WebSocket 握手缺 `Sec-WebSocket-Protocol` 被 broker 拒绝

**问题**：默认（WEBSOCKET）模式连不上，日志停在 `[net] websocket handshake failed`。

**根因**（实测复现）：MQTT over WebSocket 规定子协议名为 `"mqtt"`，EMQX（Cowboy）在握手时校验它。`esp_ws_connect()` 的握手请求里**没带** `Sec-WebSocket-Protocol: mqtt`，EMQX 直接回 **`HTTP/1.1 400 Bad Request`**（`esp_http_status()` 读到的不是 101 → 握手判失败）。用原始 socket 验证：不带该头 → 400，带上 → 101。

**修复**：`library/esp_ws.c` 的 `esp_ws_connect()` 在握手请求里补 `Sec-WebSocket-Protocol: mqtt`。

**验证**：编译零 `-Wall` 警告；FLASH 44572 → **44604 B**（+32 B 字面量），RAM 不变 15632 B。⚠️ 板端端到端待硬件复跑（EMQX ws: 8083 `/mqtt`）。

---

## 2026-10-08 — P2：MQTT over WebSocket（HTTP Upgrade + WS 帧）

**目标**：让 MQTT 能跑在 `ws://` 上（很多云平台只开 8083 而不开裸 1883）。原 TCP 直连路径原样保留，用 `ESP_MQTT_TRANSPORT` **编译期**切换，**默认已设为 WEBSOCKET**。

**原理**：WebSocket 不是新协议栈，只是"借一次 HTTP 请求把 TCP 升级成帧通道"。所以 = HTTP 握手 + 帧封/拆，底层仍是已有的 TCP 通路。

**新增**：

- `library/esp_http.[ch]` —— 极简 HTTP/1.1 客户端：拼请求 / 收响应头 / 解析状态码与头（可复用做 REST）。
- `library/esp_ws.[ch]` —— WebSocket 客户端：随机 16 B → Base64 当 `Sec-WebSocket-Key` → HTTP Upgrade → 校验 `Sec-WebSocket-Accept`（自带 SHA-1 + Base64）；写出帧加**客户端掩码**、读入逐帧解析并就地处理 ping/close；**产出一个 `esp_stream_t`**，MQTT 完全无感。
- `library/esp_net_config.h` —— 新增 `ESP_MQTT_TRANSPORT`（默认 WEBSOCKET）、`ESP_WS_PORT` / `ESP_WS_PATH` / `ESP_WS_HANDSHAKE_TIMEOUT_MS` / `ESP_WS_TX_BUFFER_SIZE` / `ESP_WS_RX_BUFFER_SIZE`。**只加宏，未删改任何旧宏。**
- `Makefile.user` 加入 `esp_http.c` / `esp_ws.c`。

**接线**（`library/esp_net.c`）：WEBSOCKET 时 `esp_ws_init(&s_stream)` 后把 `esp_ws_stream()` 交给协议实现；建链时 TCP 连好后、MQTT CONNECT 前插入 `esp_ws_connect()`，失败即退出透传并重试。协议层（`esp_proto_mqtt.c` / `mqtt_client.c`）**一行未改**。

**验证**：两种传输模式均编译零 `-Wall` 警告。

- TCP（透传链路，非默认）：FLASH **38752 B** / RAM **14792 B (72.23%)** —— 相比改动前的透传基线（37896/14728）只 +856 B FLASH / **+64 B RAM**：TCP 模式下 `--gc-sections` 把 WS 的代码与缓冲整体丢弃，几乎零开销。
- WEBSOCKET（透传链路，**默认**）：FLASH **44572 B** / RAM **15632 B (76.33%)**（+5.8 KB FLASH / +840 B RAM）。
- 注：上一版记录的 39444/15312 是**分帧链路**下的数字（多 512 B 分帧数据缓冲），不能与上面的透传数字直接相减。
- ⚠️ **硬件端到端未验证**：需 EMQX 开启 ws 监听（默认 8083 / 路径 `/mqtt`）后，按 AGENTS.md §2 五判据跑（ONLINE / 周期上报 / 下行命令 / 停 broker 干净重试 / 重开自动恢复）。

**限制**：只支持明文 `ws://` —— 板载 AT 固件 v1.2.0.0 无 TLS socket，`wss://` 不可用。

---

## 2026-10-08 — 分帧模式收发修复（上行/下行隔离）

**问题**：`ESP_LINK_MODE = FRAME` 时根本连不上服务器，日志停在 `[mqtt] CONNECT failed, status 3`。

**根因**（两个）：

1. **`library/esp8266_at.c` 完全没有解析 `+IPD,<len>:`** —— 分帧模式下 TCP 载荷从未被挑出来交给 MQTT，收到的字节全被当成 AT 应答文本，MQTT 永远等不到 CONNACK，最终超时（`status 3` 是库内部的 `MQTT_ERR_TIMEOUT`，不是 broker 返回码；EMQX 本身正常）。
2. **`esp_at_cmd()` 每次发 AT 指令都会调 `esp_at_flush()`** —— 它会连 TCP 载荷缓冲和拆包状态一起清掉。分帧模式下发送要等 `>`，这段等待期间到达的下行数据躺在数据缓冲里，会被下一次发送静默丢弃。

**修复**：

- `library/esp8266_at.c`
  - 新增分帧分流状态机 `at_ipd_feed()`（`+IPD,` → 长度 → `:` → 搬 `<len>` 字节到数据缓冲），非头部字节回吐为 AT 文本（`s_ipdHold[5]` 暂存候选前缀）。
  - 新增 `s_dataRing[ESP_AT_DATA_BUFFER_SIZE]` 专用 TCP 载荷缓冲，与 AT 文本缓冲 `s_rxRing` 物理隔离。
  - 新增 `at_flush_rx()`：只清 AT 文本缓冲；`esp_at_cmd()` 改用它，**不再**动数据缓冲与拆包状态机。
  - `esp_at_flush()`（全清）保留给重建链路的场景（复位 / 退透传 / init）。
  - 新增 `esp_at_read_data()`：分帧只取数据缓冲；透传等价 `esp_at_read()`。
- `library/esp_net_config.h`：新增 `ESP_AT_DATA_BUFFER_SIZE`（512 B，仅分帧用）。
- `library/esp_net.c`：`net_transport_read()` 改调 `esp_at_read_data()`。
- `library/esp8266_at.h`：新增 `esp_at_read_data()` 声明；更新两种模式与 `esp_at_flush()` 的说明。

**验证**（硬件，分帧模式，5 条判据全过）：`CONNACK ok` → `subscribed: stm32/down` → `ONLINE` → 周期上报；下行 `period=3` 生效（上报间隔 10s→3s）；停 EMQX 后干净重试、无 `+++`；重开 EMQX 自动恢复 ONLINE。

**内存**：RAM 14776 → **15312 B (74.77%)**（+512 B 数据缓冲），FLASH 38544 → **39444 B (60.19%)**。编译零 `-Wall` 警告。

**遗留**：透传模式是等价替换（编译通过），**未重跑硬件回归**。

---

## 2026-10-08 — P1：协议解耦（可插拔协议栈）

**目标**：让 `esp_net.c` 不再硬编码 MQTT，为后期接 HTTP 铺路。

**改动**：

- 新增 `library/esp_proto.h` —— 协议 vtable（`esp_proto_t`）+ `esp_stream_t` 字节流抽象；返回值约定 `<0` = 链路错、`publish` 的 `>0` = 报文问题（丢该条不断链）。
- 新增 `library/esp_proto_mqtt.[ch]` —— 把原 `esp_net.c` 里的 MQTT 状态与缓冲平移过来，包住现有 `mqtt_client`。
- `library/esp_net.c` 改为只调 `esp_proto_t` 表；建链时序、在线循环、断线自愈逻辑一字未改。
- `library/esp_net.h` **只新增** `esp_net_set_proto()`（7 → 8 个 API），现有签名不动。
- `Makefile.user` 加入 `esp_proto_mqtt.c`。

**验证**：编译零警告；烧录并过全部 5 条判据（当时用透传模式）。**RAM 14728 → 14776 B**（新增协议配置结构），FLASH 37896 → 38544 B。

---

## 2026-10-08 — 构建体系加固（CubeMX 重新生成后免手工修补）

- 新增 `cubemx-fix.ps1`：CubeMX 重新生成后自动补回 `-include Makefile.user`（插在 `vpath %.c` 前）和链接脚本的 `(READONLY)` 删除；无变化时静默跳过，锚点找不到时**报错中止**（而非静默编出坏产物）。
- `build.cmd` 编译前先跑 `cubemx-fix.ps1`，失败即中止。
- `Makefile.user` 开启头文件依赖跟踪：`%.o: CFLAGS += -MMD -MP`、`.DEFAULT_GOAL := all`、`-include $(wildcard $(BUILD_DIR)/*.d)`（末尾三行不可删）。
- `flash.ps1` 兼容 `build/<config>/` 与扁平 `build/` 两种产物目录。
- 同步 `README.md` / `AGENTS.md`。

**验证**：`cubemx-fix.ps1` 空操作退出码 0；全量编译零警告，内存与基线一致。

---

## 基线（引入以上改动之前）

- 透传模式联网可用；Debug FLASH 37896 B / RAM 14728 B。
- 零 `malloc`，任务静态创建；断线自愈三机制（`CLOSED` 匹配 / 发送失败 / keepalive 超时）+ 硬件复位兜底。