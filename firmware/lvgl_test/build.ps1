<#
.SYNOPSIS
    Build the PROTORIG ES M2 firmware (BL616 / bl616dk).

.DESCRIPTION
    Same PATH recipe as V1.6 firmware/build.ps1:
        project tools\make + SDK tools\cmake\bin + T-Head toolchain + Python
    plus the shell hardening from firmware_ble_probe/build.ps1 (Git Bash /
    MSYS exports SHELL=/usr/bin/sh, which makes the SDK make execute Windows
    backslash command lines through sh and eat the paths; we force
    SHELL=cmd.exe and strip MSYS/MinGW entries from PATH).

    The project directory is the folder this script lives in, so the build
    never touches the repository working tree.

.PARAMETER Clean
    Run "mingw32-make clean" first.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Clean
    From Git Bash:
    powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

.NOTES
    Environment overrides:
      BL_SDK_BASE / BL_TOOLCHAIN_DIR / BL_PYTHON_DIR come from the environment,
      or from firmware/dev-env.json (gitignored; see firmware/README.md).
#>
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'
$Proj = $PSScriptRoot

# Tee-Object writes UTF-16 in PowerShell 5.1; convert the build log to UTF-8
# (no BOM) and strip the ANSI colour codes make emits.
function Convert-BuildLogUtf8 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $text = Get-Content -LiteralPath $Path -Raw
    if ($null -eq $text) { return }
    $text = [regex]::Replace($text, '\x1B\[[0-9;]*[A-Za-z]', '')
    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# ---------------------------------------------------------------------------
# SDK patch: USB console init delay for BLE/RF coexistence (R4-usbdelay)
# Applied before build, reversed afterwards (also on build failure).
# Equivalent text transform: sdk_patches/bouffalo_sdk_usb_console_delay.patch
# ---------------------------------------------------------------------------
function Get-SdkBoardPath {
    return (Join-Path $Sdk 'bsp\board\bl616dk\board.c')
}

function Apply-SdkUsbConsoleDelayPatch {
    param([string]$Path, [string]$Backup)

    $patchFile = Join-Path $Proj 'sdk_patches/bouffalo_sdk_usb_console_delay.patch'
    if (-not (Test-Path -LiteralPath $patchFile)) {
        throw "SDK patch file missing: $patchFile"
    }
    if (-not (Test-Path -LiteralPath $Path)) { throw "SDK board.c not found: $Path" }
    $text = [System.IO.File]::ReadAllText($Path)
    if ($text.Contains('protorig_usb_console_delay')) {
        throw "SDK board.c already contains protorig_usb_console_delay; refusing to patch twice"
    }
    [System.IO.File]::Copy($Path, $Backup, $true)

    $anchor = 'extern void log_start(void);'
    $insert = @(
        '/* R4-usbdelay: app may strongly define protorig_usb_console_delay=1 to delay',
        ' * USB console init until after BLE/RF init. Weak default 0 keeps other apps',
        ' * (probe etc.) unchanged. */',
        '__attribute__((weak)) int protorig_usb_console_delay = 0;',
        ''
    ) -join "`r`n"
    if (-not $text.Contains($anchor)) { throw "SDK board.c anchor not found: $anchor" }
    $text = $text.Replace($anchor, $insert + "`r`n" + $anchor)

    $old = "#ifdef CONFIG_BSP_CONSOLE_USB_CDC`r`n    usb_console_init();`r`n#endif"
    $new = "#ifdef CONFIG_BSP_CONSOLE_USB_CDC`r`n    if (!protorig_usb_console_delay) {`r`n        usb_console_init();`r`n    }`r`n#endif"
    if (-not $text.Contains($old)) { throw "SDK board.c usb_console_init block not found" }
    $text = $text.Replace($old, $new)

    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[sdk] applied usb-console-delay patch (temporary)" -ForegroundColor Yellow
}

function Restore-SdkBoardPatch {
    param([string]$Path, [string]$Backup)
    if (Test-Path -LiteralPath $Backup) {
        [System.IO.File]::Copy($Backup, $Path, $true)
        Remove-Item -LiteralPath $Backup -Force -ErrorAction SilentlyContinue
        Write-Host "[sdk] board.c restored (patch reversed)" -ForegroundColor Yellow
    } else {
        Write-Host "[sdk] WARNING: backup missing; board.c may still be patched" -ForegroundColor Red
    }
}

# ---------------------------------------------------------------------------
# SDK patch: raw CDC RX hook for PC-mode pixel passthrough (protorig-usbpc)
#   usb_console.c 的 shell_exe_cmd() 之前插一个弱钩子 protorig_usb_pc_rx()，
#   固件强定义它来收 A5 5A len 二进制帧；弱默认返回 0 = 其他 app 行为不变。
#   写路径与 USB 描述符都不动 → 不重新枚举、COM 号不变。
# Applied before build, reversed afterwards (also on build failure).
# Equivalent text transform: sdk_patches/bouffalo_sdk_usb_pc_rx.patch
# ---------------------------------------------------------------------------
function Get-SdkUsbConsolePath {
    return (Join-Path $Sdk 'bsp\common\usb_app\usb_console.c')
}

function Apply-SdkUsbPcRxPatch {
    param([string]$Path, [string]$Backup)

    $patchFile = Join-Path $Proj 'sdk_patches/bouffalo_sdk_usb_pc_rx.patch'
    if (-not (Test-Path -LiteralPath $patchFile)) {
        throw "SDK patch file missing: $patchFile"
    }
    if (-not (Test-Path -LiteralPath $Path)) { throw "SDK usb_console.c not found: $Path" }
    $text = [System.IO.File]::ReadAllText($Path)
    if ($text.Contains('protorig_usb_pc_rx')) {
        throw "SDK usb_console.c already contains protorig_usb_pc_rx; refusing to patch twice"
    }
    [System.IO.File]::Copy($Path, $Backup, $true)

    $anchor = 'static const uint8_t usb_console_device_descriptor[] = {'
    $insert = @(
        '/* protorig-usbpc: raw CDC RX hook. The PROTORIG ES app strongly defines',
        ' * protorig_usb_pc_rx() to take binary pixel frames in PC mode',
        ' * (A5 5A len payload, see bsp/usb_pc_link.c); the weak default keeps',
        ' * stock shell behaviour for every other app.',
        ' * Return 1 = consumed, 0 = hand the bytes to the shell. */',
        '__attribute__((weak)) int protorig_usb_pc_rx(const uint8_t *buf, uint16_t len)',
        '{',
        '    (void)buf;',
        '    (void)len;',
        '    return 0;',
        '}',
        '',
        '/* protorig-usbpc: number of completed USB IN transfers. The app watches',
        ' * this counter to detect a dead PC app while the cable is still plugged',
        ' * in (closing the PC process stops the host from reading our IN data). */',
        'volatile uint32_t protorig_usb_console_in_events = 0;',
        ''
    ) -join "`r`n"
    if (-not $text.Contains($anchor)) { throw "SDK usb_console.c anchor not found: $anchor" }
    $text = $text.Replace($anchor, $insert + "`r`n" + $anchor)

    $old = "            if ((rx_len != 0U) && (rx_len <= sizeof(usb_console_rx_buffer))) {`r`n                shell_exe_cmd(usb_console_rx_buffer, (uint16_t)rx_len);`r`n            }"
    $new = "            if ((rx_len != 0U) && (rx_len <= sizeof(usb_console_rx_buffer))) {`r`n                /* protorig-usbpc: 1 = binary frame consumed, 0 = shell */`r`n                if (!protorig_usb_pc_rx(usb_console_rx_buffer, (uint16_t)rx_len)) {`r`n                    shell_exe_cmd(usb_console_rx_buffer, (uint16_t)rx_len);`r`n                }`r`n            }"
    if (-not $text.Contains($old)) { throw "SDK usb_console.c shell_exe_cmd block not found" }
    $text = $text.Replace($old, $new)

    # 主机存活计数：usb_console_bulk_in_cb 里每次 IN 完成 +1。
    # PC 端进程被杀后没人再读 IN 端点，这个计数停止增长 —— 固件据此判定主机消失。
    $oldIn = '    usb_console.tx_done_pending = true;'
    $newIn = "    protorig_usb_console_in_events++;`r`n    usb_console.tx_done_pending = true;"
    if (-not $text.Contains($oldIn)) { throw 'SDK usb_console.c IN-callback anchor not found' }
    $text = $text.Replace($oldIn, $newIn)

    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[sdk] applied usb-pc-rx patch (temporary)" -ForegroundColor Yellow
}

function Restore-SdkUsbConsolePatch {
    param([string]$Path, [string]$Backup)
    if (Test-Path -LiteralPath $Backup) {
        [System.IO.File]::Copy($Backup, $Path, $true)
        Remove-Item -LiteralPath $Backup -Force -ErrorAction SilentlyContinue
        Write-Host "[sdk] usb_console.c restored (patch reversed)" -ForegroundColor Yellow
    } else {
        Write-Host "[sdk] WARNING: backup missing; usb_console.c may still be patched" -ForegroundColor Red
    }
}


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

$Sdk   = $env:BL_SDK_BASE
$Tc    = $env:BL_TOOLCHAIN_DIR
$Py    = $env:BL_PYTHON_DIR
$Make  = Join-Path $Proj 'tools\make'
$Cmake = Join-Path $Sdk 'tools\cmake\bin'

# ---------- environment checks ----------
$missing = @()
foreach ($p in @($Sdk, $Tc, $Py, $Make, $Cmake, (Join-Path $Proj 'Makefile'))) {
    if (-not (Test-Path $p)) { $missing += $p }
}
if ($missing.Count -gt 0) {
    Write-Host "Missing paths:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" }
    Write-Host ""
    Write-Host "Override with environment variables:" -ForegroundColor Yellow
    Write-Host '  $env:BL_SDK_BASE      = "D:\path\to\bouffalo_sdk"'
    Write-Host '  $env:BL_TOOLCHAIN_DIR = "D:\path\to\toolchain_gcc_t-head_windows"'
    Write-Host '  $env:BL_PYTHON_DIR    = "D:\path\to\python"'
    exit 1
}

# ---------- keep MSYS / MinGW out of the command line ----------
$env:SHELL = 'cmd.exe'
$env:MAKEFLAGS = ''
$env:MFLAGS = ''

$pathParts = @()
foreach ($p in ($env:PATH -split ';')) {
    if ([string]::IsNullOrWhiteSpace($p)) { continue }
    $lp = $p.ToLowerInvariant()
    if ($lp.Contains('msys') -or
        $lp.Contains('/usr/bin') -or
        $lp.Contains('\usr\bin') -or
        $lp.Contains('/mingw') -or
        $lp.Contains('\mingw')) {
        continue
    }
    $pathParts += $p
}
# cmd.exe must be reachable (make runs command lines with SHELL=cmd.exe)
foreach ($must in @('C:\Windows\System32', 'C:\Windows')) {
    if (-not ($pathParts | Where-Object { $_.TrimEnd('\') -ieq $must.TrimEnd('\') })) {
        $pathParts = @($must) + $pathParts
    }
}
$env:PATH = (@($Make, $Cmake, (Join-Path $Tc 'bin'), $Py) + $pathParts) -join ';'

Write-Host "SDK      : $Sdk"
Write-Host "Toolchain: $Tc"
Write-Host "Project  : $Proj"
Write-Host ""

$SdkBoard = Get-SdkBoardPath
$SdkBoardBackup = Join-Path $env:TEMP "protorig_board.c.usbdelay.bak"
$SdkUsbConsole = Get-SdkUsbConsolePath
$SdkUsbConsoleBackup = Join-Path $env:TEMP "protorig_usb_console.c.usbpc.bak"
$SdkPatched = $false
$SdkUsbPatched = $false
try {
    Apply-SdkUsbConsoleDelayPatch -Path $SdkBoard -Backup $SdkBoardBackup
    $SdkPatched = $true
    Apply-SdkUsbPcRxPatch -Path $SdkUsbConsole -Backup $SdkUsbConsoleBackup
    $SdkUsbPatched = $true

Push-Location $Proj
    try {    if ($Clean) {
        Write-Host "[clean]" -ForegroundColor Yellow
        & mingw32-make clean CHIP=bl616 BOARD=bl616dk PYTHON=python "SHELL=cmd.exe" 2>&1 |
            Tee-Object -FilePath (Join-Path $Proj 'build_clean.log') | Out-Null
        Write-Host "clean exit=$LASTEXITCODE" -ForegroundColor Gray
        Convert-BuildLogUtf8 (Join-Path $Proj 'build_clean.log')
    }

    Write-Host "[build] mingw32-make CHIP=bl616 BOARD=bl616dk ..." -ForegroundColor Cyan
    & mingw32-make CHIP=bl616 BOARD=bl616dk PYTHON=python "SHELL=cmd.exe" 2>&1 |
        Tee-Object -FilePath (Join-Path $Proj 'build.log')
    $rc = $LASTEXITCODE
    Convert-BuildLogUtf8 (Join-Path $Proj 'build.log')

    if ($rc -ne 0) {
        Write-Host ""
        Write-Host "BUILD FAILED (exit=$rc), full log: $Proj\build.log" -ForegroundColor Red
        if ($SdkUsbPatched) { Restore-SdkUsbConsolePatch -Path $SdkUsbConsole -Backup $SdkUsbConsoleBackup; $SdkUsbPatched = $false }
        if ($SdkPatched) { Restore-SdkBoardPatch -Path $SdkBoard -Backup $SdkBoardBackup; $SdkPatched = $false }
        exit $rc
    }

    $bin = Get-ChildItem 'build\build_out\*.bin' -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch 'boot2|partition|efusedata|dwarfcfi' } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $elf = Get-ChildItem 'build\build_out\*.elf' -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1

    Write-Host ""
    if ($bin) {
        Write-Host "BUILD OK" -ForegroundColor Green
        Write-Host "  bin : $($bin.FullName)"
        Write-Host "  size: $($bin.Length) bytes ($([math]::Round($bin.Length / 1KB, 1)) KB)  time: $($bin.LastWriteTime)"
        # text = Flash code+rodata, data = initialised RAM (also in Flash image),
        # bss = zeroed RAM, dec = whole image footprint.
        $sizeTool = Join-Path $Tc 'bin\riscv64-unknown-elf-size.exe'
        if ($elf -and (Test-Path $sizeTool)) {
            $sz = & $sizeTool $elf.FullName 2>&1 | Out-String
            Write-Host "  elf : $($elf.FullName)"
            Write-Host "  size (text/data/bss/dec/hex):"
            Write-Host $sz.TrimEnd()
        }
        Write-Host ""
        Write-Host "Next (needs the board; not executed by this script):" -ForegroundColor Cyan
        Write-Host "  flash + power-cycle steps: firmware/README.md (or AGENTS.md at repo root)"
    } else {
        Write-Host "BUILD OK but no .bin found under build\build_out\" -ForegroundColor Yellow
    }
}
    finally { Pop-Location }
}
finally {
    if ($SdkUsbPatched) {
        Restore-SdkUsbConsolePatch -Path $SdkUsbConsole -Backup $SdkUsbConsoleBackup
    }
    if ($SdkPatched) {
        Restore-SdkBoardPatch -Path $SdkBoard -Backup $SdkBoardBackup
    }
}
