# запуски службы продукта по её журналу и по журналу System (после перезагрузок при включённом Smart App Control)
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 7036 } -MaxEvents 600 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'VM2Backup' } | Select-Object -First 14 | ForEach-Object { "INFO  SCM 7036 {0:MM-dd HH:mm:ss}: {1}" -f $_.TimeCreated, (($_.Message -split "`r?`n")[0]) }
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 6005, 6006 } -MaxEvents 12 -ErrorAction SilentlyContinue | ForEach-Object { "INFO  boot-log {0} {1:MM-dd HH:mm:ss}" -f $_.Id, $_.TimeCreated }
$log = "$env:ProgramData\VM2Backup\logs\service.log"
if (Test-Path $log) { Get-Content $log -ErrorAction SilentlyContinue | Select-String -Pattern 'start|started|stop|listen' | Select-Object -Last 14 | ForEach-Object { "INFO  service.log: " + $_.Line } }
