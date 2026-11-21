<#
.SYNOPSIS
    G0 / R-round serial harness for the PROTORIG ES watch (BL616 USB console).

.DESCRIPTION
    Sends shell commands to the running application console and logs every byte
    with a relative timestamp, so the G0 operation sheet (research/19) can be
    filled from a file instead of a scrollback buffer.

    Port autodetection: matches the Bouffalo USB console (VID_349B) so the script
    keeps working when Windows renumbers the COM port after a power cycle.

    Nothing here flashes or resets anything. It only writes the commands you ask
    for plus "\r\n".

.PARAMETER Port
    Explicit port (e.g. COM8). Default: autodetect the Bouffalo console.

.PARAMETER Cmd
    One or more shell command lines, sent in order.

.PARAMETER Seq
    Comma/semicolon separated commands, same as -Cmd but from one string.

.PARAMETER Probe
    Send no command; just open the port, drain for a moment and report the port
    identity plus anything the board says spontaneously.

.PARAMETER Boot
    Listen for -Seconds without sending anything (use with a power cycle).

.PARAMETER Quiet
    Milliseconds of silence that ends the read for a command (default 700).

.PARAMETER Timeout
    Hard cap per command in milliseconds (default 4000).

.PARAMETER Tag
    Included in the log file name.

.PARAMETER NoLog
    Print only; do not write a log file.

.PARAMETER OutDir
    Log directory. Default: <repo>\research\g0_logs

.PARAMETER WaitSeconds
    Wait this long for -Port to appear before opening it.  Use after a flash or a
    power cycle, while the board is still off:
        .\g0_serial.ps1 -Port COM8 -WaitSeconds 120 -Boot -Seconds 15 -Tag boot

.EXAMPLE
    .\g0_serial.ps1 -Probe

.EXAMPLE
    .\g0_serial.ps1 -Cmd batt -Tag r9-charge

.EXAMPLE
    .\g0_serial.ps1 -Seq "settings,batt,appstat,rtcstat" -Tag r11-r12

.EXAMPLE
    .\g0_serial.ps1 -Cmd "panelsleep 1" -Quiet 2000 -Tag i3

.NOTES
    Console baud is 115200 (same rate flash.ps1 uses to send "reboot uart").
#>
[CmdletBinding()]
param(
    [string]$Port = '',
    [int]$Baud = 115200,
    [string[]]$Cmd = @(),
    [string]$Seq = '',
    [switch]$Probe,
    [switch]$Boot,
    [int]$Seconds = 12,
    [int]$Listen = 0,
    [int]$Quiet = 700,
    [int]$Timeout = 4000,
    [int]$WaitSeconds = 0,
    [string]$Tag = 'manual',
    [switch]$NoLog,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ port ---
function Get-BouffaloDevices {
    @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
      Where-Object { $_.DeviceID -like '*VID_349B*' -and $_.Name -match '\(COM\d+\)' })
}

function Find-WatchPort {
    # Both consoles are 349B:6160 with the same display name.  The application
    # console is the composite CDC interface (DeviceID contains MI_00); the ROM
    # download interface has no MI_ and only exists in download mode.
    $devs = @(Get-BouffaloDevices)
    $app = $devs | Where-Object { $_.DeviceID -like '*MI_00*' } | Select-Object -First 1
    if ($app -and $app.Name -match '\((COM\d+)\)') { return $Matches[1] }
    if ($devs.Count -gt 0 -and $devs[0].Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

function Test-IsRomInterface([string]$name) {
    $d = @(Get-BouffaloDevices) | Where-Object { $_.Name -match "\($name\)" } | Select-Object -First 1
    if ($null -eq $d) { return $false }
    return ($d.DeviceID -notlike '*MI_00*')
}

function Find-RomPort {
    # ROM download mode enumerates as a different USB interface; catch the usual
    # Bouffalo names so we can tell "board is in download mode" from "app console".
    Get-CimInstance Win32_PnPEntity |
        Where-Object {
            $_.Name -match '\((COM\d+)\)' -and (
                $_.Name -match 'Bouffalo' -or
                $_.Name -match 'CDC DEMO' -or
                ($_.DeviceID -like '*VID_349B*' -and $_.Name -notmatch 'USB'))
        } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } }
}

if ($Port -eq '') {
    $Port = Find-WatchPort
    if ($null -eq $Port) {
        Write-Host "No Bouffalo USB console (VID_349B) found." -ForegroundColor Red
        Write-Host "  Present ports:" -ForegroundColor Gray
        [System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }
        $rom = @(Find-RomPort)
        if ($rom.Count -gt 0) {
            Write-Host "  Board looks like it is in ROM download mode: $($rom -join ', ')" -ForegroundColor Yellow
        }
        exit 2
    }
    Write-Host "port    : $Port (autodetected Bouffalo console)" -ForegroundColor Cyan
} else {
    Write-Host "port    : $Port" -ForegroundColor Cyan
}

if (Test-IsRomInterface $Port) {
    Write-Host "  WARNING: this looks like the ROM download interface, not the app console." -ForegroundColor Yellow
    Write-Host "  The board is probably still in download mode: power-cycle it manually (SW2 ON -> OFF -> ON; USB unplug does not power-cycle a battery board)." -ForegroundColor Yellow
}

# ------------------------------------------------------------------- log ---
if (-not $NoLog -and $OutDir -eq '') {
    $root = Split-Path -Parent $PSScriptRoot          # firmware_m2
    $OutDir = Join-Path (Split-Path -Parent $root) 'research\g0_logs'
}
$logPath = $null
if (-not $NoLog) {
    if (-not (Test-Path -LiteralPath $OutDir)) {
        New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $logPath = Join-Path $OutDir "$stamp`_$Tag.log"
}

if ($WaitSeconds -gt 0) {
    Write-Host "waiting for $Port to appear (up to ${WaitSeconds}s) ..." -ForegroundColor Yellow
    $w = 0
    $seen = $false
    while ($w -lt ($WaitSeconds * 1000)) {
        if (@([System.IO.Ports.SerialPort]::GetPortNames()) -contains $Port) { $seen = $true; break }
        Start-Sleep -Milliseconds 300
        $w += 300
        if (($w % 3000) -eq 0) { Write-Host ("  ... {0}s" -f ($w / 1000)) -ForegroundColor DarkGray }
    }
    if (-not $seen) {
        Write-Host "$Port did not appear in ${WaitSeconds}s." -ForegroundColor Red
        Write-Host "  Ports now: $(([System.IO.Ports.SerialPort]::GetPortNames()) -join ', ')" -ForegroundColor Gray
        exit 4
    }
    Write-Host "appeared after $([math]::Round($w / 1000, 1))s" -ForegroundColor Green
    Start-Sleep -Milliseconds 200
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$script:logLines = New-Object System.Collections.Generic.List[string]

function Emit([string]$text) {
    $ms = $sw.ElapsedMilliseconds
    $ts = '[{0,7} ms] ' -f $ms
    Write-Host ($ts + $text)
    $script:logLines.Add($ts + $text)
}

function EmitRaw([string]$text) {
    Write-Host $text
    $script:logLines.Add($text)
}

# ----------------------------------------------------------------- serial ---
$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName  = $Port
$sp.BaudRate  = $Baud
$sp.Parity    = [System.IO.Ports.Parity]::None
$sp.DataBits  = 8
$sp.StopBits  = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout  = 200
$sp.WriteTimeout = 1000
$sp.NewLine = "`r`n"
# The firmware prints UTF-8 (Chinese comments end up in shell output); without
# this the default ANSI encoding turns every CJK byte into "?".
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)

try {
    $sp.Open()
} catch {
    Write-Host "Cannot open ${Port}: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "  Is the port held by a terminal (PuTTY / VSCode serial monitor)?" -ForegroundColor Gray
    exit 3
}

EmitRaw "================================================================"
EmitRaw " G0 serial log  $Tag"
EmitRaw " time  : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
EmitRaw " port  : $Port  @ $Baud"
EmitRaw " host  : $env:COMPUTERNAME  user: $env:USERNAME"
EmitRaw "================================================================"

function Read-Quiet([int]$quietMs, [int]$hardMs) {
    $buf = New-Object System.Text.StringBuilder
    $last = [System.Diagnostics.Stopwatch]::StartNew()
    $hard = [System.Diagnostics.Stopwatch]::StartNew()
    while ($hard.ElapsedMilliseconds -lt $hardMs) {
        $chunk = $null
        try { $chunk = $sp.ReadExisting() } catch { $chunk = $null }
        if ($chunk -and $chunk.Length -gt 0) {
            [void]$buf.Append($chunk)
            $last.Restart()
        } else {
            Start-Sleep -Milliseconds 25
        }
        if ($buf.Length -gt 0 -and $last.ElapsedMilliseconds -ge $quietMs) { break }
        if ($buf.Length -eq 0 -and $hard.ElapsedMilliseconds -gt ($hardMs * 0.6)) { break }
    }
    return $buf.ToString()
}

function Write-Text([string]$s) {
    if ([string]::IsNullOrEmpty($s)) { return }
    $clean = $s -replace "`r`n", "`n" -replace "`r", "`n"
    foreach ($line in ($clean -split "`n")) {
        if ($line.Trim().Length -eq 0) { continue }
        Emit $line
    }
}

$exitCode = 0
try {
    # drain anything already buffered (stale output from a previous session)
    Start-Sleep -Milliseconds 150
    $stale = $sp.ReadExisting()
    if ($stale -and $stale.Trim().Length -gt 0) {
        EmitRaw "--- buffered before start ---"
        Write-Text $stale
    }

    if ($Listen -gt 0) {
        # Continuous capture with ARRIVAL timestamps: use this to correlate
        # physical actions (tap, swipe) with firmware log lines in real time.
        # Unlike -Boot this does not stop on silence.
        EmitRaw "--- listening ${Listen}s (arrival timestamps) ---"
        $end = (Get-Date).AddSeconds($Listen)
        while ((Get-Date) -lt $end) {
            $chunk = $null
            try { $chunk = $sp.ReadExisting() } catch { $chunk = $null }
            if ($chunk -and $chunk.Length -gt 0) { Write-Text $chunk }
            Start-Sleep -Milliseconds 30
        }
        EmitRaw "--- listen done ---"
    }
    elseif ($Probe) {
        EmitRaw "--- probe: 1.5 s listen, no command sent ---"
        Write-Text (Read-Quiet -quietMs 800 -hardMs 1500)
        EmitRaw "--- probe done ---"
    }
    elseif ($Boot) {
        EmitRaw "--- listening for boot output (${Seconds}s) ---"
        Write-Text (Read-Quiet -quietMs 1500 -hardMs ($Seconds * 1000))
        EmitRaw "--- boot listen done ---"
    }
    else {
        $list = @()
        foreach ($c in $Cmd) { if ($c -ne '') { $list += $c } }
        if ($Seq -ne '') {
            foreach ($c in ($Seq -split '[,;]')) {
                $t = $c.Trim()
                if ($t -ne '') { $list += $t }
            }
        }
        if ($list.Count -eq 0) {
            EmitRaw "No -Cmd / -Seq / -Probe / -Boot given; nothing to do."
            $exitCode = 1
        }
        foreach ($line in $list) {
            EmitRaw ""
            EmitRaw ">>> $line"
            $sp.Write($line + "`r`n")
            Start-Sleep -Milliseconds 60
            Write-Text (Read-Quiet -quietMs $Quiet -hardMs $Timeout)
        }
    }
}
finally {
    try { $sp.Close() } catch { }
    $sp.Dispose()
}

if ($logPath) {
    $header = @(
        "# G0 serial log - $Tag",
        "# port $Port @ $Baud",
        "# $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
        ""
    )
    Set-Content -LiteralPath $logPath -Value ($header + $script:logLines) -Encoding UTF8
    Write-Host ""
    Write-Host "log saved: $logPath" -ForegroundColor Green
    Write-Host ("  lines : {0}" -f $script:logLines.Count) -ForegroundColor Gray
}

exit $exitCode
