# Host unit tests (no SDK / no board).
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\host\run_host_tests.ps1
$ErrorActionPreference = 'Continue'
$Proj = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Out  = Join-Path $PSScriptRoot 'build_host'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$gcc = 'gcc'
$fail = 0

function Build-And-Run {
    param([string]$Name, [string[]]$Sources, [string[]]$Includes, [string[]]$Defines = @())
    $exe = Join-Path $Out ($Name + '.exe')
    Write-Host "[$Name] compile" -ForegroundColor Cyan
    $args = @('-std=c99', '-Wall', '-Wextra', '-Werror')
    foreach ($def in $Defines) { $args += ('-D' + $def) }
    foreach ($inc in $Includes) { $args += '-I'; $args += $inc }
    $args += $Sources
    $args += '-o'
    $args += $exe
    & $gcc @args
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$Name] COMPILE FAILED" -ForegroundColor Red
        $script:fail = 1
        return
    }
    Write-Host "[$Name] run" -ForegroundColor Cyan
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$Name] TEST FAILED" -ForegroundColor Red
        $script:fail = 1
    }
}

Build-And-Run -Name 'test_settings_codec' -Includes @(
    (Join-Path $Proj 'services')
) -Sources @(
    (Join-Path $PSScriptRoot 'test_settings_codec.c'),
    (Join-Path $Proj 'services\settings_codec.c')
)

# R2c-fix: power idle timing (board_power.c compiled with host stubs)
Build-And-Run -Name 'test_power_timing' -Includes @(
    (Join-Path $PSScriptRoot 'stubs'),
    (Join-Path $Proj 'bsp'),
    (Join-Path $Proj 'drivers')
) -Defines @('PROTORIG_HOST_TEST') -Sources @(
    (Join-Path $PSScriptRoot 'test_power_timing.c'),
    (Join-Path $PSScriptRoot 'power_stubs.c'),
    (Join-Path $Proj 'bsp\board_power.c')
)

if ($fail -ne 0) { Write-Host 'HOST TESTS FAILED' -ForegroundColor Red } else { Write-Host 'HOST TESTS PASSED' -ForegroundColor Green }
exit $fail
