<#
.SYNOPSIS
    从板子的 USB CDC 虚拟串口抓一段日志，可选先发几条 shell 命令。

.DESCRIPTION
    应用跑起来以后会出现一个虚拟串口（设备描述 "Bouffalo Lab USB Console"）。
    CDC 不看波特率，这里填 115200 只是占位。

    串口号会变：下载模式 = "Bouffalo CDC DEMO"，应用 = "Bouffalo Lab USB Console"。
    用下面这条找：

        Get-CimInstance Win32_PnPEntity | ? { $_.DeviceID -like "*VID_349B*" } |
            select Name, DeviceID

.EXAMPLE
    .\serial_capture.ps1 -Port COM8 -Seconds 25 -Out wifi.log
    .\serial_capture.ps1 -Port COM8 -Seconds 15 -Send "wifitest" -Out wifitest.log
#>
param(
    [string]$Port = 'COM8',
    [int]$Seconds = 20,
    [string]$Out = 'capture.txt',
    [string[]]$Send = @(),
    [int]$SendDelayMs = 800
)

$ErrorActionPreference = 'Continue'

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 500
$sp.NewLine = "`r`n"

try {
    $sp.Open()
} catch {
    Write-Host "打不开 $Port : $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Write-Host "已打开 $Port，抓 ${Seconds}s ..." -ForegroundColor Cyan

$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds($Seconds)
$sent = 0

while ((Get-Date) -lt $deadline) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) {
            [void]$sb.Append($chunk)
            Write-Host -NoNewline $chunk
        }
    } catch { }

    # 抓够 SendDelayMs 之后开始依次发命令
    if ($sent -lt $Send.Count -and ((Get-Date).AddSeconds($Seconds) - $deadline).TotalMilliseconds -ge 0) {
        Start-Sleep -Milliseconds 200
        $cmd = $Send[$sent]
        Write-Host "`r`n>>> $cmd" -ForegroundColor Yellow
        $sp.Write("$cmd`r`n")
        $sent++
        Start-Sleep -Milliseconds $SendDelayMs
    } else {
        Start-Sleep -Milliseconds 60
    }
}

$sp.Close()

$text = $sb.ToString()
Set-Content -Path $Out -Value $text -Encoding utf8
Write-Host "`r`n--- 一共 $($text.Length) 字符，已存到 $Out ---" -ForegroundColor Green
