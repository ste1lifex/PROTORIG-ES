<#
.SYNOPSIS
    把编译好的固件烧进 PROTORIG ES V1.4 板子。

.DESCRIPTION
    用工程 build\ 目录里的 flash_prog_cfg.ini（含 boot2 / partition / 应用三块），
    调项目自带的 tools\BLFlashCommand.exe 通过 ROM 下载模式写入。

    ⚠️ 重要：烧完之后必须**手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电）**才会跑新固件！
        ROM 进入下载模式后会把状态记在 AON 域，BLFlashCommand 的 --reset
       （Reset cpu）和 --warm_reset 都清不掉，只有真正拨到 OFF 再 ON 才行。

    所以完整流程是：
      1. 让正在跑的应用跳回下载模式：.\flash.ps1 <工程> -AppPort COM8
         （或者手动按住 SW3 再按 SW1）
      2. 脚本自动烧录
      3. 人断电重上电

.PARAMETER Project
    工程目录名（要先 .\build.ps1 编过）

.PARAMETER Port
    ROM 下载模式的串口，通常 COM7

.PARAMETER AppPort
    可选。如果应用正在跑（通常 COM8），先往这个口发 `reboot uart`，
    芯片会自己跳回 ROM 下载模式，不用碰板子。

.PARAMETER Baud
    烧录波特率，默认 2000000

.EXAMPLE
    .\flash.ps1 bringup -AppPort COM8
    .\flash.ps1 screen_test -Port COM7
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Project,
    [string]$Port = 'COM7',
    [string]$AppPort = '',
    [int]$Baud = 2000000,
    [switch]$Force
)

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
$Sdk  = $env:BL_SDK_BASE
$Make = Join-Path $FwRoot 'tools\make'
$Proj = Join-Path $FwRoot $Project
$Tool = Join-Path $FwRoot 'tools\BLFlashCommand.exe'

foreach ($p in @($Sdk, $Make, $Proj, $Tool)) {
    if (-not (Test-Path $p)) { Write-Host "找不到：$p" -ForegroundColor Red; exit 1 }
}

$Cfg = Join-Path $Proj 'build\flash_prog_cfg.ini'
if (-not (Test-Path $Cfg)) {
    Write-Host "没有 $Cfg" -ForegroundColor Red
    Write-Host "先编译：.\build.ps1 $Project" -ForegroundColor Yellow
    exit 1
}

# ---------- 陈旧二进制检查 ----------
# 编译失败时 build_out 里会留着上一次的 .bin，不检查就会把旧固件烧进去
#（这个坑真的踩过一次）。所以确认 .bin 不比源码旧。
$bin = Get-ChildItem (Join-Path $Proj 'build\build_out\*.bin') -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -notmatch 'boot2|partition|efusedata|dwarfcfi' } |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $bin) {
    Write-Host "build_out 里没有 .bin，先编译。" -ForegroundColor Red
    exit 1
}
$newestSrc = Get-ChildItem $Proj -File -ErrorAction SilentlyContinue |
             Where-Object { $_.Extension -in '.c', '.h' } |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSrc -and $newestSrc.LastWriteTime -gt $bin.LastWriteTime) {
    Write-Host "!! .bin 比源码旧，说明编译失败或者根本没编过。" -ForegroundColor Red
    Write-Host "     bin  : $($bin.Name)  $($bin.LastWriteTime)" -ForegroundColor Gray
    Write-Host "     源码 : $($newestSrc.Name)  $($newestSrc.LastWriteTime)" -ForegroundColor Gray
    Write-Host "   先跑：.\build.ps1 $Project" -ForegroundColor Yellow
    Write-Host "   （确实想烧旧的，加 -Force）" -ForegroundColor Yellow
    if (-not $Force) { exit 1 }
}
Write-Host "将烧录：$($bin.Name)  ($([math]::Round($bin.Length / 1KB, 1)) KB, $($bin.LastWriteTime))" -ForegroundColor Cyan

# ---------- 可选：先让应用跳回下载模式 ----------
if ($AppPort -ne '') {
    Write-Host "[1/2] 往 $AppPort 发 'reboot uart'，让芯片跳回 ROM 下载模式..." -ForegroundColor Cyan
    try {
        $p = New-Object System.IO.Ports.SerialPort
        $p.PortName = $AppPort; $p.BaudRate = 115200
        $p.Parity = [System.IO.Ports.Parity]::None; $p.DataBits = 8
        $p.StopBits = [System.IO.Ports.StopBits]::One
        $p.Handshake = [System.IO.Ports.Handshake]::None
        $p.DtrEnable = $true; $p.RtsEnable = $true; $p.ReadTimeout = 500
        $p.Open(); Start-Sleep -Milliseconds 300
        $p.ReadExisting() | Out-Null
        $p.Write("reboot uart`r`n")
        Start-Sleep -Milliseconds 700
        $p.Close()
        Write-Host "      已发送，等 6 秒让 ROM 起来..." -ForegroundColor Gray
        Start-Sleep -Seconds 6
    } catch {
        Write-Host "      发不出去（$($_.Exception.Message)）。" -ForegroundColor Yellow
        Write-Host "      那就手动：按住 SW3(BOOT) 再按一下 SW1(RST)，然后松开 SW3。" -ForegroundColor Yellow
    }
}

# ---------- 烧录 ----------
Write-Host "[2/2] 烧录到 $Port ..." -ForegroundColor Cyan
$env:PATH = "$Make;$Sdk\tools\cmake\bin;" + $env:PATH

Push-Location $Proj
try {
    & mingw32-make flash CHIP=bl616 BOARD=bl616dk COMX=$Port BAUDRATE=$Baud PYTHON=python `
        "BL_FLASH_PROGRAM=$Tool"
    $rc = $LASTEXITCODE
}
finally { Pop-Location }

Write-Host ""
if ($rc -eq 0) {
    Write-Host "烧录成功" -ForegroundColor Green
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Yellow
    Write-Host " 现在请【手动拨动开关重启】：" -ForegroundColor Yellow
    Write-Host "   SW2 拨到 ON → OFF → ON 再上电（拔 USB 不会断开电池供电）。" -ForegroundColor Yellow
    Write-Host " 软复位清不掉 ROM 的下载模式，只有真断电才行。" -ForegroundColor Yellow
    Write-Host "================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "上电后应用会以一个虚拟串口出现（设备描述 'Bouffalo Lab USB Console'），" -ForegroundColor Gray
    Write-Host "串口号可能不再是 COM7/COM8，用下面这条看：" -ForegroundColor Gray
    Write-Host '  Get-CimInstance Win32_PnPEntity | ? { $_.DeviceID -like "*VID_349B*" } | select Name,DeviceID' -ForegroundColor Gray
} else {
    Write-Host "烧录失败 (exit=$rc)" -ForegroundColor Red
    Write-Host "常见原因：" -ForegroundColor Yellow
    Write-Host "  - 板子不在下载模式：先 -AppPort COM8，或按住 SW3 再按 SW1"
    Write-Host "  - 串口号不对：-Port COMx（下载模式下面设备描述是 'Bouffalo CDC DEMO'）"
}
