<#
.SYNOPSIS
    Flash the compiled PROTORIG ES M2 firmware (BL616) over the ROM download port.

.DESCRIPTION
    Adapted from V1.6/firmware/flash.ps1 for the single-project firmware_m2
    layout (no -Project argument; the build output is always firmware_m2\build).

    Uses build\flash_prog_cfg.ini (boot2 + partition + app) and the SDK copy of
    BLFlashCommand.exe to program the chip in ROM download mode.

    IMPORTANT: after flashing you MUST power-cycle the board manually with
    SW2 ON -> OFF -> ON.  Unplugging USB does NOT power-cycle a battery board.
    The ROM download mode state lives in the AON domain and neither --reset nor
    --warm_reset clears it.  Only a real power cycle boots the new image.

    Typical G0 sequence:
      1. Make the running app jump back to ROM download mode:
             .\flash.ps1 -AppPort COM8
         (or hold SW3/BOOT, tap SW1/RST, release SW3)
      2. The script flashes automatically.
      3. Power-cycle the board by hand.

.PARAMETER Port
    ROM download port.  Usually COM7, but Windows renumbers it per machine and per
    enumeration, so when this is omitted the script autodetects it (see -AppPort).

.PARAMETER AppPort
    Optional.  If the application is running (usually COM8), send
    "reboot uart" to this port first, so the chip enters ROM download mode
    without touching the board.  The script then waits for the ROM port to appear
    and flashes that port automatically.

.PARAMETER Bin
    Optional.  Flash a specific image instead of the newest one in build\build_out.
    Use this to flash an archived G0 image without rebuilding:
        .\flash.ps1 -Bin artifacts\protorig_m2_R2c_bl616.bin -AppPort COM8
    flash_prog_cfg.ini always programs build\build_out\protorig_m2_bl616.bin, so
    the image is sha256-copied there and the previous file is kept under
    build\build_out\_prev\.

.PARAMETER Baud
    Flash baud rate, default 2000000.

.PARAMETER Force
    Allow flashing a build_out .bin that is older than the newest source file.

.EXAMPLE
    .\flash.ps1
    .\flash.ps1 -Port COM7 -AppPort COM8
#>
param(
    [string]$Port = '',
    [string]$AppPort = '',
    [string]$Bin = '',
    [int]$Baud = 2000000,
    [switch]$Force,
    [switch]$DryRun
)

$ErrorActionPreference = 'Continue'
$Proj = $PSScriptRoot

# 本机开发路径不入库：优先环境变量；其次读 firmware/dev-env.json（见 firmware/README.md）。
$DevEnvFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'dev-env.json'
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
$Py   = $env:BL_PYTHON_DIR
$Make = Join-Path $Proj 'tools\make'
# Prefer the SDK flash tool; fall back to a local copy in firmware_m2\tools.
$Tool = Join-Path $Sdk 'tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe'
if (-not (Test-Path $Tool)) {
    $Tool = Join-Path $Proj 'tools\BLFlashCommand.exe'
}

foreach ($p in @($Sdk, $Py, $Make, $Proj, $Tool)) {
    if (-not (Test-Path $p)) { Write-Host "Missing: $p" -ForegroundColor Red; exit 1 }
}

$Cfg = Join-Path $Proj 'build\flash_prog_cfg.ini'
if (-not (Test-Path $Cfg)) {
    Write-Host "Missing $Cfg" -ForegroundColor Red
    Write-Host "Build first: .\build.ps1" -ForegroundColor Yellow
    exit 1
}

# ---------- pick the image ----------
# flash_prog_cfg.ini always programs build\build_out\protorig_m2_bl616.bin, so a
# -Bin artifact is staged there (sha256 verified) before flashing.
$binPath = Join-Path $Proj 'build\build_out\protorig_m2_bl616.bin'

if ($Bin -ne '') {
    if (-not [System.IO.Path]::IsPathRooted($Bin)) { $Bin = Join-Path $Proj $Bin }
    if (-not (Test-Path -LiteralPath $Bin)) {
        Write-Host "Missing -Bin: $Bin" -ForegroundColor Red
        exit 1
    }
    $srcItem = Get-Item -LiteralPath $Bin
    $srcHash = (Get-FileHash -LiteralPath $srcItem.FullName -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $binPath) {
        $prevDir = Join-Path $Proj 'build\build_out\_prev'
        New-Item -ItemType Directory -Path $prevDir -Force | Out-Null
        $keep = Join-Path $prevDir ('protorig_m2_bl616_{0}.bin' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Copy-Item -LiteralPath $binPath -Destination $keep -Force
        Write-Host "keep  : build\build_out\_prev\$(Split-Path -Leaf $keep)" -ForegroundColor Gray
    }
    Copy-Item -LiteralPath $srcItem.FullName -Destination $binPath -Force
    $dstHash = (Get-FileHash -LiteralPath $binPath -Algorithm SHA256).Hash
    if ($dstHash -ne $srcHash) {
        Write-Host "!! staging copy verification failed ($dstHash != $srcHash)" -ForegroundColor Red
        exit 1
    }
    $img = Get-Item -LiteralPath $binPath
    Write-Host "image : $($srcItem.Name)  $($srcItem.Length) B  sha256 $($srcHash.Substring(0, 16))..." -ForegroundColor Cyan
} else {
    # ---------- stale binary check ----------
    # A failed build leaves the previous .bin in build_out; without this check we
    # would silently flash old firmware (this really happened once on V1.6).
    $img = Get-ChildItem (Join-Path $Proj 'build\build_out\*.bin') -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'boot2|partition|efusedata|dwarfcfi' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $img) {
        Write-Host "No .bin in build_out; build first (or pass -Bin <artifact>)." -ForegroundColor Red
        exit 1
    }
    # tools/ 是主机工具（face_preview / device_ui_test 等），不进固件镜像，别拿它们判"旧 bin"。
    $newestSrc = Get-ChildItem $Proj -Recurse -File -ErrorAction SilentlyContinue |
                 Where-Object { ($_.Extension -in '.c', '.h') -and
                                ($_.FullName -notmatch '\\build\\') -and
                                ($_.FullName -notmatch '\\tools\\') } |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newestSrc -and $newestSrc.LastWriteTime -gt $img.LastWriteTime) {
        Write-Host "!! .bin is older than the newest source; the build probably failed." -ForegroundColor Red
        Write-Host "     bin  : $($img.Name)  $($img.LastWriteTime)" -ForegroundColor Gray
        Write-Host "     src  : $($newestSrc.FullName)  $($newestSrc.LastWriteTime)" -ForegroundColor Gray
        Write-Host "   Run .\build.ps1 first, or pass -Force, or flash a known-good" -ForegroundColor Yellow
        Write-Host "   artifact with -Bin artifacts\<image>.bin." -ForegroundColor Yellow
        if (-not $Force) { exit 1 }
    }
    Write-Host "image : build output $($img.Name)" -ForegroundColor Cyan
}
Write-Host "flash : $($img.Name)  $([math]::Round($img.Length / 1KB, 1)) KB  $($img.LastWriteTime)" -ForegroundColor Cyan

# ---------- resolve the ROM download port ----------
function Get-ComPorts {
    @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
}

# The ROM and app consoles are the same USB VID/PID (349B:6160) with different
# instance IDs, and Windows names both "USB 串行设备 (COMx)", so the only reliable
# test is "which Bouffalo port is not the app console".
function Get-BouffaloPorts {
    @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
      Where-Object { $_.DeviceID -like '*VID_349B*' -and $_.Name -match '\((COM\d+)\)' } |
      ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } |
      Sort-Object -Unique)
}

if ($Port -eq '' -and $AppPort -ne '') {
    # Already in download mode (reset by hand, or a previous run left it there):
    # the app console is gone, so any other Bouffalo port is the ROM port.
    $others = @((Get-BouffaloPorts) | Where-Object { $_ -ne $AppPort })
    if ($others.Count -eq 1) {
        $Port = $others[0]
        $romAlreadyUp = $true
        Write-Host "romport: $Port (already in download mode)" -ForegroundColor Cyan
    }
}

if ($AppPort -ne '' -and -not $romAlreadyUp) {
    $before = Get-ComPorts
    Write-Host "[1/3] 'reboot uart' -> $AppPort" -ForegroundColor Cyan
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
    } catch {
        Write-Host "      could not send: $($_.Exception.Message)" -ForegroundColor Yellow
        Write-Host "      Do it by hand: hold SW3 (BOOT), tap SW1 (RST), release SW3." -ForegroundColor Yellow
    }
    if ($Port -eq '') {
        Write-Host "      waiting for the download-mode port to appear..." -ForegroundColor Gray
        $waited = 0
        while ($waited -lt 25000 -and $Port -eq '') {
            Start-Sleep -Milliseconds 400
            $waited += 400
            $new = @((Get-ComPorts) | Where-Object { $before -notcontains $_ })
            if ($new.Count -gt 0) {
                $Port = $new[0]
                Write-Host "      romport: $Port (appeared after $waited ms)" -ForegroundColor Cyan
            }
        }
        if ($Port -eq '') {
            Write-Host "No new port appeared in 25 s." -ForegroundColor Red
            Write-Host "  Ports now : $((Get-ComPorts) -join ', ')" -ForegroundColor Gray
            Write-Host "  Manual way: hold SW3 (BOOT), tap SW1 (RST), release SW3, rerun with -Port COMx" -ForegroundColor Yellow
            exit 1
        }
        Start-Sleep -Seconds 2
    } else {
        Start-Sleep -Seconds 6
    }
}

if ($Port -eq '') {
    Write-Host "No ROM download port. Pass -AppPort COM8 (app running) or -Port COMx." -ForegroundColor Red
    Write-Host "  Ports now : $((Get-ComPorts) -join ', ')" -ForegroundColor Gray
    exit 1
}

# ---------- flash ----------
if ($DryRun) {
    Write-Host "[3/3] DRY RUN - nothing written to flash." -ForegroundColor Yellow
    Write-Host "      would run: mingw32-make flash CHIP=bl616 BOARD=bl616dk COMX=$Port BAUDRATE=$Baud"
    Write-Host "      image     : $($img.FullName)" -ForegroundColor Gray
    Write-Host "      port      : $Port" -ForegroundColor Gray
    exit 0
}

Write-Host "[2/3] Flashing to $Port ..." -ForegroundColor Cyan
$env:PATH = "$Make;$Sdk\tools\cmake\bin;" + $env:PATH

Push-Location $Proj
try {
    & mingw32-make flash CHIP=bl616 BOARD=bl616dk COMX=$Port BAUDRATE=$Baud PYTHON=python `
        "BL_FLASH_PROGRAM=$Tool" "SHELL=cmd.exe"
    $rc = $LASTEXITCODE
}
finally { Pop-Location }

Write-Host ""
if ($rc -eq 0) {
    Write-Host "FLASH OK" -ForegroundColor Green
    Write-Host ""
    Write-Host "================================================================" -ForegroundColor Yellow
    Write-Host " POWER-CYCLE THE BOARD NOW!" -ForegroundColor Yellow
    Write-Host "   Toggle SW2 ON -> OFF -> ON (USB unplug does NOT power-cycle a battery board)." -ForegroundColor Yellow
    Write-Host " A soft reset cannot clear the ROM download mode; only power-off can." -ForegroundColor Yellow
    Write-Host "================================================================" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "After boot the app shows up as a virtual COM port (Bouffalo USB Console)." -ForegroundColor Gray
    Write-Host "List it with:" -ForegroundColor Gray
    Write-Host '  Get-CimInstance Win32_PnPEntity | ? { $_.DeviceID -like "*VID_349B*" } | select Name,DeviceID' -ForegroundColor Gray
} else {
    Write-Host "FLASH FAILED (exit=$rc)" -ForegroundColor Red
    Write-Host "Common causes:" -ForegroundColor Yellow
    Write-Host "  - board is not in download mode: use -AppPort COM8, or hold SW3 + tap SW1"
    Write-Host "  - wrong port: -Port COMx (download mode shows as 'Bouffalo CDC DEMO')"
}
