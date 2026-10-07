<#
    stm32103 (STM32F103C8T6) —— 用 ST-Link + OpenOCD 烧录脚本

    用法：
        pwsh -File .\flash.ps1                  # 烧录 build\Debug\stm32103.elf（含校验、复位）
        pwsh -File .\flash.ps1 -Config Release
        pwsh -File .\flash.ps1 -Probe           # 只检测 ST-Link / 芯片连接，不烧录
        pwsh -File .\flash.ps1 -Erase           # 先全片擦除再烧录（解除读写保护后常用）
        pwsh -File .\flash.ps1 -File build\Debug\stm32103.hex   # 指定任意固件文件

    接线（ST-Link V2 → 单片机）：
        SWDIO  → PA13 (SWDIO)
        SWCLK  → PA14 (SWCLK)
        GND    → GND
        3.3V   → 3V3      ← 必须接，ST-Link 靠它检测目标电压

    若 OpenOCD 不在默认位置，先设置环境变量：
        $env:OPENOCD_EXE = 'D:\path\to\openocd.exe'
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Debug',
    [switch]$Probe,
    [switch]$Erase,
    [string]$File
)

$ErrorActionPreference = 'Stop'
$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

# ---------------------------------------------------------------- OpenOCD 定位
$candidates = @(
    $env:OPENOCD_EXE,
    'D:\twBlock\toolchains\xpack-openocd-0.12.0-7-win32-x64\bin\openocd.exe',
    'C:\Program Files\OpenOCD\bin\openocd.exe',
    'C:\xpack-openocd\bin\openocd.exe'
) | Where-Object { $_ }

$OCD = $null
foreach ($c in $candidates) {
    if (Test-Path $c) { $OCD = $c; break }
}
if (-not $OCD) {
    $cmd = Get-Command openocd -ErrorAction SilentlyContinue
    if ($cmd) { $OCD = $cmd.Source } else { throw "找不到 openocd.exe，请设置环境变量 OPENOCD_EXE。" }
}

# 查找脚本目录（interface/target 配置）
$ocdRoot = Split-Path (Split-Path $OCD -Parent) -Parent
$scriptDirs = @(
    (Join-Path $ocdRoot 'openocd\scripts'),          # xpack 布局
    (Join-Path $ocdRoot 'share\openocd\scripts'),    # 官方安装布局
    (Join-Path (Split-Path $OCD -Parent) 'scripts')
)
$Scripts = $scriptDirs | Where-Object { Test-Path (Join-Path $_ 'interface\stlink.cfg') } | Select-Object -First 1
if (-not $Scripts) { throw "找不到 OpenOCD 脚本目录（interface/stlink.cfg）。" }

Push-Location $Root
try {
    if ($Probe) {
        Write-Host "--- 检测 ST-Link 与目标芯片 ---" -ForegroundColor Cyan
        & $OCD -s $Scripts -f interface/stlink.cfg -f target/stm32f1x.cfg -c "init; targets; shutdown"
        exit $LASTEXITCODE
    }

    $Target = if ($File) { $File } else { "build/$Config/stm32103.elf" }
    if (-not (Test-Path $Target)) { throw "找不到固件 $Target，请先运行 mingw32-make 编译。" }
    # OpenOCD 对含中文的绝对路径不友好，统一转成相对路径（脚本已切到工程根目录）
    $Target = (Resolve-Path $Target).Path.Substring($Root.Length).TrimStart('\', '/') -replace '\\', '/'

    Write-Host "OpenOCD : $OCD" -ForegroundColor Cyan
    Write-Host "固件    : $Target" -ForegroundColor Cyan

    $ocdArgs = @('-s', $Scripts, '-f', 'interface/stlink.cfg', '-f', 'target/stm32f1x.cfg')
    if ($Erase) {
        Write-Host "先执行全片擦除 ..." -ForegroundColor Yellow
        $ocdArgs += @('-c', 'init', '-c', 'reset halt', '-c', 'stm32f1x mass_erase 0')
    }
    $ocdArgs += @('-c', "program $Target verify reset exit")

    & $OCD @ocdArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`n烧录失败。请检查：" -ForegroundColor Red
        Write-Host "  1) 接线：SWDIO/SWCLK/GND/3V3 是否都接好（3V3 必须接）"
        Write-Host "  2) 单片机是否已供电（OpenOCD 报 Target voltage 0.000000 即未供电）"
        Write-Host "  3) 芯片是否被读保护：用 -Erase 参数重试"
        throw "OpenOCD 退出码 $LASTEXITCODE"
    }
    Write-Host "`n烧录完成并校验通过，芯片已复位运行。" -ForegroundColor Green
}
finally {
    Pop-Location
}
