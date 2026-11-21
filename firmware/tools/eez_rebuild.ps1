<#
.SYNOPSIS
    EEZ 工程 → 固件 一条命令全流程（不用手动开 EEZ Studio）

.DESCRIPTION
    关键发现：打包版 EEZ Studio 支持**无头构建**（见它 main.tsx 的主进程逻辑）：

        EEZ Studio.exe --build-project <工程文件>

    它会跳过单实例锁、不渲染界面、把构建日志打到 stdout、构建完自动退出并给出 exit code。

    本脚本把整条链路串起来：

        ① EEZ 无头构建        （.eez-project → src/ui/*.c）
        ② 重生成字库          （把 EEZ 页面里用到的字符并进 lv_font_*，可选）
        ③ post_build_patch    （修标识符/include/字体引用 + 填样式函数）
        ④ 编译固件
        ⑤ 烧录（可选）

.EXAMPLE
    .\eez_rebuild.ps1                       # 只构建到 .bin
    .\eez_rebuild.ps1 -Flash                # 构建 + 烧录
    .\eez_rebuild.ps1 -SkipFonts -Flash     # 没改文字就跳过字库（快）
    .\eez_rebuild.ps1 -Preview              # 顺手出一份圆屏预览图
#>
param(
    [switch]$Flash,
    [switch]$SkipFonts,
    [switch]$Preview,
    [string]$AppPort = 'COM8',
    [string]$Port = 'COM5',
    [int]$BuildTimeoutSec = 120
)

$ErrorActionPreference = 'Continue'
$FwRoot = Split-Path -Parent $PSScriptRoot          # firmware/
$Proj = Join-Path $FwRoot 'eez_ui\protorig_ui.eez-project'
$EezExe = "$env:LOCALAPPDATA\Programs\eezstudio\EEZ Studio.exe"
$Py = if ($env:BL_PYTHON_DIR) { Join-Path $env:BL_PYTHON_DIR 'python.exe' }
      else { (Get-Command python -ErrorAction SilentlyContinue).Source }

function Step($n) { Write-Host ""; Write-Host ("=" * 62) -ForegroundColor DarkGray; Write-Host "  $n" -ForegroundColor Cyan; Write-Host ("=" * 62) -ForegroundColor DarkGray }
function Die($m) { Write-Host "!! $m" -ForegroundColor Red; exit 1 }

foreach ($p in @($Proj, $EezExe, $Py)) { if (-not (Test-Path $p)) { Die "找不到 $p" } }

# ---------- ① EEZ 无头构建 ----------
Step "① EEZ 无头构建"
$sw = [Diagnostics.Stopwatch]::StartNew()
$out = "$env:TEMP\eez_headless_out.txt"; $err = "$env:TEMP\eez_headless_err.txt"
Remove-Item $out, $err -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath $EezExe -ArgumentList '--build-project', $Proj `
                      -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
if (-not $proc.WaitForExit($BuildTimeoutSec * 1000)) {
    $proc.Kill(); Die "EEZ 构建超时（$BuildTimeoutSec s）"
}
$log = if (Test-Path $out) { Get-Content $out } else { @() }
$log | Where-Object { $_ -notmatch '^\s*Unused style' } | Select-Object -Last 12 | ForEach-Object { Write-Host "   $_" -ForegroundColor Gray }
$sw.Stop()

if ($proc.ExitCode -ne 0) { Die "EEZ 构建失败，exit=$($proc.ExitCode)" }
# ⚠️ 不能简单地 match 'error' —— EEZ 成功时也会打 "No error and no warning detected"
$errLines = $log | Where-Object {
    $_ -match '(?i)(^|\s)error(:|\s)|failed|exception' -and $_ -notmatch '(?i)no error'
}
if ($errLines) {
    Write-Host "   !! 日志里有错误行：" -ForegroundColor Yellow
    $errLines | Select-Object -First 5 | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }
    Die "EEZ 构建输出里有错误"
}
Write-Host ("   EEZ 构建完成，耗时 {0:N1} s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Green

# ---------- ② 字库 ----------
if (-not $SkipFonts) {
    Step "② 重生成字库（把 EEZ 页面用到的字符并进去）"
    Push-Location (Join-Path $FwRoot 'lvgl_test')
    & $Py tools\gen_lvgl_fonts.py
    $rc = $LASTEXITCODE
    & $Py tools\check_font_coverage.py
    Pop-Location
    if ($rc -ne 0) { Die "字库生成失败" }
} else {
    Step "② 跳过字库（-SkipFonts）"
}

# ---------- ③ post_build_patch ----------
Step "③ 补丁：修标识符 / include / 字体引用 + 填样式函数"
Push-Location (Join-Path $FwRoot 'eez_ui')
& $Py post_build_patch.py
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { Die "post_build_patch 失败" }

# ---------- 预览（可选） ----------
if ($Preview) {
    Step "  圆屏预览"
    Push-Location (Join-Path $FwRoot 'lvgl_test')
    & $Py tools\preview_pages.py
    Pop-Location
    Write-Host "   打开: $(Join-Path $FwRoot 'eez_ui\preview\preview_all.png')" -ForegroundColor Gray
}

# ---------- ④ 编译 ----------
Step "④ 编译固件"
& (Join-Path $FwRoot 'build.ps1') lvgl_test 2>&1 |
    Select-String -Pattern 'error:|undefined reference|编译成功|编译失败|大小 :' |
    ForEach-Object { Write-Host "   $_" }
if ($LASTEXITCODE -ne 0) { Die "编译失败" }

# ---------- ⑤ 烧录 ----------
if ($Flash) {
    Step "⑤ 烧录"
    # ⚠️ 别用 $args —— 那是 PowerShell 的自动变量
    $flashArgs = @('lvgl_test', '-Port', $Port)
    if (Get-PnpDevice -Class Ports -Status OK | Where-Object { $_.FriendlyName -match $AppPort }) {
        $flashArgs += @('-AppPort', $AppPort)
    }
    & (Join-Path $FwRoot 'flash.ps1') @flashArgs 2>&1 |
        Select-String -Pattern '将烧录|已发送|烧录成功|烧录失败|Flash writing' |
        ForEach-Object { Write-Host "   $_" }
    Write-Host ""
    Write-Host "   ⚠️ 现在请手动拨动开关重启（SW2 ON→OFF→ON；拔 USB 不会断开电池供电）才会跑新固件" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "   没烧录（加 -Flash 即可）" -ForegroundColor Gray
}
