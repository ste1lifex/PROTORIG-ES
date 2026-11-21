<#
.SYNOPSIS
    从 agent / CI 的非原生 shell（MSYS bash、Git Bash）里编译固件。

.DESCRIPTION
    build.ps1 本身没问题。问题出在 MSYS 环境：它会把 SHELL=/usr/bin/sh
    和 /usr/bin 注入子进程，SDK 的 make 于是用 sh 执行带 Windows 反斜杠的
    命令（`<SDK>\...` 被当成转义，变成 `<SDK>...`），编译直接失败。

    本脚本在调用 build.ps1 之前：
      · 把 SHELL 指回 cmd.exe
      · 从 PATH 里剔除 MSYS / MinGW 目录
      · 清掉 MAKEFLAGS / MFLAGS，避免继承父 shell 的 make 参数

    原生 PowerShell 里直接跑 .\build.ps1 即可，不需要本脚本。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File build-agent.ps1 lvgl_test
    powershell -NoProfile -ExecutionPolicy Bypass -File build-agent.ps1 lvgl_test -Clean
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Project,
    [switch]$Clean
)

$ErrorActionPreference = 'Continue'

$env:SHELL = 'cmd.exe'
$env:MAKEFLAGS = ''
$env:MFLAGS = ''

$pathParts = @()
foreach ($p in ($env:PATH -split ';')) {
    if ([string]::IsNullOrWhiteSpace($p)) { continue }
    $lower = $p.ToLowerInvariant()
    if ($lower.Contains('msys') -or
        $lower.Contains('/usr/bin') -or
        $lower.Contains('\usr\bin') -or
        $lower.Contains('/mingw')) {
        continue
    }
    $pathParts += $p
}
$env:PATH = ($pathParts -join ';')

$build = Join-Path $PSScriptRoot 'build.ps1'
if (-not (Test-Path $build)) {
    Write-Host "找不到 build.ps1：$build" -ForegroundColor Red
    exit 1
}

& $build $Project -Clean:$Clean
exit $LASTEXITCODE
