Start-Sleep -Seconds 3
$port = New-Object System.IO.Ports.SerialPort('COM3', 115200, 'None', 8, 1)
$port.Open()
$port.ReadTimeout = 3000
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 8000) {
    try {
        $line = $port.ReadLine()
        Write-Host $line
    } catch [TimeoutException] {}
}
$port.Close()
