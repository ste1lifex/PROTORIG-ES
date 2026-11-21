param(
    [string]$Port = '',
    [int]$BootWaitMs = 2500
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if ($Port -eq '') {
    $devs = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*VID_349B*' })
    if ($devs.Count -eq 0) { Write-Output 'NO_DEVICE'; exit 1 }
    foreach ($d in $devs) {
        $prop = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_BusReportedDeviceDesc'
        Write-Output ("BUS=" + $prop.Data + "  NAME=" + $d.FriendlyName)
        if ($d.FriendlyName -match '\((COM\d+)\)') { $Port = $Matches[1] }
    }
}
if ($Port -eq '') { Write-Output 'NO_PORT'; exit 1 }
Write-Output ("PORT=" + $Port)

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 200
$sp.WriteTimeout = 500
$sp.Open()
Start-Sleep -Milliseconds 300
$boot = $sp.ReadExisting()
Start-Sleep -Milliseconds $BootWaitMs
$boot += $sp.ReadExisting()
Write-Output '--- BOOT/RESIDUAL ---'
Write-Output $boot.TrimEnd()

foreach ($c in @('batt', 'chrg', 'tpstat', 'rtcstat')) {
    $sp.Write($c + [char]13 + [char]10)
    Start-Sleep -Milliseconds 1500
    Write-Output (">>> " + $c)
    $out = $sp.ReadExisting()
    if ($out) { Write-Output $out.TrimEnd() } else { Write-Output '(no echo)' }
}
$sp.Close()
