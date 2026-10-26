# waitsvc.ps1 [seconds]: waits until the VM2Backup service is RUNNING and the site answers on 127.0.0.1:8443 (the service starts with a delay after Windows boots: DELAYED auto start)
param([int]$Seconds = 420)
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    $s = (& sc.exe query VM2BackupService 2>$null | Select-String '\b(STOPPED|START_PENDING|STOP_PENDING|RUNNING|CONTINUE_PENDING|PAUSE_PENDING|PAUSED)\b' | Select-Object -First 1) ; $running = $s -and $s.ToString() -match 'RUNNING'
    if ($running) { try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 8443); $c.Close(); Write-Output ("service up and the site answers after {0:N0}s" -f ((Get-Date) - $t0).TotalSeconds); exit 0 } catch {} }
    Start-Sleep -Seconds 2
}
Write-Output ("TIMEOUT: service " + $s + ", site not answering"); exit 1
