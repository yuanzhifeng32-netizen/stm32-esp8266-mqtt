# stm32103 — STM32F103C8T6 + ESP8266 + MQTT 示例工程

本目录是 [esp-net-framework](../../README.md) 的**完整可跑 demo**：CubeMX 工程 + 一键编译/烧录脚本，
克隆下来改两行配置就能联网。

- 框架本身（`library/` + `port/`）在**仓库根目录**，本目录只有「应用代码 + CubeMX 生成的骨架 + 第三方库」；
- 应用代码只有两个文件：[demo_app.c](Core/Src/demo_app.c)（业务：周期上报 + 下行命令）和 [freertos.c](Core/Src/freertos.c)（起两个任务）；
- 想知道 API 怎么用：看 [demo_app.c](Core/Src/demo_app.c)；想改 WiFi/服务器：改 [library/esp_net_config.h](../../library/esp_net_config.h)。

---

## 0. 最快路径

在本目录下：

```bat
build.cmd          :: 编译（Debug），产物在 build\Debug\
flash.cmd          :: 用 ST-Link 烧录（含校验和复位）
```

也可以直接敲 make：

```powershell
mingw32-make                  # Debug（默认）
mingw32-make CONFIG=Release   # Release
mingw32-make -j8              # 并行编译
mingw32-make clean            # 删除 build\
mingw32-make flash            # 编译 + 烧录
```

> 需要 PATH 里有 **GNU Make**（本机是 `C:\MinGW\bin\mingw32-make.exe`，`build.cmd` 会先检查并给出提示）。
> `flash.ps1` 是 PowerShell 脚本，必须保存为 **UTF-8 带 BOM**，否则 Windows PowerShell 5.1 会按 GBK 解码中文注释、直接语法报错（本工程已带 BOM）。

跑起来之前**必须先改** [library/esp_net_config.h](../../library/esp_net_config.h) 里的 WiFi 与 MQTT 服务器地址，否则会一直打印 `wifi join failed`。

---

## 1. 工程结构

```
examples/stm32f103c8t6/
├─ Core/
│   ├─ Inc/  demo_app.h          demo 业务声明
│   │        demo_config.h       ★ demo 自己的参数（上行主题 / 上报周期 / 透传调试开关）
│   ├─ Src/  demo_app.c          ★ demo 业务：周期上报 + 下行命令（API 用法看这里）
│   │        freertos.c          ★ 起两个任务：esp_net_task() + demo_app_task()
│   │        main.c / gpio.c / usart.c / stm32f1xx_it.c / ...   CubeMX 生成
├─ Drivers/                      STM32F1 HAL 驱动 + CMSIS（ST 官方，CubeMX 管理）
├─ Middlewares/                  FreeRTOS 内核 + CMSIS-RTOS V2（官方，CubeMX 管理）
├─ startup_stm32f103xb.s         启动文件（向量表 / Reset_Handler）
├─ STM32F103XX_FLASH.ld          链接脚本
├─ stm32103.ioc                  CubeMX 工程文件（改引脚/外设就改它再重新生成）
├─ Makefile                      ★ 构建主体（CubeMX「Makefile」工具链的形状，可被重新生成）
├─ Makefile.user                 ★ 我们自加的源文件/包含路径（CubeMX 不认识，重新生成不会被覆盖）
├─ build.cmd                     ★ 双击即可编译（内部调 mingw32-make）
├─ flash.ps1  flash.cmd          ★ ST-Link 烧录脚本
└─ build/                        输出目录（build\Debug、build\Release，已 gitignore）
```

`Drivers/` 与 `Middlewares/` 是**第三方代码**，随工程一起提交只是为了「克隆即可编译」；改配置不要动它们。

**框架代码在仓库根目录**（本目录通过相对路径 `-I../../library` 引用）：

| 路径 | 作用 |
|---|---|
| [library/esp_net.h](../../library/esp_net.h) | 用户唯一头文件，7 个 API |
| [library/esp_net_config.h](../../library/esp_net_config.h) | 唯一配置入口：WiFi / broker / 主题 / 缓冲 / 日志 |
| [library/esp_net.c](../../library/esp_net.c) | 联网编排 + 断线自愈 + 下行命令分发 |
| [library/esp8266_at.c](../../library/esp8266_at.c) / [mqtt_client.c](../../library/mqtt_client.c) | 内部：AT 驱动 / MQTT 3.1.1 报文 |
| [port/esp_port_stm32f1.c](../../port/esp_port_stm32f1.c) | STM32F1 移植层（USART DMA 循环 + IDLE 中断 + PA8 复位） |

### 芯片配置（来自 `stm32103.ioc` 与生成代码）

| 项目 | 值 |
|---|---|
| 系统时钟 | HSI 8 MHz，**未开 PLL**，SYSCLK/APB1/APB2 = 8 MHz |
| GPIO | **PA8 = ESP8266 使能/复位脚**（推挽输出，低=复位、高=使能），在 `MX_GPIO_Init()` 的 `USER CODE BEGIN ESP_EN` 段里配置 |
| 调试口 | SWD：PA13(SWDIO) / PA14(SWCLK) |
| USART1 | **PA9=TX / PA10=RX**，115200-8-N-1，**日志输出串口**（`printf` 重定向到此） |
| USART2 | PA2=TX / PA3=RX，115200-8-N-1，**DMA1_Channel6 循环接收 + IDLE 空闲中断**，接 ESP8266 |
| USART3 | PB10/PB11，115200-8-N-1（本 demo 未使用） |
| RTOS | FreeRTOS + CMSIS-RTOS V2：`defaultTask`（空闲）、`netTask`（`esp_net_task`，静态栈 2 KB）、`demoTask`（`demo_app_task`，静态栈 1 KB） |
| 时基 | TIM4（`HAL_TIM_PeriodElapsedCallback` → `HAL_IncTick`） |

**为什么任务必须静态分配**：`FreeRTOSConfig.h` 里 `configTOTAL_HEAP_SIZE` 只有 **3072 B**，idle + timer + defaultTask 已占约 2.4 KB，塞不下额外的任务栈。所以在 `osThreadAttr_t` 里填 `cb_mem`/`stack_mem`，CMSIS-RTOS2 走 `xTaskCreateStatic`，**完全不占 FreeRTOS 堆**。**改代码时不要改成 `osThreadNew` + `osThreadNew` 动态创建，会立刻耗尽堆。**

---

## 2. 本机工具链（已就绪，不需要装任何东西）

| 用途 | 路径 | 版本 |
|---|---|---|
| ARM 编译器 | `D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-gcc.exe` | GCC **5.2.1** 20151202 |
| binutils / objcopy / size | 同目录 `arm-none-eabi-objcopy.exe`、`arm-none-eabi-size.exe` | GNU ld **2.25.90** |
| 烧录/调试 | `D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\bin\openocd.exe` | OpenOCD **0.12.0** |
| OpenOCD 脚本目录 | `...\xpack-openocd-0.12.0-7-win32-x64\openocd\scripts` | `interface/stlink.cfg`、`target/stm32f1x.cfg` |
| **GNU Make（构建）** | `C:\MinGW\bin\mingw32-make.exe` | GNU Make **4.2.1**（x86_64-w64-mingw32） |
| Python（抓串口用） | `D:\Python\python.exe` | 3.14.2，已装 pyserial 3.5 |
| `stm32flash`（串口 ISP 备用） | `D:\twBlock\apm32f103\tools\stm32flash.exe` | — |

一条命令验证工具链：

```powershell
& 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-gcc.exe' --version
```

脚本会自动在这些位置找工具链；**换电脑时若路径不同**，先设环境变量：

```powershell
$env:ARM_GCC_BIN  = 'D:\别的路径\gcc-arm\bin'
$env:OPENOCD_EXE  = 'D:\别的路径\openocd.exe'
```

---

## 3. 编译

### 3.1 方式一：make（推荐，已实测通过）

```powershell
mingw32-make                  # Debug：-O0 -g3 -DDEBUG（默认）
mingw32-make CONFIG=Release   # Release：-Os -g0
mingw32-make -j8              # 并行
```

产物（`build\<配置>\`）：

| 文件 | 用途 |
|---|---|
| `stm32103.elf` | ST-Link / OpenOCD 烧录和调试 |
| `stm32103.hex` | Intel HEX，STM32CubeProgrammer / ST-Link Utility / 串口 ISP |
| `stm32103.bin` | 纯二进制，通用烧录器 / 串口 ISP |
| `stm32103.map` | 链接映射表（查内存占用、符号地址） |

本机实测结果（重构为 library/port 分层之后）：

```
Debug   : FLASH 37896 B / 64 KB (57.82%)   RAM 14728 B / 20 KB (71.91%)
Release : FLASH 25892 B / 64 KB (39.51%)   RAM 14720 B / 20 KB (71.88%)
```

`-Wall` **无警告**。Debug 与 Release 各自输出到独立目录，切换配置不会混用旧的目标文件。

> **新增自己的 .c 文件时，写到 `Makefile.user`，不要写进 `Makefile`。**
> 理由：`Makefile` 是 CubeMX 能重新生成的文件（见 §3.3），写进去会被覆盖；`Makefile.user` 不会被碰。
> 忘了加会导致链接报 `undefined reference`。

### 3.2 方式二：手动敲命令（了解原理 / 没有 make 的环境）

在本目录执行下面 4 步即可。清单与 `Makefile` / `Makefile.user` 保持一致
（**唯一权威是 `Makefile` 与 `Makefile.user`**；想看 make 实际发出的完整命令：`mingw32-make -n | Select-String 'arm-none-eabi-gcc'`）。

**第 1 步 · 定义公共参数**（PowerShell 数组，避免路径被拆坏）：

```powershell
$GCC  = 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-gcc.exe'
$OBJ  = 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-objcopy.exe'

$CPU  = @('-mcpu=cortex-m3','-mthumb')            # Cortex-M3 必须 -mthumb
$WARN = @('-Wall','-fdata-sections','-ffunction-sections','-std=gnu11')
$OPT  = @('-O0','-g3')                            # Release 改成 @('-Os','-g0')
$DEF  = @('-DUSE_HAL_DRIVER','-DSTM32F103xB','-DDEBUG')

$INC  = @(
  '-ICore/Inc'
  '-I../../library'                               # ★ 框架
  '-I../../port'                                  # ★ 移植层
  '-IDrivers/STM32F1xx_HAL_Driver/Inc'
  '-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy'
  '-IMiddlewares/Third_Party/FreeRTOS/Source/include'
  '-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2'
  '-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3'
  '-IDrivers/CMSIS/Device/ST/STM32F1xx/Include'
  '-IDrivers/CMSIS/Include'
)
$SRC = @(
  'Core/Src/main.c','Core/Src/gpio.c','Core/Src/freertos.c','Core/Src/usart.c',
  'Core/Src/stm32f1xx_it.c','Core/Src/stm32f1xx_hal_msp.c','Core/Src/stm32f1xx_hal_timebase_tim.c',
  'Core/Src/sysmem.c','Core/Src/syscalls.c','Core/Src/system_stm32f1xx.c',
  'Core/Src/demo_app.c',                          # ★ demo 业务
  '../../library/esp8266_at.c','../../library/mqtt_client.c','../../library/esp_net.c',   # ★ 框架
  '../../port/esp_port_stm32f1.c',                # ★ 移植层
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c',
  'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c',
  'Middlewares/Third_Party/FreeRTOS/Source/croutine.c',
  'Middlewares/Third_Party/FreeRTOS/Source/event_groups.c',
  'Middlewares/Third_Party/FreeRTOS/Source/list.c',
  'Middlewares/Third_Party/FreeRTOS/Source/queue.c',
  'Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c',
  'Middlewares/Third_Party/FreeRTOS/Source/tasks.c',
  'Middlewares/Third_Party/FreeRTOS/Source/timers.c',
  'Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c',
  'Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c',
  'Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c',
  'startup_stm32f103xb.s'
)
$LINK = @(
  '-T','STM32F103XX_FLASH.ld','--specs=nano.specs',
  '-Wl,-Map=build/Debug/stm32103.map','-Wl,--gc-sections','-Wl,--print-memory-usage','-lm'
)
New-Item -ItemType Directory -Force build\Debug | Out-Null
```

**第 2 步 · 编译 + 链接**（一条命令编完所有 .c 和 .s）：

```powershell
& $GCC @CPU @WARN @OPT @DEF @INC @SRC @LINK -o build/Debug/stm32103.elf
```

**第 3 步 · 生成 hex / bin**：

```powershell
& $OBJ -O ihex   build/Debug/stm32103.elf build/Debug/stm32103.hex
& $OBJ -O binary build/Debug/stm32103.elf build/Debug/stm32103.bin
```

**第 4 步 · 查看段大小**：

```powershell
& 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-size.exe' build/Debug/stm32103.elf
```

### 3.3 用 CubeMX 重新生成代码时要注意什么

工程的 `.ioc` 已把工具链设为 **Makefile**（`ProjectManager.TargetToolchain=Makefile`），
所以以后用 CubeMX 重新生成时，它会产出一份**属于它的 `Makefile`，并覆盖本目录现有的 `Makefile`**。

好在我们已经把「CubeMX 管的部分」和「我们管的部分」分开了：

| 谁管 | 文件 | 重新生成后 |
|---|---|---|
| CubeMX | `Makefile`（主体：Core / Drivers / Middlewares 的源清单与包含路径） | 被覆盖 |
| 我们 | `Makefile.user`（`../../library`、`../../port`、`demo_app.c`） | **不受影响** |

所以重新生成后只需要做一件事：**把这一行补回主 `Makefile`**（位置：紧跟 `C_INCLUDES` 定义之后），然后照常 `mingw32-make`：

```makefile
-include Makefile.user
```

另见 §3.4。

### 3.4 为适配 GCC 5.2.1（很老）需要注意的两处

**① 编译必须带 `-mthumb`**

原因：Cortex-M3 是**纯 Thumb 内核**。本机这套 GCC 默认按 ARM 模式编译，缺了会直接报错：

```
error: target CPU does not support ARM mode
```

本工程已把它写进 `Makefile` 的 `MCU = -mcpu=cortex-m3 -mthumb`，用 make 不会遇到；
只有自己手敲 `arm-none-eabi-gcc`（§3.2）时才要记得带上。

**② `STM32F103XX_FLASH.ld`：删除 `(READONLY)` 段属性**

```diff
-  .ARM.extab (READONLY) : /* ... remove it if using GCC10 or earlier. */
+  .ARM.extab : /* (READONLY) removed: only supported by GCC11+/binutils 2.38+ ... */
```

共 5 处（`.ARM.extab` / `.ARM` / `.preinit_array` / `.init_array` / `.fini_array`）。
原因：`(READONLY)` 需要 **GCC 11+ / binutils 2.38+**，本机是 GCC 5.2.1 / binutils 2.25.90，链接报错：

```
STM32F103XX_FLASH.ld:105: non constant or forward reference address expression for section .ARM.extab
```

链接脚本自己的注释也写了「GCC10 或更早就删掉它」。删掉后对 GCC 11+ 同样合法，所以这个改动是安全的。

> ⚠️ 如果以后用 CubeMX 重新生成代码，`STM32F103XX_FLASH.ld` 与 `Makefile` 都会被覆盖：
> 前者要把 `(READONLY)` 再删一遍（见 ② ），后者要把 §3.3 那一行 `-include Makefile.user` 补回。

---

## 4. 烧录到单片机（ST-Link + OpenOCD）

### 4.1 接线

| ST-Link | 单片机 | 说明 |
|---|---|---|
| SWDIO | **PA13** | 数据 |
| SWCLK | **PA14** | 时钟 |
| GND | GND | **必须共地** |
| 3.3V | 3V3 | **必须接**！OpenOCD 靠这根线检测目标电压，不接会直接报 0 V 拒绝连接 |

也可以不用 ST-Link 供电、板子插自己的 USB 供电，但 **3.3V 这根线仍然要接**（当电压检测用）。

**ST-Link V2 克隆的 10 脚（2×5）排针有两个变种，引脚定义不同，且丝印经常标错**，详见 [stlink-clone-adapter](https://github.com/noahp/stlink-clone-adapter)。两个变种的 3.3V **都在第 7、8 脚**：

| | 变种 1（较老） | 变种 2（较新） |
|---|---|---|
| 1 / 2 | RST / **SWDIO** | RST / **SWCLK** |
| 3 / 4 | **GND** / GND | SWIM / **SWDIO** |
| 5 / 6 | SWIM / **SWCLK** | **GND** / GND |
| 7 / 8 | **3.3V** / 3.3V | **3.3V** / 3.3V |
| 9 / 10 | 5.0V / 5.0V | 5.0V / 5.0V |

判定自己手上是哪个变种：拔掉 ST-Link，用万用表通断档量——
**3、4 脚互通 = 变种 1**；**5、6 脚互通 = 变种 2**（那一对就是 GND）。
然后用万用表确认第 7/8 脚对 GND 是 3.3 V，再按变种把 SWDIO/SWCLK 接到 PA13/PA14。

> 单片机一般用板子上的 4 针 SWD 排针（丝印 3V3 / SWDIO / SWCLK / GND），或直接飞线到 PA13(第 34 脚)、PA14(第 37 脚)、3V3、GND。

### 4.2 一键烧录

```powershell
.\flash.cmd                 # 烧录 build\Debug\stm32103.elf，自动校验+复位
.\flash.cmd -Config Release
.\flash.cmd -Probe          # 只检测连接，不烧录
.\flash.cmd -Erase          # 先全片擦除再烧录（芯片被读保护时用）
```

### 4.3 手动 OpenOCD 命令

```powershell
$OCD = 'D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\bin\openocd.exe'
$SD  = 'D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\openocd\scripts'

# ① 先只检测（这一步能排除 90% 的问题）
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg -c "init; targets; shutdown"

# ② 烧录 + 校验 + 复位 + 退出
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "program build/Debug/stm32103.elf verify reset exit"

# ③ 芯片被读保护时：先全片擦除再烧
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "init" -c "reset halt" -c "stm32f1x mass_erase 0" `
       -c "program build/Debug/stm32103.elf verify reset exit"

# ④ 烧 .hex 也可以（把文件名换掉即可）
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "program build/Debug/stm32103.hex verify reset exit"
```

> 注意：OpenOCD 对**含中文的绝对路径**处理不好，所以命令里统一用**相对路径**（先 `cd` 到本目录）。

**连上目标时应该看到：**

```
Info : STLINK V2J46S7 (API v2) VID:PID 0483:3748
Info : Target voltage: 3.2xxxxx
Info : [stm32f1x.cpu] Cortex-M3 r1p1 processor detected
Info : [stm32f1x.cpu] target has 6 breakpoints, 4 watchpoints
** Programming Started **
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **
```

### 4.4 验证真的烧进去了（不需要 LED）

板子上没有用户 LED，肉眼看不到现象，用 OpenOCD 回读芯片内容验证：

```powershell
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "init" -c "halt" -c "mdw 0x08000000 4" -c "resume" -c "shutdown"
```

期望输出里前两个字应该是：

| 地址 | 期望值 | 含义 |
|---|---|---|
| `0x08000000` | `20005000` | 初始 MSP = 0x20000000 + 0x5000（20 KB SRAM 顶部） |
| `0x08000004` | `0800xxxx`（**奇数**） | Reset_Handler 地址，Thumb 地址最低位为 1 |

更简单的办法：`program ... verify` 输出 `** Verified OK **` 就说明 Flash 内容与 elf 完全一致。

### 4.5 报 `Target voltage: 0.000000` 时怎么查

这是本机遇到过的真实故障，OpenOCD 会这样拒绝连接：

```
Info : STLINK V2J46S7 (API v2) VID:PID 0483:3748
Info : Target voltage: 0.000000
Error: target voltage may be too low for reliable debugging
Error: init mode failed (unable to connect to the target)
```

含义很明确：**ST-Link 的电压检测脚（TVCC / 3.3V）上量到 0 V**。按可能性从高到低排查：

**⓪ 同时插了多个 ST-Link（本机最终就是这个原因）**
电脑上插了 2 个 ST-Link 时，OpenOCD **只会打开其中一个**，如果打开的刚好是没接板子的那个，读到的就是 0.000000 V，看起来像「板子没接」。
拔掉多余的 ST-Link，只留接板子的那一个；或者用序列号指定：

```powershell
& $OCD -s $SD -f interface/stlink.cfg -c "adapter serial <序列号>" -f target/stm32f1x.cfg -c "init; targets; shutdown"
```

序列号可以从设备管理器看，或者先跑一次 `-Probe` 看它打开的是哪台。**实测：拔掉多余的那个之后，同一套接线立刻读出 `Target voltage: 3.333601` 并成功烧录。**

**① 克隆的「变种」判断错了**
克隆排针有变种 1 / 变种 2 两套**完全不同**的引脚定义，而外壳和丝印经常按另一套标注（见 §4.1 的出处）。若按错的变种接 GND，就等于**没有共地**，OpenOCD 的 ADC 会读到 0.000 V 并直接拒绝连接。

两种接法的完整对照（3.3V 都在第 7/8 脚）：

| 接到单片机 | 变种 1 接法 | 变种 2 接法 |
|---|---|---|
| PA13 (SWDIO) | **pin 2** | **pin 4** |
| PA14 (SWCLK) | **pin 6** | **pin 2** |
| GND | **pin 3 或 4** | **pin 5 或 6** |
| 3V3 | **pin 7 或 8** | **pin 7 或 8** |

判定方法（30 秒）：拔下 ST-Link，用万用表**通断档**量——`3–4 脚互通 ⇒ 变种 1`，`5–6 脚互通 ⇒ 变种 2`（互相导通的那一对才是 GND）。
没有万用表就**直接按变种 1 重接一次再试**：只是挪 3 根线，最坏结果是白试一次。

**② 3.3V 根本没接** —— ST-Link 的 3.3V **必须**连到板子 3V3，即使板子自己插 USB 供电也要接，它同时是电压检测线。

**③ 板子确实没电 / 3V3 是 0 V** —— 拔掉 ST-Link，只给板子插 USB，量板子 3V3 对 GND 应为 3.3 V。

**④ 排线、杜邦线接触不良** —— 换线，或直接焊到测试点。

**⑤ SWDIO/SWCLK 接反** —— 电压已读到 3.3 V 但仍 `init mode failed` 时，把这两根对调再试。

**⑥ PA13/PA14 被程序占用** —— 本工程没有占用（`gpio.c` 只配了 PA8），可排除。

**⑦ 电压正常仍连不上** → 加 `-Erase`（可能被读保护），或降低速度：在 cfg 后加 `-c "adapter speed 480"`。

### 4.6 其它烧录方式（备用）

| 方式 | 需要的硬件 | 命令/工具 |
|---|---|---|
| STM32CubeProgrammer | ST-Link | 选 SWD，直接加载 `stm32103.hex` 或 `.elf`，点 Download |
| ST-Link Utility（老板软件） | ST-Link | `Target → Connect`，`File → Open` 选 `.hex`，`Target → Program & Verify` |
| 串口 ISP（芯片自带 Bootloader） | USB-TTL（CH340/CP2102） | 见下方具体做法 |
| 通用烧录器 | — | 直接写 `stm32103.bin` 到 `0x08000000` |

**串口 ISP 具体做法**（本机已有 `stm32flash.exe`，无需再装软件）：

接线用 USART1 的**默认引脚** PA9/PA10，和 §6 的日志接线完全相同：

| USB-TTL | 单片机 |
|---|---|
| TX | **PA10**（USART1_RX） |
| RX | **PA9**（USART1_TX） |
| GND | GND |
| 3V3（若有） | 3V3 |

再把板子的 **BOOT0 跳到 1（3.3V）**，按一下复位（或重新上电），然后：

```powershell
$SF = 'D:\twBlock\apm32f103\tools\stm32flash.exe'
& $SF -b 115200 -w build/Debug/stm32103.hex -v -g 0x08000000 COM5    # COM5 换成实际串口
```

写入 `.bin` 时要显式给起始地址：

```powershell
& $SF -b 115200 -S 0x08000000 -w build/Debug/stm32103.bin -v -g 0x08000000 COM5
```

烧完把 **BOOT0 跳回 0（GND）**，再复位，芯片就从 Flash 启动运行了。
（`stm32flash` 默认串口模式是 `8e1`，即 8 数据位 + 偶校验，符合 STM32 Bootloader 要求，不要改。）

---

## 5. 上电后芯片在做什么

`main()`：`HAL_Init` → `SystemClock_Config`（HSI 8 MHz，无 PLL）→ 初始化 GPIO/USART1/USART2/USART3 →
`osKernelInitialize` → `MX_FREERTOS_Init`（静态创建 `defaultTask` / 两个业务任务，并调 `esp_net_init()`）→ `osKernelStart`。

三个任务：

| 任务 | 作用 |
|---|---|
| `defaultTask` | 空循环 `osDelay(1)`，无业务逻辑 |
| `netTask` → `esp_net_task()` | **联网主任务**：连 WiFi → 连 TCP → MQTT 握手 → 订阅 → 收发保活，任何一步失败或掉线都会自动回到起点重来（**不返回**） |
| `demoTask` → `demo_app_task()` | **示例业务**：联网后按周期把 JSON 上报到 `stm32/up`；并把 `period` 变量登记进下行命令表 |

`esp_net_init(&esp_port_stm32f1)` 在调度器启动**之前**调用，它内部会：
启动 USART2 的 DMA 循环接收 → 关掉 DMA 的 TC/HT 中断 → 打开 USART2 的 IDLE 空闲中断。
这样 ESP8266 的短应答（几十字节、填不满 DMA 缓冲）也能及时被喂进库的环形缓冲。

**PA8 复位时序**：`MX_GPIO_Init()` 里把 PA8 拉低 300 ms 再拉高，让 ESP8266 干净启动；
后续断线恢复时，`esp_at_hw_reset()` 会用同样的时序兜底复位模块。

**预期串口输出**（USART1，115200）：

```
============================================
  stm32103 | STM32F103C8T6 | ESP8266 + MQTT (demo)
  USART2 : PA2/PA3 115200 <--DMA ch6--> ESP8266
  USART1 : PA9/PA10 115200   debug log
  broker : 192.168.137.1:1883   cmd topic: stm32/down
  pub    : stm32/up   period: 10 s
  build  : Oct  7 2026 ...   heap_free: ... B
============================================
...（连 WiFi / TCP / MQTT 的 AT 过程日志）...
[net] ============== ONLINE ==============
[net] up stm32/up -> {"uptime":10,"seq":1}
[net] up stm32/up -> {"uptime":20,"seq":2}
```

接通后从任意 MQTT 客户端往 `stm32/down` 发下行命令，可在线改上报周期：

```
[net] down stm32/down = period=5
[net] cmd: period = 5
[demo] message on stm32/down, echo it back
[net] up stm32/up -> {"uptime":25,"seq":3}     ← 变成每 5 s 一次
```

断线时（例如把 EMQX 停掉）会自动恢复，不会卡死：

```
[net] keepalive timeout -> reconnect
[esp] exit transparent (+++)
[net] wifi still up -> reopen TCP only
[net] link up failed, retry in 5000 ms
...（每 5 s 重试一次，broker 恢复后自动 ONLINE 并继续上报）
```

---

## 6. 日志串口接线（USART1）

| USB-TTL | 单片机 | 说明 |
|---|---|---|
| **RX** | **PA9** | 单片机发、电脑收（TX→RX 交叉） |
| **TX** | **PA10** | 电脑发、单片机收 |
| GND | GND | **必须共地**，否则收到乱码或收不到 |
| 3V3（可选） | 3V3 | 板子已由别处供电时可不接 |

串口助手设置：**115200 / 8 数据位 / 无校验 / 1 停止位 / 无流控**。

`printf` 的重定向在 `Core/Src/main.c` 的 `USER CODE BEGIN 4`（`__io_putchar()` → `huart1`）。

**要打印自己的数据**：在任意任务里直接 `ESP_LOG("xxx\r\n")`。注意 newlib 的 stdio 不可重入——
同一时刻只让一个任务调用 `printf`，也**不要在中断里调用**。

**不用串口助手也能自检**（读 USART1 寄存器）：

```powershell
& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "init" -c "halt" -c "mdw 0x40021018 1" -c "mdw 0x40010004 1" `
       -c "mdw 0x40010804 1" -c "mdw 0x40013808 2" -c "resume" -c "shutdown"
```

| 寄存器 | 正常值 | 含义 |
|---|---|---|
| `RCC_APB2ENR` | `0x0000400D` | USART1 时钟 + GPIOA 时钟已开 |
| `AFIO_MAPR` | `0x02000000` | bit2(USART1_REMAP)=0 → 串口1在 PA9/PA10 |
| `GPIOA_CRH` | `0x888444B4` | PA9=0xB(复用推挽输出)，PA10=0x4(浮空输入) |
| `USART1_BRR` | `0x45` | 115942 bps ≈ 115200 |
| `USART1_CR1` | `0x0000200C` | UE/TE/RE 全部使能 |

---

## 7. 常见问题（FAQ）

**编译 / 脚本**

| 现象 | 原因 / 解决 |
|---|---|
| `'mingw32-make' 不是内部或外部命令` | PATH 里没有 GNU Make。装 MinGW 并把 `C:\MinGW\bin` 加进 PATH |
| `无法加载文件 flash.ps1，因为在此系统上禁止运行脚本` | PowerShell 执行策略。用 `flash.cmd`，或给 pwsh 加 `-ExecutionPolicy Bypass` |
| `flash.ps1` 报一堆 `Unexpected token` / 中文乱码 | 脚本没存成 **UTF-8 with BOM**。用「另存为 → UTF-8 带 BOM」重存 |
| `undefined reference to 'esp_net_init'` 之类的链接错误 | 新增的 `.c` 没加进 `Makefile.user`（见 §3.1） |
| `Makefile:108: *** CONFIG 只能是 Debug 或 Release，当前是 "xxx"` | `CONFIG=` 值拼错了，只能是 `Debug` 或 `Release` |
| `error: target CPU does not support ARM mode` | 缺 `-mthumb`（`Makefile` 已带；手敲命令时见 §3.4 ①） |
| `non constant or forward reference address expression for section .ARM.extab` | 链接脚本 `(READONLY)` 与老 binutils 不兼容（见 §3.4 ②） |
| `undefined reference to '_exit'` | 链接时缺 `-specs=nano.specs`，或没把 `Core/Src/syscalls.c` 加进源文件列表 |

**烧录 / ST-Link**

| 现象 | 原因 / 解决 |
|---|---|
| `Target voltage: 0.000000` | **先确认只插了一个 ST-Link**（插两个时 OpenOCD 可能打开没接板子那个）；其余见 §4.5 |
| 接线明明没错，电压却一直是 0 V | 同上：多个 ST-Link 抢设备，或克隆丝印标错（§4.5 ⓪①） |
| `Error: init mode failed` 但电压正常 | SWDIO/SWCLK 接反、速度太高、或芯片被读保护（加 `-Erase`） |
| OpenOCD 报找不到 cfg | 少了 `-s <scripts 目录>`，或 `-f interface/stlink.cfg` 路径写错 |

**串口 / 联网**

| 现象 | 原因 / 解决 |
|---|---|
| 串口助手收不到任何数据 | ① 接线：TTL 的 **RX 要接 PA9**（不是 PA10，收发要交叉）；② **GND 必须共地**；③ 确认 115200-8-N-1；④ 用 §6 的寄存器自检表确认 `AFIO_MAPR` bit2=0 |
| 串口收到乱码 | 波特率不匹配，或没共地 |
| 串口助手提示「拒绝访问 / 端口被占用」 | 该 COM 口已被别的软件打开（串口助手、另一个终端），同一时刻只能有一个程序占用 |
| 日志停在 `wifi join failed` | ESP 回 `+CWJAP:n`：**1=连接超时，2=密码错误，3=找不到该 AP，4=连接失败**。最常见的 **3** 是热点没开，或热点开在 **5 GHz**（ESP8266 只支持 2.4 GHz） |
| 上不了网但 WiFi 连上了 | 检查 Windows 移动热点是否开启：本机热点网卡应是 `192.168.137.1`；若本机 IPv4 只有别的网段，说明热点没开 |
| `[net] ESP8266 not responding` | 模块没上电/没复位。确认 PA8 上电后被拉低 300 ms 再拉高，以及 USART2 的 PA2/PA3 交叉接对。重连时出现则会自动跟进 `hardware reset (PA8)` 兜底 |
| 一直 `keepalive timeout -> reconnect` | broker 没回 PINGRESP：① 确认 `ESP_MQTT_HOST/PORT` 与 EMQX 监听端口一致（1883）；② 防火墙是否放行 1883；③ ESP 与 broker 是否同一网段 |
| 发到 `stm32/down` 的命令没反应 | ① 确认订阅成功（日志有 `mqtt subscribed`）；② 主题名与 `ESP_NET_TOPIC_CMD` 完全一致；③ 变量名要先 `esp_net_var_register()` 登记过；④ 载荷格式必须是 `名字=数值` |
| `mqtt CONNECT failed, status 1` | `1 = MQTT_ERR_PARAM`：`mqtt_client_t` 还没绑定传输层，检查有没有先调 `esp_net_init()` |
| `mqtt CONNECT failed, status 5` | `5 = MQTT_ERR_REFUSED`：broker 拒绝了连接。常见原因是 broker 开了鉴权而 `ESP_MQTT_USERNAME/PASSWORD` 留空，或 client_id 与别的在线客户端重复 |
| 断线重连后日志刷 `+++` 停不下来 | 这是「透传模式下把 AT 应答里的 `CLOSED` 误判成掉线」的经典坑。库已按模式区分处理（只在透传模式才认 `CLOSED`），**不要删掉 `esp8266_at.c` 里 `esp_at_watch_byte()` 开头的模式判断** |
| Espressif 模块串口2 上开头一段是乱码 | ESP8266 **启动日志是 74880 波特率**（在 115200 下就是乱码），之后 AT 固件才切到 115200，属正常现象 |

---

## 8. 附录：纯手打命令清单

```powershell
# ---- 进入本目录 ----
cd '<仓库根>\examples\stm32f103c8t6'

# ---- 编译 ----
mingw32-make                    # Debug（默认）
mingw32-make CONFIG=Release     # Release
mingw32-make -j8                # 并行
mingw32-make clean
# 或双击 build.cmd

# ---- 编译并烧录 ----
mingw32-make flash -j8

# ---- 只烧录（不重新编译）----
.\flash.cmd
.\flash.cmd -Config Release

# ---- 只检测连接 ----
.\flash.cmd -Probe

# ---- 不依赖脚本，全程手动 ----
$GCC = 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-gcc.exe'
$OBJ = 'D:\twBlock\toolchains\gcc-arm\bin\arm-none-eabi-objcopy.exe'
$OCD = 'D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\bin\openocd.exe'
$SD  = 'D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\openocd\scripts'

# （此处先按 §3.2 定义 $CPU/$WARN/$OPT/$DEF/$INC/$SRC/$LINK）
& $GCC @CPU @WARN @OPT @DEF @INC @SRC @LINK -o build/Debug/stm32103.elf
& $OBJ -O ihex   build/Debug/stm32103.elf build/Debug/stm32103.hex
& $OBJ -O binary build/Debug/stm32103.elf build/Debug/stm32103.bin

& $OCD -s $SD -f interface/stlink.cfg -f target/stm32f1x.cfg `
       -c "program build/Debug/stm32103.elf verify reset exit"
```

---

## 9. 改动本 demo 时要注意的几件事

1. **加 .c 文件要写进 `Makefile.user`**（`Makefile` 归 CubeMX 管，写进去会被重新生成覆盖）。
2. **任务必须静态创建**（`configTOTAL_HEAP_SIZE` 只有 3072 B），别用动态 `osThreadNew` 之外的方式另开栈。
3. **中断优先级必须 ≥ 5**（`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`）。
4. **`Core/` 下 `USER CODE BEGIN/END` 之外的代码会被 CubeMX 重新生成时覆盖**——PA8 配置、printf 重定向都在 USER CODE 段里，挪出去会丢。
5. 想彻底关掉本 demo 的业务、只留联网框架：删掉 `freertos.c` 里 `osThreadNew(demo_app_task, ...)` 那一行即可。
6. 想在不联网时手工敲 AT 指令排查：把 [demo_config.h](Core/Inc/demo_config.h) 的 `APP_BRIDGE_DEBUG_ENABLE` 改成 `1`，USART1 ↔ USART2 会变成一根原始双向透传线（此时联网任务不启动）。