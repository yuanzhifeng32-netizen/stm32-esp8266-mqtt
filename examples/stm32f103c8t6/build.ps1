<#
    stm32103 (STM32F103C8T6) —— 手动编译脚本（不依赖 CMake）

    用法：
        pwsh -File .\build.ps1                    # 编译 Debug（-O0 -g3）
        pwsh -File .\build.ps1 -Config Release     # 编译 Release（-Os -g0）
        pwsh -File .\build.ps1 -Clean              # 先删除输出目录再编译

    产物统一放在 build\<Config>\ 下：
        stm32103.elf   ST-Link / OpenOCD 烧录用
        stm32103.hex   Intel HEX（ST-Link Utility / STM32CubeProgrammer / 串口 ISP 可用）
        stm32103.bin   纯二进制（串口 ISP / 通用烧录器可用）
        stm32103.map   链接映射表

    若 GCC 不在默认位置，先设置环境变量：
        $env:ARM_GCC_BIN = 'D:\path\to\gcc-arm\bin'
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Debug',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$OutDir = "build/$Config"
$Target = 'stm32103'

# ---------------------------------------------------------------- 工具链定位
$candidates = @(
    $env:ARM_GCC_BIN,
    'D:\twBlock\toolchains\gcc-arm\bin',
    'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.3.rel1\bin',
    'C:\Program Files\Arm GNU Toolchain arm-none-eabi\13.3.rel1\bin',
    'C:\Program Files (x86)\GNU Arm Embedded Toolchain\10 2021.10\bin'
) | Where-Object { $_ }

$ToolBin = $null
foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c 'arm-none-eabi-gcc.exe')) { $ToolBin = $c; break }
}

if ($ToolBin) {
    $GCC     = Join-Path $ToolBin 'arm-none-eabi-gcc.exe'
    $OBJCOPY = Join-Path $ToolBin 'arm-none-eabi-objcopy.exe'
    $SIZE    = Join-Path $ToolBin 'arm-none-eabi-size.exe'
} elseif (Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue) {
    $GCC = 'arm-none-eabi-gcc'; $OBJCOPY = 'arm-none-eabi-objcopy'; $SIZE = 'arm-none-eabi-size'
} else {
    throw "找不到 arm-none-eabi-gcc，请设置环境变量 ARM_GCC_BIN 指向工具链 bin 目录。"
}

# ---------------------------------------------------------------- 源文件清单
# 与 cmake/stm32cubemx/CMakeLists.txt 中的 MX_Application_Src / STM32_Drivers_Src / FreeRTOS_Src 保持一致
$Sources = @(
    # 应用层（CubeMX 生成）
    'Core/Src/main.c'
    'Core/Src/gpio.c'
    'Core/Src/freertos.c'
    'Core/Src/usart.c'
    'Core/Src/stm32f1xx_it.c'
    'Core/Src/stm32f1xx_hal_msp.c'
    'Core/Src/stm32f1xx_hal_timebase_tim.c'
    'Core/Src/sysmem.c'
    'Core/Src/syscalls.c'
    'Core/Src/system_stm32f1xx.c'
    # 用户业务（demo：周期上报 + 下行命令）
    'Core/Src/demo_app.c'
    # ESP8266 联网框架（可移植的库，别人只拷 ../library 和 ../port）
    '../../library/esp8266_at.c'
    '../../library/mqtt_client.c'
    '../../library/esp_net.c'
    # 移植层（STM32F1 + HAL 参考实现）
    '../../port/esp_port_stm32f1.c'
    # STM32F1 HAL 驱动
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_tim_ex.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_exti.c'
    'Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_uart.c'
    # FreeRTOS 内核
    'Middlewares/Third_Party/FreeRTOS/Source/croutine.c'
    'Middlewares/Third_Party/FreeRTOS/Source/event_groups.c'
    'Middlewares/Third_Party/FreeRTOS/Source/list.c'
    'Middlewares/Third_Party/FreeRTOS/Source/queue.c'
    'Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c'
    'Middlewares/Third_Party/FreeRTOS/Source/tasks.c'
    'Middlewares/Third_Party/FreeRTOS/Source/timers.c'
    'Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2/cmsis_os2.c'
    'Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c'
    'Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3/port.c'
    # 启动文件（汇编，GCC 会按扩展名 .s 自动识别）
    'startup_stm32f103xb.s'
)

$Includes = @(
    '-ICore/Inc'
    # ESP8266 联网框架与移植层（在上述 $Sources 里以相对路径引用）
    '-I../../library'
    '-I../../port'
    '-IDrivers/STM32F1xx_HAL_Driver/Inc'
    '-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy'
    '-IMiddlewares/Third_Party/FreeRTOS/Source/include'
    '-IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2'
    '-IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM3'
    '-IDrivers/CMSIS/Device/ST/STM32F1xx/Include'
    '-IDrivers/CMSIS/Include'
)

$Defines = @('-DUSE_HAL_DRIVER', '-DSTM32F103xB')
if ($Config -eq 'Debug') { $Defines += '-DDEBUG' }

# Cortex-M3 是纯 Thumb 内核，-mthumb 必须显式给出
$CpuFlags = @('-mcpu=cortex-m3', '-mthumb')
$WarnFlags = @('-Wall', '-fdata-sections', '-ffunction-sections', '-std=gnu11')
$OptFlags = if ($Config -eq 'Debug') { @('-O0', '-g3') } else { @('-Os', '-g0') }

$LinkFlags = @(
    '-T', 'STM32F103XX_FLASH.ld'
    '--specs=nano.specs'
    "-Wl,-Map=$OutDir/$Target.map"
    '-Wl,--gc-sections'
    '-Wl,--print-memory-usage'
    '-lm'
)

# ---------------------------------------------------------------- 开始编译
Push-Location $Root
try {
    if ($Clean -and (Test-Path $OutDir)) { Remove-Item -Recurse -Force $OutDir }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    Write-Host "工具链 : $GCC" -ForegroundColor Cyan
    Write-Host "配置   : $Config" -ForegroundColor Cyan
    Write-Host "输出   : $OutDir/$Target.elf" -ForegroundColor Cyan
    Write-Host "编译中 ..." -ForegroundColor Cyan

    & $GCC @CpuFlags @WarnFlags @OptFlags @Defines @Includes @Sources @LinkFlags -o "$OutDir/$Target.elf"
    if ($LASTEXITCODE -ne 0) { throw "编译/链接失败（exit $LASTEXITCODE）" }

    & $OBJCOPY -O ihex   "$OutDir/$Target.elf" "$OutDir/$Target.hex"
    & $OBJCOPY -O binary "$OutDir/$Target.elf" "$OutDir/$Target.bin"

    Write-Host "`n=== 段大小 ===" -ForegroundColor Green
    & $SIZE "$OutDir/$Target.elf"

    Write-Host "`n=== 产物 ===" -ForegroundColor Green
    Get-ChildItem $OutDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
    Write-Host "编译成功。烧录：pwsh -File .\flash.ps1 -Config $Config" -ForegroundColor Green
}
finally {
    Pop-Location
}
