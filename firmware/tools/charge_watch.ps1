param(
    [string]$Port = 'COM8',
    [int]$Minutes = 45
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = 115200
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.ReadTimeout = 500
$sp.WriteTimeout = 1000
$sp.Open()

$start = Get-Date
$last = $null
while (((Get-Date) - $start).TotalMinutes -lt $Minutes) {
    try {
        $sp.DiscardInBuffer()
        $sp.Write("batt" + [char]13 + [char]10)
        Start-Sleep -Milliseconds 1200
        $s = $sp.ReadExisting()
        $mv = ''
        if ($s -match 'battery\[0\] = (\d+) mV') { $mv = $Matches[1] }
        $full = ($s -match 'full=YES')
        $chg = ($s -match 'charging=YES')
        Write-Output ("{0:HH:mm:ss} mv={1} charging={2} full={3}" -f (Get-Date), $mv, $chg, $full)
        if ($full) { Write-Output 'FULL_REACHED'; break }
    } catch {
        Write-Output ("{0:HH:mm:ss} ERR {1}" -f (Get-Date), $_.Exception.Message)
    }
    Start-Sleep -Seconds 60
}
$sp.Close()
Write-Output ("DONE {0:HH:mm:ss}" -f (Get-Date))
