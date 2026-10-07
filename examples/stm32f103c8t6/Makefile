##############################################################################
# stm32103 — STM32F103C8T6 + ESP8266 + MQTT（esp-net-framework 的 demo）
#
# 用法（在 examples/stm32f103c8t6/ 目录下执行）：
#   mingw32-make                    编译（默认 Debug：-O0 -g3 -DDEBUG）
#   mingw32-make CONFIG=Release     编译 Release（-Os -g0）
#   mingw32-make -j8                并行编译
#   mingw32-make clean              删除 build/
#   mingw32-make flash              编译后烧录（需 ST-Link + OpenOCD）
#
# 产物（都在 build/<配置>/ 下）：stm32103.elf / .hex / .bin / .map
#
# ── 维护约定（重要）────────────────────────────────────────────────────────
# 本文件的结构与 CubeMX 选「Makefile」工具链后生成的一致，是**可以重新生成**的。
# 因此：本框架自己新增的源文件和包含路径**不要写在本文件里**，
#       全部写到同目录的 Makefile.user，由下面那行 `-include Makefile.user` 合并进来。
# 这样就分清了「CubeMX 管的部分」和「我们管的部分」：
#   · CubeMX 重新生成 → 覆盖本文件，只需把 `-include Makefile.user` 这一行补回去；
#   · 我们自己的内容在 Makefile.user 里，不受影响。
# 详见仓库根目录 AGENTS.md §6。
##############################################################################

TARGET      = stm32103
BUILD_ROOT  = build

# Debug（-O0 -g3 -DDEBUG）或 Release（-Os -g0）。
# 两种配置各自输出到 build/Debug、build/Release，互不干扰
# （否则新旧目标文件时间戳会相互「看起来是最新的」，导致静默链接出混合产物）。
CONFIG     ?= Debug
BUILD_DIR   = $(BUILD_ROOT)/$(CONFIG)

# 工具链目录（换电脑时改这里，或命令行覆盖：mingw32-make TOOLCHAIN=/别的/bin）
TOOLCHAIN ?= D:/twBlock/toolchains/gcc-arm/bin

CC      = $(TOOLCHAIN)/arm-none-eabi-gcc
AS      = $(CC) -x assembler-with-cpp
CP      = $(TOOLCHAIN)/arm-none-eabi-objcopy
SZ      = $(TOOLCHAIN)/arm-none-eabi-size
HEX     = $(CP) -O ihex
BIN     = $(CP) -O binary -S

##############################################################################
# 源文件
##############################################################################
C_SOURCES =  \
Core/Src/main.c \
Core/Src/gpio.c \
Core/Src/freertos.c \
Core/Src/usart.c \
Core/Src/stm32f1xx_it.c \
Core/Src/stm32f1xx_hal_msp.c \
Core/Src/stm32f1xx_hal_timebase_tim.c \
Core/Src/sysmem.c \
Core/Src/syscalls.c \
Core/Src/system_stm32f1xx.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c \
Middlewares/Third_Party/FreeRTOS/Source/croutine.c \
Middlewares/Third_Party/FreeRTOS/Source/event_groups.c \
Middlewares/Third_Party/FreeRTOS/Source/list.c \
Middlewares/Third_Party/FreeRTOS/Source/queue.c \
Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c \
Middlewares/Third_Party/FreeRTOS/Source/tasks.c \
Middlewares/Third_Party/FreeRTOS/Source/timers.c \
Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c \
Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c \
Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c

ASM_SOURCES = startup_stm32f103xb.s

# 包含路径（CubeMX 管的部分）
C_INCLUDES =  \
-ICore/Inc \
-IDrivers/STM32F1xx_HAL_Driver/Inc \
-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy \
-IMiddlewares/Third_Party/FreeRTOS/Source/include \
-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3 \
-IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
-IDrivers/CMSIS/Include

# ★ 我们自己管的部分：esp-net-framework 的库、移植层、demo 业务。
#   必须放在 C_INCLUDES 定义之后。
-include Makefile.user

##############################################################################
# 编译选项
##############################################################################
# 默认 Debug；Release 用 mingw32-make CONFIG=Release
ifeq ($(CONFIG),Debug)
OPT     = -O0 -g3
C_DEFS += -DDEBUG
else ifeq ($(CONFIG),Release)
OPT     = -Os -g0
else
$(error CONFIG 只能是 Debug 或 Release，当前是 "$(CONFIG)")
endif

MCU     = -mcpu=cortex-m3 -mthumb

C_DEFS += -DUSE_HAL_DRIVER -DSTM32F103xB
AS_DEFS =

ASFLAGS = $(MCU) $(AS_DEFS) $(OPT) -Wall -fdata-sections -ffunction-sections
CFLAGS  = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

LDSCRIPT = STM32F103XX_FLASH.ld
LIBS     = -lc -lm -lnosys
LDFLAGS  = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBS) \
           -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
           -Wl,--gc-sections -Wl,--print-memory-usage

##############################################################################
# 目标
##############################################################################
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# Windows(cmd) 与类 Unix 的 mkdir/rmdir 写法不同（本机 MinGW 里没有 sh/rm）
ifeq ($(OS),Windows_NT)
MKDIR_BUILD = if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
CLEAN_BUILD = if exist "$(BUILD_ROOT)" rmdir /S /Q "$(BUILD_ROOT)"
else
MKDIR_BUILD = mkdir -p "$(BUILD_DIR)"
CLEAN_BUILD = rm -rf "$(BUILD_ROOT)"
endif

.PHONY: all clean flash

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

$(BUILD_DIR)/%.o: %.c Makefile Makefile.user | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile Makefile.user | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile Makefile.user
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	@$(MKDIR_BUILD)

# 编译 + 烧录（烧录脚本与构建方式无关，复用 flash.ps1）
flash: all
	powershell -NoProfile -ExecutionPolicy Bypass -File ./flash.ps1 -Config $(CONFIG)

# 注意：清掉整个 build/（含 Debug 与 Release 两套产物）
clean:
	@$(CLEAN_BUILD)