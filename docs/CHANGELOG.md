# 更新日志

> 格式：`日期 — 版本/阶段 — 一句话`，下面列**改了什么 / 为什么 / 验证结果 / 内存变化**。
> 更细的维护红线见 [AGENTS.md](../AGENTS.md)，原理见 [INTERNALS.md](INTERNALS.md)。

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