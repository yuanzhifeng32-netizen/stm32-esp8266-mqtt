<#
    cubemx-fix.ps1 —— CubeMX 重新生成代码后的自检与修补（由 build.cmd 自动调用）

    为什么需要它：用 CubeMX 重新生成代码时，它会用自己的模板**整体覆盖** Makefile
    和 STM32F103XX_FLASH.ld，把这个仓库做过的两处必要改动冲掉。不补回来会编译 /
    链接失败，而报错信息（undefined reference、链接脚本报错）并不直观。这个脚本
    把这两处自动补回，于是「CubeMX 生成完 → 直接跑 build.cmd」就能用。

    补什么（只动这两处，其余一概不碰）：
      1) Makefile 里加回 `-include Makefile.user`
         不补：框架的 4 个 .c 不参与编译 → undefined reference to esp_net_*
      2) STM32F103XX_FLASH.ld 里删掉段属性 (READONLY)
         不删：GCC 5.2.1 / binutils 2.25.90 链接报
               non constant or forward reference address expression

    没有需要修补的地方时，本脚本静默通过，不输出任何内容。
    详见 examples/stm32f103c8t6/README.md 的 3.3 / 3.4 节。
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$changed = @()

# Makefile 与链接脚本都不能带 BOM（BOM 会被 make / 链接器当成内容）
function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

# ─────────────────────────────────────────── 1) Makefile：补回 -include Makefile.user
$mk = Join-Path $Root 'Makefile'
if (-not (Test-Path $mk)) { throw "找不到 $mk，无法修补。" }

$mkText = [System.IO.File]::ReadAllText($mk) -replace "`r`n", "`n"

if ($mkText -notmatch '(?m)^\s*-include\s+Makefile\.user\s*$') {
    # 插入位置必须在 vpath 之前：Makefile.user 会往 C_SOURCES 里加源文件，
    # 而 vpath 要用 C_SOURCES 算搜索目录，晚于 vpath 就会找不到那些 .c
    $lines  = $mkText -split "`n"
    $anchor = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*vpath\s+%.c') { $anchor = $i; break }
    }
    if ($anchor -lt 0) {
        throw ("Makefile 里找不到锚点行 'vpath %.c'，无法定位插入位置。" +
               "请手工把 '-include Makefile.user' 加到 C_INCLUDES 定义之后" +
               "（见 examples/stm32f103c8t6/README.md 3.3）。")
    }
    $insert = @(
        '',
        '# ★ esp-net-framework：合并我们自己的源文件与包含路径（本行由 build.cmd 自动补回，勿删）',
        '-include Makefile.user',
        ''
    )
    $new = @($lines[0..($anchor - 1)]) + $insert + @($lines[$anchor..($lines.Count - 1)])
    Write-Utf8NoBom $mk ($new -join "`n")
    $changed += 'Makefile：补回 -include Makefile.user'
}

# ─────────────────────────── 2) 链接脚本：删掉段属性 (READONLY)
$ld = Join-Path $Root 'STM32F103XX_FLASH.ld'
if (Test-Path $ld) {
    $ldText  = [System.IO.File]::ReadAllText($ld)
    # 只匹配作为「段属性」出现的 (READONLY)（后面跟着冒号的那种），
    # 不会误伤注释里提到的 (READONLY) 字样
    $patched = [regex]::Replace($ldText, '\s*\(READONLY\)\s*:', ' :')
    if ($patched -ne $ldText) {
        Write-Utf8NoBom $ld $patched
        $changed += 'STM32F103XX_FLASH.ld：删除段属性 (READONLY)'
    }
}

if ($changed.Count -gt 0) {
    Write-Host '[cubemx-fix] 检测到 CubeMX 重新生成过的痕迹，已自动补回：' -ForegroundColor Yellow
    $changed | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
    Write-Host '[cubemx-fix] 原因见 examples/stm32f103c8t6/README.md 3.3 / 3.4' -ForegroundColor Yellow
}