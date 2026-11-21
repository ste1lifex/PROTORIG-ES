<#
.SYNOPSIS
    PROTORIG ES V1.5 一键板上验证（UI + 字体 + RTC + CST9220 低功耗调研结论）

.DESCRIPTION
    板子上电后跑这一条，自动完成：

      [1] 发 `reboot` 触发重启，抓完整启动日志
          -> 验证 EEZ 三页初始化、字库加载、SD3010 RTC
      [2] tpmode       读 0x0002 模式回显（子代理报告里的天然自检寄存器）
      [3] tpstat       触摸统计
      [4] 静置 20s 再 tpstat
          -> 验证 INT 门控在**空闲期**到底省了多少 I2C 流量
      [5] tpnormal     写 D1 09（厂商手册要求，我们和厂商驱动原先都漏了）
      [6] tpgesture 1  序列 A：D0 4C 80 手势模式 + 0xD04C bit7 自检
      [7] tpgesture 0  退出
      [8] rtcstat      SD3010 真实时间
      [9] screen 0/1   熄屏/亮屏

    依据：firmware/docs/cst9220_lowpower_investigation.md

.EXAMPLE
    .\board_verify.ps1
    .\board_verify.ps1 -Port COM8
#>
param(
    [string]$Port = '',
    [int]$IdleSeconds = 20
)

$ErrorActionPreference = 'Continue'
$FwRoot = Split-Path -Parent $PSScriptRoot
$LogDir = Join-Path $FwRoot 'tool_outputs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir ("board_verify_" + (Get-Date -Format 'yyyyMMdd_HHmmss') + ".log")

# ---------- 找应用串口 ----------
if (-not $Port) {
    $cand = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
            Where-Object { $_.FriendlyName -match 'USB 串行设备|USB Serial|Bouffalo' -and $_.FriendlyName -notmatch 'CDC DEMO' }
    if ($cand) {
        if ($cand.FriendlyName -match '\(COM(\d+)\)') { $Port = "COM$($Matches[1])" }
    }
}
if (-not $Port) {
    # 退一步：找 VID_349B 的端口
    $d = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
         Where-Object { $_.DeviceID -like '*VID_349B*' -and $_.Name -match '\(COM(\d+)\)' }
    if ($d) { if ($d[0].Name -match '\(COM(\d+)\)') { $Port = "COM$($Matches[1])" } }
}
if (-not $Port) {
    Write-Host "找不到应用串口 —— 板子还没上电？(下载模式是 CDC DEMO，不是这个)" -ForegroundColor Red
    Write-Host "当前串口：" -ForegroundColor Gray
    Get-PnpDevice -Class Ports -Status OK | Select-Object -ExpandProperty FriendlyName | ForEach-Object { Write-Host "   $_" }
    exit 1
}

$lines = New-Object System.Collections.Generic.List[string]
function Say($s) { Write-Host $s; $lines.Add($s) }
function Section($s) { Say ""; Say ("=" * 66); Say $s; Say ("=" * 66) }

Section "PROTORIG ES V1.5 板上验证   端口=$Port    $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port; $sp.BaudRate = 115200
$sp.Parity = 'None'; $sp.DataBits = 8; $sp.StopBits = 'One'
$sp.Handshake = 'None'; $sp.DtrEnable = $true; $sp.RtsEnable = $true
$sp.ReadTimeout = 200

function Drain([int]$ms) {
    $sb = New-Object System.Text.StringBuilder
    $t = (Get-Date).AddMilliseconds($ms)
    while ((Get-Date) -lt $t) {
        try { $s = $sp.ReadExisting(); if ($s) { [void]$sb.Append($s) } } catch {}
        Start-Sleep -Milliseconds 50
    }
    return $sb.ToString()
}
function Cmd([string]$c, [int]$ms = 1200) {
    Say ""
    Say ">>> $c"
    $sp.Write("$c`r`n")
    $out = Drain $ms
    if ($out) { $out.TrimEnd() -split "`r?`n" | ForEach-Object { Say "    $_" } }
    else { Say "    (无回显)" }
}

try { $sp.Open() } catch { Write-Host "打开 $Port 失败: $_" -ForegroundColor Red; exit 1 }
Start-Sleep -Milliseconds 300
$sp.ReadExisting() | Out-Null

# ---- [1] 重启抓启动日志 ----
Section "[1] 启动日志（发 reboot 触发）"
$sp.Write("reboot`r`n")
$boot = Drain 9000
Say $boot.TrimEnd()
Say ""
Say "--- 启动关键点检查 ---"
foreach ($k in @('[rtc]', 'RTC =', '[tp]', '[ui]', 'ready.', '[power]', 'checkcode', 'mode echo')) {
    $hit = ($boot -split "`r?`n" | Where-Object { $_ -match [regex]::Escape($k) } | Select-Object -First 2)
    if ($hit) { $hit | ForEach-Object { Say "    [OK] $_" } }
    else { Say "    [--] 没看到 '$k'" }
}

# ---- [2..9] 命令序列 ----
Section "[2] 模式回显（报告 §5.2 的天然自检寄存器 0x0002）"
Cmd 'tpmode'

Section "[3] 触摸统计（含 INT 门控计数）"
Cmd 'tpstat'

Section "[4] 静置 $IdleSeconds 秒 —— 验证 INT 门控空闲期省了多少"
Say "    （不要碰屏幕）"
Start-Sleep -Seconds $IdleSeconds
Cmd 'tpstat'

Section "[5] 写回 D1 09 NORMAL（厂商手册要求，原先我们和厂商驱动都漏了）"
Cmd 'tpnormal'

Section "[6] 序列 A：手势模式 D0 4C 80 + 0xD04C bit7 自检"
Cmd 'tpgesture 1' 2000
Say ""
Say "    >>> 现在请在屏幕上画一个 Z 或 O，然后回车继续"
Read-Host "    按回车继续" | Out-Null
Cmd 'tpstat'

Section "[7] 退出手势模式"
Cmd 'tpgesture 0' 2000

Section "[8] SD3010 RTC"
Cmd 'rtcstat'

Section "[9] 熄屏 / 亮屏"
Cmd 'screen 0' 800
Cmd 'screen 1' 800

Section "[10] 接近感应（序列 D，试一下）"
Cmd 'tpprox 1' 1500
Cmd 'tpprox 0' 1500

$sp.Close()

Section "完成   日志已存: $Log"
[System.IO.File]::WriteAllLines($Log, $lines)
Write-Host ""
Write-Host "报告: $Log" -ForegroundColor Cyan
