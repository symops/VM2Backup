# esxilocal.ps1 start | stop : эмулятор ESXi / vCenter (tools/testservers/esxi_mock.py) работает ВНУТРИ гостя, данные датастора на его локальном NTFS.
# Зачем: клиент SMB в Windows держит в кеше содержимое и листинг файлов, которые сервер менял на месте (round 1: 5 ложных падений esxi),
# а имена с ':' на SMB приходят искажёнными. Сам продукт ходит к эмулятору по HTTPS, как и раньше; тесты читают датастор как обычную папку.
# Python: embeddable-zip (python.org, 32 бит) из pkg\pyembed; сертификаты заранее сделаны на хосте (у гостя нет модуля cryptography).
param([string]$Mode = 'start')
$ErrorActionPreference = 'Stop'
$root = 'C:\vm2b\esxi'; $py = 'C:\vm2b\py'; $pids = "$root\pids.txt"
function Stop-All { if (Test-Path $pids) { foreach ($p in (Get-Content $pids)) { if ($p -match '^\d+$') { & taskkill.exe /f /t /pid $p 2>&1 | Out-Null } }; Remove-Item $pids -Force -ErrorAction SilentlyContinue } }
if ($Mode -eq 'stop') { Stop-All; Start-Sleep 2; Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue; Write-Output 'esxi mocks stopped'; exit 0 }
Stop-All
if (-not (Test-Path "$py\python.exe")) { New-Item -ItemType Directory -Force $py | Out-Null; Expand-Archive -Path "$env:SRC\pkg\pyembed\python-embed-win32.zip" -DestinationPath $py -Force }
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $root | Out-Null
# клиент SMB может ещё не видеть только что созданную папку pkg\esxi (кеш каталога): ждём до 60 с
for ($i = 0; $i -lt 30 -and -not (Test-Path "$env:SRC\pkg\esxi\esxi_mock.py"); $i++) { Start-Sleep -Seconds 2 }
Copy-Item "$env:SRC\pkg\esxi\*" $root -Recurse -Force
$env:VM2B_PREMADE_CERTS = "$root\certs"; $env:VM2B_BIND_ADDR = '127.0.0.1'; $env:VM2B_SERVER_HOST = '127.0.0.1'
$seed = if ($env:ESXI_SEED) { $env:ESXI_SEED } else { '4242' }
& "$py\python.exe" -I "$root\esxi_launch.py" "$root\run" "$root\servers.json" $pids $seed 2>&1 | Write-Output
if (-not (Test-Path "$root\servers.json")) { Write-Output 'FAIL  esxi mocks did not start'; Get-ChildItem "$root\run\*\mock.log" -ErrorAction SilentlyContinue | ForEach-Object { Write-Output $_.FullName; Get-Content $_.FullName | Select-Object -Last 8 }; exit 1 }
Write-Output ("esxi mocks started: " + (@(Get-Content $pids)).Count + " processes; servers.json in $root")
