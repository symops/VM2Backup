# What a real user sees: the single-file Setup with the Mark of the Web (Zone.Identifier ZoneId=3, as after a browser download) started like a double click.
$d = "$env:USERPROFILE\Downloads"; New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item "$env:SRC\pkg\VM2Backup-Setup.exe" "$d\VM2Backup-Setup.exe" -Force
Set-Content -Path "$d\VM2Backup-Setup.exe" -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3`r`nReferrerUrl=https://example.test/`r`nHostUrl=https://example.test/VM2Backup-Setup.exe"
"MOTW: " + ((Get-Content "$d\VM2Backup-Setup.exe" -Stream Zone.Identifier) -join ' ')
"signature: " + (Get-AuthenticodeSignature "$d\VM2Backup-Setup.exe").Status
Start-Process explorer.exe -ArgumentList "`"$d\VM2Backup-Setup.exe`""
Start-Sleep -Seconds 20
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\shot.ps1 smartscreen-1
'processes: ' + ((Get-Process | Where-Object { $_.ProcessName -match 'Setup|smartscreen|VM2' } | Select-Object -ExpandProperty ProcessName) -join ',')
# расширенная часть: «Подробнее» -> видна кнопка «Выполнить в любом случае»; снимок и перечень элементов окна
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\signing.ps1 click more 8
Start-Sleep -Seconds 3
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\shot.ps1 smartscreen-2-more-info
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\signing.ps1 dump
