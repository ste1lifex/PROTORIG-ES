<#
.SYNOPSIS
    将指定 Git 提交导出到新的源码目录。

.DESCRIPTION
    从仓库根目录运行。git archive 根据 .gitattributes 排除仅供本机使用的文件。
    输出目录必须尚未存在。打包说明见 PUBLISHING.md。

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools/export-public.ps1 -Out ..\PROTORIG-ES-source
#>
param(
    [string]$Out = '..\PROTORIG-ES-source',
    [string]$Ref = 'HEAD'
)
$ErrorActionPreference = 'Stop'

$root = (git rev-parse --show-toplevel).Trim()
if (-not $root) { throw '当前目录不是 git 仓库' }
Push-Location $root
try {
    if (Test-Path -LiteralPath $Out) {
        throw "输出路径已存在，请指定新目录：$Out"
    }
    $zip = Join-Path $env:TEMP ("protorig-public-" + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        git archive --format=zip --output="$zip" --end-of-options $Ref
        if ($LASTEXITCODE -ne 0) { throw "无法归档 Git 引用：$Ref" }
        New-Item -ItemType Directory -Path $Out -ErrorAction Stop | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath $Out
    } finally {
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    }

    $leak = Join-Path $Out 'local'
    if (Test-Path -LiteralPath $leak) {
        $items = @(Get-ChildItem -LiteralPath $leak -Recurse -Force -ErrorAction SilentlyContinue)
        if ($items.Count -gt 0) {
            throw "local/ 泄漏进了发布树（$($items.Count) 项）：检查 .gitattributes 的 export-ignore"
        }
        Remove-Item -LiteralPath $leak -Force
    }
    $eda = @(Get-ChildItem -LiteralPath $Out -Recurse -Filter *.epro2 -File -ErrorAction SilentlyContinue)
    if ($eda.Count -gt 0) {
        throw "EDA 工程文件泄漏进了发布树（$($eda.Count) 个 .epro2）：检查 export-ignore"
    }
    Write-Host "public tree -> $Out" -ForegroundColor Green
    Get-ChildItem -LiteralPath $Out | Select-Object Name, Mode | Format-Table -AutoSize
} finally {
    Pop-Location
}
