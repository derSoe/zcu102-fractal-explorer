# Logs the serial console of the ZCU102 (default: COM9, 115200 8N1) to a file.
# Usage: powershell -ExecutionPolicy Bypass -File tools/uart_log.ps1 -Port COM9 -Out uart.log -Seconds 3600
param([string]$Port = "COM9", [string]$Out = "uart.log", [int]$Seconds = 3600)
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.Open()
"=== UART $Port log from $(Get-Date -Format HH:mm:ss) ===" | Out-File -FilePath $Out -Encoding utf8
$end = (Get-Date).AddSeconds($Seconds)
try {
    while ((Get-Date) -lt $end) {
        $s = $sp.ReadExisting()
        if ($s.Length -gt 0) { [System.IO.File]::AppendAllText($Out, $s) }
        Start-Sleep -Milliseconds 100
    }
} finally { $sp.Close() }
