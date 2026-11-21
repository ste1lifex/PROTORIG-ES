<#
.SYNOPSIS
    电脑模式 USB 原始二进制上行（A5 5A len）最小验收脚本。

.DESCRIPTION
    验证 sdk_patches/bouffalo_sdk_usb_pc_rx.patch + bsp/usb_pc_link.c 这条链：
      1) shell 发 `pcmode 1` 进电脑模式，等 `@PCM1 PC 1`；
      2) 发 HELLO（A5 5A 01 01），应回 `@PCM1 FRAME a55a50 81 01 E0 01 10 00 02`
         并出现 `[pc] passthrough ON`（== USB 侧的二进制 RX 真的接管了）；
      3) 发 CLEAR 纯红（整屏变红，肉眼看得到）；
      4) 发一块 16x16 瓦片（BEGIN/DATA×3/END），验证分片与组帧；
      5) shell 发 `pcmode`，断言 passthrough=1 / usbraw=1 / bad=0 / ring_drop=0；
      6) `pcmode 0` 退出，断言 `@PCM1 PC 0` + active=0 + usbraw=0（读取权还给 shell）。

    脚本只读写串口，不烧录、不复位。板子必须先上电开机（ROM 下载模式下没有 COM 口）。

.PARAMETER Port
    应用口，默认自动找 Bouffalo USB Console（VID 349B）。

.PARAMETER SkipTile
    跳过 16x16 瓦片测试（只想验证 HELLO/CLEAR 时用）。

.PARAMETER Quiet
    只打印失败项与最终结果。

.PARAMETER Band
    额外发一整条 480x16 的像素带（15360 B，65 个 DATA 片），测真实吞吐与 ring_drop。
    这是插件"整屏刷新"的单带基准（整屏 = 30 带）。

.PARAMETER TouchMs
    额外进入触摸监听模式 N 毫秒：透传期间手摸屏幕，脚本解析 0x04 触摸帧并打印坐标。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\usb_pc_test.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\usb_pc_test.ps1 -Band -TouchMs 15000
#>
[CmdletBinding()]
param(
    [string]$Port = '',
    [switch]$SkipTile,
    [switch]$Quiet,
    [switch]$Band,
    [int]$Bands = 1,
    [int]$TouchMs = 0,
    [int]$TouchMin = 3
)

$ErrorActionPreference = 'Stop'

function Find-BouffaloPort {
    $cands = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -like '*VID_349B*' -and $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { $Matches = $null; if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } |
        Sort-Object -Unique)
    $open = @([System.IO.Ports.SerialPort]::GetPortNames())
    foreach ($c in $cands) { if ($open -contains $c) { return $c } }
    return ''
}

if ($Port -eq '') { $Port = Find-BouffaloPort }
if ($Port -eq '') {
    Write-Host "找不到可用的 Bouffalo USB Console 串口。" -ForegroundColor Red
    Write-Host "  板子可能还在 ROM 下载模式：先手动拨动开关重启（SW2 ON→OFF→ON）。" -ForegroundColor Yellow
    Write-Host "  当前串口：$(([System.IO.Ports.SerialPort]::GetPortNames()) -join ', ')" -ForegroundColor Gray
    exit 1
}

$port0 = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$port0.Handshake = 'None'
$port0.DtrEnable = $true
$port0.RtsEnable = $true
$port0.ReadTimeout = 200
$port0.WriteTimeout = 500

try { $port0.Open() }
catch {
    Write-Host "打开 $Port 失败：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host "（COM 口被占用？先关掉串口助手 / 烧录工具 / 旧 desktop 客户端）" -ForegroundColor Yellow
    exit 1
}

$script:Rx = New-Object System.Text.StringBuilder
$script:Fails = New-Object System.Collections.Generic.List[string]
$script:Passes = 0
$script:T0 = Get-Date

function Get-Elapsed { [int]((Get-Date) - $script:T0).TotalMilliseconds }

function Read-For {
    param([int]$Ms)
    $deadline = (Get-Date).AddMilliseconds($Ms)
    while ((Get-Date) -lt $deadline) {
        try {
            $chunk = $port0.ReadExisting()
        } catch { $chunk = '' }
        if ($chunk) {
            [void]$script:Rx.Append($chunk)
            foreach ($ln in ($chunk -split "`r?`n")) {
                $t = $ln -replace '\x1B\[[0-9;]*[A-Za-z]', ''
                if ($t.Trim()) {
                    if (-not $Quiet) { Write-Host ("  [{0,5} ms] {1}" -f (Get-Elapsed), $t.TrimEnd()) -ForegroundColor DarkGray }
                }
            }
        } else {
            Start-Sleep -Milliseconds 15
        }
    }
}

function Wait-Text {
    param([string]$Pattern, [int]$TimeoutMs = 2000)
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if ($script:Rx.ToString() -match $Pattern) { return $true }
        Read-For -Ms 60
    }
    return ($script:Rx.ToString() -match $Pattern)
}

function Send-Text {
    param([string]$Text)
    if (-not $Quiet) { Write-Host ">> $Text" -ForegroundColor Cyan }
    $b = [System.Text.Encoding]::ASCII.GetBytes($Text + "`r`n")
    $port0.Write($b, 0, $b.Length)
    Start-Sleep -Milliseconds 80
}

function Send-Frame {
    param([byte[]]$Payload, [string]$Label, [switch]$NoDelay, [switch]$NoPrint)
    $f = New-Object System.Collections.Generic.List[byte]
    $f.Add(0xA5); $f.Add(0x5A); $f.Add([byte]$Payload.Length)
    $f.AddRange($Payload)
    if (-not $Quiet -and -not $NoPrint) {
        $hex = ($f.ToArray() | ForEach-Object { $_.ToString('X2') }) -join ' '
        if ($hex.Length -gt 90) { $hex = $hex.Substring(0, 90) + " ... ($($f.Count) B)" }
        Write-Host ">> frame $Label : $hex" -ForegroundColor Cyan
    }
    $arr = $f.ToArray()
    $port0.Write($arr, 0, $arr.Length)
    if (-not $NoDelay) { Start-Sleep -Milliseconds 60 }
}

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    if ($Ok) {
        $script:Passes++
        Write-Host ("  PASS  {0}" -f $Name) -ForegroundColor Green
    } else {
        $script:Fails.Add($Name + $(if ($Detail) { " ($Detail)" } else { '' }))
        Write-Host ("  FAIL  {0} {1}" -f $Name, $Detail) -ForegroundColor Red
    }
}

function Clear-Rx { [void]$script:Rx.Clear() }

Write-Host "port: $Port   baud: 115200" -ForegroundColor Gray
Write-Host "=== 1) 进电脑模式 ===" -ForegroundColor Yellow
Clear-Rx
Read-For -Ms 200
Send-Text "pcmode 1"
$ok = Wait-Text '@PCM1 PC 1' 2500
Check 'shell pcmode 1 -> @PCM1 PC 1' $ok
if ($ok) { Start-Sleep -Milliseconds 200 }

Write-Host "=== 2) HELLO（二进制帧） ===" -ForegroundColor Yellow
Clear-Rx
Send-Frame -Payload ([byte[]]@(0x01)) -Label 'HELLO'
$okHello = Wait-Text '@PCM1 FRAME a55a5081' 2000
$okOn = Wait-Text 'passthrough ON' 1000
Check 'HELLO 收到 0x81 回帧（原始 RX 已接管）' $okHello
Check 'passthrough ON（LVGL 冻结）' $okOn
if ($okHello) {
    $m = [regex]::Match($script:Rx.ToString(), '@PCM1 FRAME a55a5081([0-9A-Fa-f]{12})')
    if ($m.Success) {
        $h = $m.Groups[1].Value
        # [ver][maxW lo][maxW hi][maxH lo][maxH hi][fmt]
        $ver = [Convert]::ToInt32($h.Substring(0, 2), 16)
        $maxW = [Convert]::ToInt32($h.Substring(2, 2), 16) + 256 * [Convert]::ToInt32($h.Substring(4, 2), 16)
        $maxH = [Convert]::ToInt32($h.Substring(6, 2), 16) + 256 * [Convert]::ToInt32($h.Substring(8, 2), 16)
        $fmt = [Convert]::ToInt32($h.Substring(10, 2), 16)
        Write-Host ("  报告的参数: ver={0} maxW={1} maxH={2} fmt={3}" -f $ver, $maxW, $maxH, $fmt) -ForegroundColor Gray
        Check 'maxW/maxH/fmt 合理（480 / 16 / 2）' ($maxW -eq 480 -and $maxH -eq 16 -and $fmt -eq 2)
    }
}

Write-Host "=== 3) CLEAR 整屏红 ===" -ForegroundColor Yellow
Clear-Rx
Send-Frame -Payload ([byte[]]@(0x13, 0xFF, 0x00, 0x00)) -Label 'CLEAR red'
Start-Sleep -Milliseconds 400
Write-Host "  （手表屏幕此时应整屏变红）" -ForegroundColor Gray

if (-not $SkipTile) {
    Write-Host "=== 4) 16x16 瓦片（中央） ===" -ForegroundColor Yellow
    Clear-Rx
    # TILE_BEGIN x=232 y=232 w=16 h=16
    Send-Frame -Payload ([byte[]]@(0x10, 0xE8, 0x00, 0xE8, 0x00, 0x10, 0x00, 0x10, 0x00)) -Label 'BEGIN'
    # 512 B 像素 = 239 + 239 + 34
    $white = New-Object byte[] 512
    for ($i = 0; $i -lt 512; $i++) { $white[$i] = 0xFF }
    Send-Frame -Payload ([byte[]](@(0x11) + $white[0..238]))       -Label 'DATA 239'
    Send-Frame -Payload ([byte[]](@(0x11) + $white[239..477]))     -Label 'DATA 239'
    Send-Frame -Payload ([byte[]](@(0x11) + $white[478..511]))     -Label 'DATA 34'
    Send-Frame -Payload ([byte[]]@(0x12)) -Label 'END'
    Start-Sleep -Milliseconds 400
    Write-Host "  （红屏中央应出现 16x16 白块）" -ForegroundColor Gray
}

if ($Band) {
    $bands = [Math]::Max(1, [Math]::Min(30, $Bands))
    Write-Host ("=== 4b) 整带吞吐：{0} 条 480x16（{1} B / {2} 片） ===" -f $bands, ($bands * 15360), ($bands * 65)) -ForegroundColor Yellow
    Clear-Rx

    # 红->蓝 水平渐变，RGB565 小端；每条带按 y 偏移一点，整屏看起来是斜向渐变
    $bandSrc = New-Object byte[] 15360
    for ($x = 0; $x -lt 480; $x++) {
        $r5 = [int](31 * (1 - $x / 479.0)); $b5 = [int](31 * ($x / 479.0))
        $px = ($r5 -shl 11) -bor $b5
        for ($y = 0; $y -lt 16; $y++) {
            $o = ($y * 480 + $x) * 2
            $bandSrc[$o] = [byte]($px -band 0xFF); $bandSrc[$o + 1] = [byte](($px -shr 8) -band 0xFF)
        }
    }

    # 一条带 = BEGIN + 65 DATA + END。先拼成一整块再一次性写，避免逐帧 WriteFile
    # 的主机开销污染测量（插件也应该这么发）。
    function Send-Band {
        param([int]$Y, [byte[]]$Pix, [switch]$NoPrint)
        $ms = New-Object System.IO.MemoryStream
        $begin = [byte[]]@(0x10, 0x00, 0x00, ($Y -band 0xFF), (($Y -shr 8) -band 0xFF), 0xE0, 0x01, 0x10, 0x00)
        $hdr = [byte[]]@(0xA5, 0x5A, [byte]$begin.Length)
        $ms.Write($hdr, 0, 3); $ms.Write($begin, 0, $begin.Length)
        $off = 0
        while ($off -lt $Pix.Length) {
            $n = [Math]::Min(239, $Pix.Length - $off)
            $dh = [byte[]]@(0xA5, 0x5A, [byte]($n + 1), 0x11)
            $ms.Write($dh, 0, 4); $ms.Write($Pix, $off, $n)
            $off += $n
        }
        $end = [byte[]]@(0xA5, 0x5A, 0x01, 0x12)
        $ms.Write($end, 0, 4)
        $arr = $ms.ToArray(); $ms.Dispose()
        if (-not $Quiet -and -not $NoPrint) { Write-Host ("  >> band y={0} : {1} B 一次写入" -f $Y, $arr.Length) -ForegroundColor Cyan }
        $port0.Write($arr, 0, $arr.Length)
        return $arr.Length
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $total = 0
    for ($bi = 0; $bi -lt $bands; $bi++) {
        $total += (Send-Band -Y ($bi * 16) -Pix $bandSrc -NoPrint)
    }
    $sw.Stop()
    $ms = $sw.ElapsedMilliseconds
    $sec = [Math]::Max($ms, 1) / 1000.0
    Write-Host ("  发送 {0} 条带 / {1} B，耗时 {2} ms  => {3:N1} KB/s" -f $bands, $total, $ms, (($total / 1024.0) / $sec)) -ForegroundColor Gray
    Write-Host ("  整屏帧率 ≈ {0:N2} fps（{1:N0} ms/帧）；单条带 {2:N1} ms" -f ($bands / 30.0 / $sec), ($sec * 1000.0 / ($bands / 30.0)), ($ms / $bands)) -ForegroundColor Gray
    Start-Sleep -Milliseconds 800
    Clear-Rx
    Send-Text "pcmode"
    [void](Wait-Text '\[pc\] active=' 2000)
    $bl = ($script:Rx.ToString() -split "`r?`n" | Where-Object { $_ -match '\[pc\] active=' } | Select-Object -Last 1)
    Write-Host ("  {0}" -f $bl.Trim()) -ForegroundColor Gray
    Check '整屏/整带全部合法（bad 未增加）' ($bl -match 'bad=0') "$bl"
    Check "记录环无溢出（ring_drop=0）" ($bl -match 'ring_drop=0') "$bl"
    if ($bands -ge 30) {
        Write-Host "  （整屏应变成红->蓝渐变）" -ForegroundColor Gray
    } else {
        Write-Host "  （屏幕顶部 $($bands*16) 行应出现红->蓝渐变）" -ForegroundColor Gray
    }
}

if ($TouchMs -gt 0) {
    Write-Host "=== 4c) 触摸转发：请在 $TouchMs ms 内摸屏幕 ===" -ForegroundColor Yellow
    Write-Host "  LVGL 已冻结，触摸不会误切页；脚本解析 0x04 帧。" -ForegroundColor Gray
    Clear-Rx
    $tsw = [System.Diagnostics.Stopwatch]::StartNew()
    $touchFrames = New-Object System.Collections.Generic.List[string]
    $seen = 0
    while ($tsw.ElapsedMilliseconds -lt $TouchMs -and $touchFrames.Count -lt $TouchMin) {
        Read-For -Ms 100
        $text = $script:Rx.ToString()
        # 文本帧 = "@PCM1 FRAME a55a50" + hex([cmd][payload...])，
        # 所以 cmd=0x50 之后还有 6 字节：[04][pressed][xlo][xhi][ylo][yhi]
        $ms = [regex]::Matches($text, '@PCM1 FRAME a55a5050([0-9A-Fa-f]{12})')
        for ($i = $seen; $i -lt $ms.Count; $i++) {
            $h = $ms[$i].Groups[1].Value
            $op = [Convert]::ToInt32($h.Substring(0, 2), 16)
            $pressed = [Convert]::ToInt32($h.Substring(2, 2), 16)
            $x = [Convert]::ToInt32($h.Substring(4, 2), 16) + 256 * [Convert]::ToInt32($h.Substring(6, 2), 16)
            $y = [Convert]::ToInt32($h.Substring(8, 2), 16) + 256 * [Convert]::ToInt32($h.Substring(10, 2), 16)
            $d = "op=0x{3:X2} pressed={0} x={1} y={2}" -f $pressed, $x, $y, $op
            $touchFrames.Add($d)
            $seen++
            if ($touchFrames.Count -le 40) { Write-Host ("  touch #{0}: {1}" -f $seen, $d) -ForegroundColor Gray }
        }
    }
    if ($touchFrames.Count -ge $TouchMin) {
        Write-Host ("  已收到 {0} 帧（>= {1}），提前结束等待" -f $touchFrames.Count, $TouchMin) -ForegroundColor Gray
    }
    Check ("收到触摸帧（{0} 帧）" -f $touchFrames.Count) ($touchFrames.Count -gt 0)
    if ($touchFrames.Count -gt 0) {
        $last = $touchFrames[$touchFrames.Count - 1]
        Write-Host ("  末尾：{0}" -f $last) -ForegroundColor Gray
    }
}

Write-Host "=== 5) 回 shell 查询统计 ===" -ForegroundColor Yellow
Clear-Rx
Send-Text "pcmode"
$okStat = Wait-Text '\[pc\] active=' 2000
Check '透传中 shell 仍然可用' $okStat
if ($okStat) {
    $line = ($script:Rx.ToString() -split "`r?`n" | Where-Object { $_ -match '\[pc\] active=' } | Select-Object -Last 1)
    $g = @{}
    foreach ($k in 'active', 'transport', 'passthrough', 'usbraw', 'rx', 'bad', 'ignored', 'ring_drop', 'tile_open') {
        if ($line -match "$k=(-?\d+)") { $g[$k] = [int]$Matches[1] } else { $g[$k] = -999 }
    }
    Write-Host ("  active={0} transport={1} passthrough={2} usbraw={3} rx={4} bad={5} ignored={6} ring_drop={7}" -f
        $g.active, $g.transport, $g.passthrough, $g.usbraw, $g.rx, $g.bad, $g.ignored, $g.ring_drop) -ForegroundColor Gray
    # 触摸窗口里如果用户左滑，会走本地手势"退出电脑模式"（research/56 §5.1 的设计），
    # 所以 -TouchMs 模式下不把 active=1 当断言，只报告。
    if ($TouchMs -gt 0) {
        if ($g.active -ne 1) {
            Write-Host "  注意：触摸窗口期间已退出电脑模式（本地手势生效，属设计行为）" -ForegroundColor Yellow
        } else {
            Write-Host "  触摸窗口期间没有触发本地手势，仍停在电脑模式" -ForegroundColor Gray
        }
    } else {
        Check 'active=1 transport=1(USB) passthrough=1 usbraw=1' ($g.active -eq 1 -and $g.transport -eq 1 -and $g.passthrough -eq 1 -and $g.usbraw -eq 1)
    }
    Check "帧全部解析（bad=0, rx>=5）" ($g.bad -eq 0 -and $g.rx -ge 5) "bad=$($g.bad) rx=$($g.rx)"
    if (-not $Band) {
        Check "记录环没溢出（ring_drop=0）" ($g.ring_drop -eq 0) "ring_drop=$($g.ring_drop)"
    } else {
        Write-Host ("  ring_drop={0}（-Band 模式不判失败，见 4b 的吞吐说明）" -f $g.ring_drop) -ForegroundColor Gray
    }
}

Write-Host "=== 6) 退出电脑模式 ===" -ForegroundColor Yellow
Clear-Rx
Send-Text "pcmode 0"
$okLeave = Wait-Text '@PCM1 PC 0' 2500
if ($okLeave) {
    Check '收到 @PCM1 PC 0' $true
} else {
    # 触摸窗口里已经被本地手势带走的情况：本来就是手表模式，pcmode 0 不会打印
    Write-Host "  未收到 @PCM1 PC 0（可能已在手表模式，属正常）" -ForegroundColor Yellow
}
Start-Sleep -Milliseconds 300
Clear-Rx
Send-Text "pcmode"
$okStat2 = Wait-Text '\[pc\] active=0' 2000
Check '退出后 active=0 usbraw=0（读取权还给 shell）' ($okStat2 -and ($script:Rx.ToString() -match 'usbraw=0'))
if ($okStat2) {
    $line2 = ($script:Rx.ToString() -split "`r?`n" | Where-Object { $_ -match '\[pc\] active=' } | Select-Object -Last 1)
    Write-Host ("  {0}" -f $line2.Trim()) -ForegroundColor Gray
}

$port0.Close()
$port0.Dispose()

Write-Host ""
Write-Host "================ 结果 ================" -ForegroundColor Yellow
Write-Host ("PASS {0} 项 / FAIL {1} 项" -f $script:Passes, $script:Fails.Count) -ForegroundColor $(if ($script:Fails.Count) { 'Red' } else { 'Green' })
foreach ($f in $script:Fails) { Write-Host "  - $f" -ForegroundColor Red }
if ($script:Fails.Count -eq 0) { Write-Host "USB 二进制上行链路验收通过。" -ForegroundColor Green; exit 0 }
exit 1
