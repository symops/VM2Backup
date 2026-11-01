# cpuload.ps1 <процессов> <секунд>: раунд 12, нагрузка процессора гостя на время цикла ftp_fault_injection_upload (режим «загруженный CPU»).
# Запускает N независимых процессов powershell, каждый крутит пустой цикл заданное время и сам завершается; ничего не остаётся после.
param([int]$N = 4, [int]$Seconds = 600)
$code = '$e=[DateTime]::UtcNow.AddSeconds(' + $Seconds + '); while([DateTime]::UtcNow -lt $e){ $x=1; for($i=0;$i -lt 20000;$i++){ $x=$x*3 % 7 } }'
for ($i = 0; $i -lt $N; $i++) { Start-Process -WindowStyle Hidden powershell -ArgumentList '-NoProfile', '-Command', $code | Out-Null }
Write-Output "INFO cpuload: $N processes for $Seconds s"
exit 0
