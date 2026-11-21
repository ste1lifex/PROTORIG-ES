<#
com_watch.ps1 —— 抓 bootloop / BLE 初始化串口日志（快速版）

改进：
- 不再在读取循环里调 WMI（Get-CimInstance 会把 USB 日志拖慢到每行 ~250ms）；
- 用 ReadExisting 一次性排空缓冲区，逐块写盘；
- 只用 GetPortNames 检测掉线（每 10ms 一次，很便宜）；
- 每次打开最多抓 60s，掉线自动保存并继续等下一次。

用法：
  .\tools\com_watch.ps1 -Seconds 240 -Tag r4-dbg1c
#>
param(
    [int]$Seconds = 240,
    [int]$Baud = 115200,
    [string]$Tag = 'bootwatch',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
if ($OutDir -eq '') {
    $OutDir = Join-Path (Split-Path -Parent $root) 'research\g0_logs'
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Find-WatchPort {
    $dev = Get-CimInstance Win32_PnPEntity |
           Where-Object { $_.DeviceID -like '*VID_349B*' -and $_.DeviceID -like '*MI_00*' -and $_.Name -match '\((COM\d+)\)' } |
           Select-Object -First 1
    if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

$deadline = (Get-Date).AddSeconds($Seconds)
Write-Host "watch: ${Seconds}s  baud=$Baud  tag=$Tag  out=$OutDir"

while ((Get-Date) -lt $deadline) {
    $port = Find-WatchPort
    if (-not $port) {
        Start-Sleep -Milliseconds 200
        continue
    }

    Write-Host "`n>>> port $port appeared, opening..." -ForegroundColor Cyan
    $t0 = Get-Date
    $file = Join-Path $OutDir ("{0:yyyyMMdd-HHmmss}_{1}.log" -f (Get-Date), $Tag)
    $sw = New-Object System.IO.StreamWriter($file, $false, [System.Text.Encoding]::UTF8)
    $sw.AutoFlush = $true
    $sp = $null
    $count = 0

    try {
        $ready = $false
        $rd = (Get-Date).AddMilliseconds(2000)
        while ((Get-Date) -lt $rd) {
            if ([System.IO.Ports.SerialPort]::GetPortNames() -contains $port) { $ready = $true; break }
            Start-Sleep -Milliseconds 30
        }
        if (-not $ready) {
            Write-Host ">>> port $port not ready, skip" -ForegroundColor Yellow
            Start-Sleep -Milliseconds 100
            continue
        }

        $sp = New-Object System.IO.Ports.SerialPort $port, $Baud, 'None', 8, 'One'
        $sp.ReadTimeout = 30
        $sp.WriteTimeout = 200
        $sp.DtrEnable = $true
        $sp.RtsEnable = $true

        $opened = $false
        for ($i = 0; $i -lt 10 -and -not $opened; $i++) {
            try { $sp.Open(); $opened = $true } catch { Start-Sleep -Milliseconds 60 }
        }
        if (-not $opened) {
            Write-Host ">>> open $port failed" -ForegroundColor Yellow
            Start-Sleep -Milliseconds 100
            continue
        }
        $sp.DiscardInBuffer()

        $openDeadline = (Get-Date).AddSeconds(60)
        $gone = 0
        while ((Get-Date) -lt $deadline -and (Get-Date) -lt $openDeadline) {
            try {
                $chunk = $sp.ReadExisting()
                if ($chunk -and $chunk.Length -gt 0) {
                    $sw.Write($chunk)
                    Write-Host -NoNewline $chunk
                    $count += $chunk.Length
                } else {
                    Start-Sleep -Milliseconds 10
                }
            } catch {
                Start-Sleep -Milliseconds 20
            }

            if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $port)) {
                $gone++
                if ($gone -ge 3) {
                    Write-Host "`n>>> port $port disappeared" -ForegroundColor Yellow
                    break
                }
            } else {
                $gone = 0
            }
        }
    } catch {
        Write-Host "`n>>> read failed: $($_.Exception.Message)" -ForegroundColor Yellow
    } finally {
        if ($sp -and $sp.IsOpen) { $sp.Close() }
        $sp = $null
        $sw.Close()
    }

    if ($count -gt 0) {
        Write-Host "`n>>> saved $file ($count bytes)" -ForegroundColor Green
    } else {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 300
}

Write-Host "`nwatch done."
