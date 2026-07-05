Start-Sleep -Seconds 2
$port = New-Object System.IO.Ports.SerialPort('COM3', 115200, 'None', 8, 1)
$port.Open()
$port.ReadTimeout = 2000
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 15000) {
    try {
        $line = $port.ReadLine()
        if ($line -match 'panaac|Climate|current_temperature|State updated|Control call') { Write-Host $line }
    }
    catch [TimeoutException] {}
}
$port.Close()
