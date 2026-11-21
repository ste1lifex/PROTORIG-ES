<#
.SYNOPSIS
    往板子的应用串口发一条或多条 shell 命令，并把回显抓回来。

.EXAMPLE
    .\serial_cmd.ps1 -Port COM8 -Cmd tpmode, tpstat
    .\serial_cmd.ps1 -Port COM8 -Cmd "tpgesture 1" -WaitMs 2000
#>
param(
    [string]$Port = 'COM8',
    [Parameter(Mandatory = $true)][string[]]$Cmd,
    [int]$Baud = 115200,
    [int]$WaitMs = 1200,
    [int]$BootWaitMs = 400
)

$ErrorActionPreference = 'Stop'
$p = New-Object System.IO.Ports.SerialPort
$p.PortName = $Port; $p.BaudRate = $Baud
$p.Parity = [System.IO.Ports.Parity]::None; $p.DataBits = 8
$p.StopBits = [System.IO.Ports.StopBits]::One
$p.Handshake = [System.IO.Ports.Handshake]::None
$p.DtrEnable = $true; $p.RtsEnable = $true
$p.ReadTimeout = 200
$p.WriteTimeout = 500
$p.Open()
Start-Sleep -Milliseconds $BootWaitMs
$p.ReadExisting() | Out-Null      # 丢掉上电日志残渣

foreach ($c in $Cmd) {
    Write-Output ">>> $c"
    $p.Write("$c`r`n")
    $buf = New-Object System.Text.StringBuilder
    $t = (Get-Date).AddMilliseconds($WaitMs)
    while ((Get-Date) -lt $t) {
        try { $s = $p.ReadExisting(); if ($s) { [void]$buf.Append($s) } } catch {}
        Start-Sleep -Milliseconds 60
    }
    $out = $buf.ToString()
    if ($out) { Write-Output $out.TrimEnd() } else { Write-Output "(无回显)" }
}
$p.Close()
