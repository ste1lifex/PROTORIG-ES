<#
.SYNOPSIS
    编译 PROTORIG ES V1.4 的某个固件工程。

.DESCRIPTION
    自动把 SDK 的 make / cmake / 交叉工具链 加进 PATH，然后调 mingw32-make。
    路径都可以用环境变量覆盖，见下面。

.PARAMETER Project
    工程目录名，例如 bringup / screen_test / sensor_test

.PARAMETER Clean
    先 make clean 再编

.EXAMPLE
    .\build.ps1 bringup
    .\build.ps1 screen_test -Clean

.NOTES
    可用环境变量覆盖（不设就用默认值）：
      环境变量 BL_SDK_BASE / BL_TOOLCHAIN_DIR / BL_PYTHON_DIR；
      未设置时自动读 firmware/dev-env.json（不入库，见 firmware/README.md）
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Project,
    [switch]$Clean
)

# 注意：这里故意用 Continue 而不是 Stop。
# 原生命令（cmake/make）会往 stderr 写进度和警告，EA=Stop 会把它们
# 当成终止错误，导致明明编译成功却报错退出。改成显式检查 $LASTEXITCODE。
$ErrorActionPreference = 'Continue'
$FwRoot = $PSScriptRoot

# 本机开发路径不入库：优先环境变量；其次读 firmware/dev-env.json（见 firmware/README.md）。
$DevEnvFile = Join-Path $PSScriptRoot 'dev-env.json'
if (Test-Path -LiteralPath $DevEnvFile) {
    $DevEnv = Get-Content -LiteralPath $DevEnvFile -Raw | ConvertFrom-Json
    foreach ($devName in 'BL_SDK_BASE','BL_TOOLCHAIN_DIR','BL_PYTHON_DIR') {
        if (-not [Environment]::GetEnvironmentVariable($devName) -and
            $DevEnv.PSObject.Properties.Name -contains $devName) {
            Set-Item -Path "env:$devName" -Value $DevEnv.$devName
        }
    }
}

if (-not $env:BL_SDK_BASE) { Write-Host "BL_SDK_BASE 未设置：请设环境变量，或在 firmware/dev-env.json 里配置（见 firmware/README.md）" -ForegroundColor Red; exit 1 }
if (-not $env:BL_TOOLCHAIN_DIR) { Write-Host "BL_TOOLCHAIN_DIR 未设置：请设环境变量，或在 firmware/dev-env.json 里配置（见 firmware/README.md）" -ForegroundColor Red; exit 1 }
if (-not $env:BL_PYTHON_DIR) { Write-Host "BL_PYTHON_DIR 未设置：请设环境变量，或在 firmware/dev-env.json 里配置（见 firmware/README.md）" -ForegroundColor Red; exit 1 }

$Sdk  = $env:BL_SDK_BASE
$Tc   = $env:BL_TOOLCHAIN_DIR
$Py   = $env:BL_PYTHON_DIR
$Make = Join-Path $FwRoot 'tools\make'
$Proj = Join-Path $FwRoot $Project

# ---------- 环境检查 ----------
$missing = @()
foreach ($p in @($Sdk, $Tc, $Py, $Make, $Proj)) {
    if (-not (Test-Path $p)) { $missing += $p }
}
if ($missing.Count -gt 0) {
    Write-Host "找不到下面这些路径：" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "如果是 SDK / 工具链 / Python 换了位置，用环境变量覆盖：" -ForegroundColor Yellow
    Write-Host '  $env:BL_SDK_BASE      = "D:\path\to\bouffalo_sdk"'
    Write-Host '  $env:BL_TOOLCHAIN_DIR = "D:\path\to\toolchain_gcc_t-head_windows"'
    Write-Host '  $env:BL_PYTHON_DIR    = "D:\path\to\python"'
    exit 1
}
if (-not (Test-Path (Join-Path $Proj 'Makefile'))) {
    Write-Host "$Proj 里没有 Makefile，不是一个固件工程。" -ForegroundColor Red
    Write-Host "现有工程：" -ForegroundColor Yellow
    Get-ChildItem $FwRoot -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'Makefile') } |
        ForEach-Object { Write-Host "  $($_.Name)" }
    exit 1
}

# 2026-11-15：工程自带 build.ps1 时优先用它。
# lvgl_test/build.ps1 会在编译期间临时给 SDK 打 usb_pc_rx / usb_console_delay
# 补丁（收尾自动还原），本文件是 V1.4 时代的通用包装，不干这件事；
# 直接走通用流程会缺 protorig_usb_console_in_events 而链接失败。
$ProjectBuild = Join-Path $Proj 'build.ps1'
if (Test-Path $ProjectBuild) {
    Write-Host "工程自带构建脚本：$ProjectBuild（含 SDK 临时补丁）" -ForegroundColor Cyan
    & $ProjectBuild -Clean:$Clean
    exit $LASTEXITCODE
}

# ---------- PATH ----------
$env:PATH = "$Make;$Sdk\tools\cmake\bin;$Tc\bin;$Py;" + $env:PATH

Write-Host "SDK      : $Sdk"
Write-Host "工具链   : $Tc"
Write-Host "工程     : $Project"
Write-Host ""

Push-Location $Proj
try {
    if ($Clean) {
        Write-Host "[clean]" -ForegroundColor Yellow
        & mingw32-make clean CHIP=bl616 BOARD=bl616dk PYTHON=python | Out-Null
    }

    Write-Host "[build] mingw32-make ..." -ForegroundColor Cyan
    & mingw32-make CHIP=bl616 BOARD=bl616dk PYTHON=python
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host ""
        Write-Host "编译失败 (exit=$rc)" -ForegroundColor Red
        Pop-Location
        exit $rc
    }

    $bin = Get-ChildItem 'build\build_out\*.bin' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'boot2|partition|efusedata|dwarfcfi' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1

    Write-Host ""
    if ($bin) {
        Write-Host "编译成功" -ForegroundColor Green
        Write-Host "  产物 : $($bin.FullName)"
        Write-Host "  大小 : $([math]::Round($bin.Length / 1KB, 1)) KB   时间: $($bin.LastWriteTime)"
        Write-Host ""
        Write-Host "下一步：.\flash.ps1 $Project -Port COM7" -ForegroundColor Cyan
    } else {
        Write-Host "编译成功，但没找到 .bin 产物，请检查 build\build_out\" -ForegroundColor Yellow
    }
}
finally { Pop-Location }
