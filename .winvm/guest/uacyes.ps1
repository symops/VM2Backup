# uacyes.ps1 launch | check (раунд 13, F7): НАСТОЯЩИЙ запрос UAC. Подписанный пакет запускается без повышения (ярлык с ключами /install /silent, двойной щелчок через проводник: тот же приём, что в signing.ps1),
# лаунчер просит повышение (ShellExecute runas), хост нажимает «Да» на защищённом рабочем столе (qemu sendkey), повышенный установщик стартует ИЗ удерживаемой папки в %TEMP% и обновляет продукт.
# check: служба перезапущена (другой pid), установленные exe подписаны (Valid при доверенном тестовом корне), папок VM2Backup-Setup-* не осталось, журнал %TEMP%\VM2Backup-Setup.log без ошибок.
param([string]$Mode = 'launch')
. C:\vm2b\guest\lib.ps1
$B = "$env:ProgramFiles\VM2Backup"; $d = 'C:\vm2b\uac'; $st = "$d\before.txt"
function SvcPid { $p = Get-Process VM2BackupService -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p) { $p.Id } else { 0 } }
if ($Mode -eq 'launch') {
    New-Item -ItemType Directory -Force $d | Out-Null; Copy-Item "$env:SRC\pki\variants\signed.exe" "$d\VM2Backup-Setup.exe" -Force
    Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue; Remove-Item "$env:TEMP\VM2Backup-Setup.log" -ErrorAction SilentlyContinue
    "$(SvcPid)" | Set-Content $st
    $ws = New-Object -ComObject WScript.Shell; $l = $ws.CreateShortcut("$d\install.lnk"); $l.TargetPath = "$d\VM2Backup-Setup.exe"; $l.Arguments = '/install /silent'; $l.WorkingDirectory = $d; $l.Save()
    Write-Output "INFO service pid before: $(Get-Content $st); file: $((Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion); signature of the package: $((Get-AuthenticodeSignature "$d\VM2Backup-Setup.exe").Status)"
    Start-Process explorer.exe -ArgumentList "`"$d\install.lnk`""
    for ($i = 0; $i -lt 40; $i++) { if (Get-Process consent -ErrorAction SilentlyContinue) { Write-Output "INFO the UAC prompt (consent.exe) appeared after $($i / 2) s"; exit 0 }; Start-Sleep -Milliseconds 500 }
    Write-Output 'FAIL  no UAC prompt (consent.exe) appeared in 20 s'; exit 1
}
if ($Mode -eq 'check') {
    $before = [int](Get-Content $st); $ok = $false
    for ($i = 0; $i -lt 120; $i++) { $now = SvcPid; if ($now -ne 0 -and $now -ne $before -and (Get-Service VM2BackupService).Status -eq 'Running') { $ok = $true; break }; Start-Sleep -Seconds 1 }
    Check "after the UAC consent the elevated installer ran and the service was restarted (pid $before -> $(SvcPid))" $ok ''
    Start-Sleep -Seconds 3
    Check 'no UAC prompt is left on the screen' (-not (Get-Process consent -ErrorAction SilentlyContinue)) ''
    $left = @(Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue).Count; Check "the extraction folder is removed ($left left)" ($left -eq 0) ''
    $lg = "$env:TEMP\VM2Backup-Setup.log"; $lt = if (Test-Path $lg) { (Get-Content $lg -Raw).Trim() } else { '' }; Write-Output "INFO VM2Backup-Setup.log: $(if ($lt) { $lt } else { '(none)' })"
    Check 'no error in %TEMP%\VM2Backup-Setup.log (no sharing violation)' ($lt -eq '') $lt
    foreach ($n in 'VM2Backup.exe', 'VM2BackupService.exe', 'VM2Backup-Setup.exe') { $s = Get-AuthenticodeSignature "$B\$n"; Write-Output "INFO installed $n : $($s.Status)"; Check "installed $n is signed (the signed package was installed)" ($s.Status -eq 'Valid' -or $s.Status -eq 'UnknownError' -or $s.Status -eq 'NotTrusted') "$($s.Status)" }
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
    Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
}
