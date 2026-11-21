# 主机侧"探针自证"：把 lvgl_test/st7802.c 用 -DST7802_HOST_VERIFY 编译一遍，
# 在 PC 上 assert CRC32 / pxpat 编码 / gramrd dummy 偏移 / 整分取证状态机。
#
# 为什么不塞进 host_preview.py：preview.c 里已经有 st7802_write_cmd 的桩，
# 而 st7802.c 的其余部分依赖 BL616 DBI 头文件，主机构建里根本编不了。
# 单独一个可执行文件最干净，也不动现有全绿的主机自检。
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot            # lvgl_test/tools -> lvgl_test
$root = Split-Path -Parent $root                    # lvgl_test -> firmware
$proj = Join-Path $root 'lvgl_test'
$out  = Join-Path $proj 'tools\host_output'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$cc = if ($env:HOST_CC) { $env:HOST_CC } else { 'C:/msys64/mingw64/bin/gcc.exe' }
if (-not (Test-Path $cc)) { Write-Host "找不到 gcc: $cc（用 HOST_CC 覆盖）" -ForegroundColor Red; exit 1 }

$exe = Join-Path $out 'st7802_verify.exe'
& $cc -O1 -std=c99 -DST7802_HOST_VERIFY `
      -I $proj `
      (Join-Path $proj 'tools\host\st7802_verify.c') `
      -o $exe
if ($LASTEXITCODE -ne 0) { Write-Host "编译失败" -ForegroundColor Red; exit $LASTEXITCODE }

& $exe
if ($LASTEXITCODE -ne 0) { Write-Host "自检失败" -ForegroundColor Red; exit $LASTEXITCODE }
Write-Host "st7802 探针自检全绿" -ForegroundColor Green
