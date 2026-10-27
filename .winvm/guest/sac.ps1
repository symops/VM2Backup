# Smart App Control (Windows 11): попытка перевести режим в On (принудительное применение) и проверить, что происходит с неподписанным установщиком и службой.
#   sac.ps1 info     только состояние (VerifiedAndReputablePolicyState: 0 выключен, 1 включён, 2 оценка) и журнал CodeIntegrity
#   sac.ps1 enable   пробует записать 1: сначала от администратора, потом от SYSTEM (задача планировщика); печатает, что получилось. Нужна перезагрузка.
#   sac.ps1 check    после перезагрузки: состояние, запуск Setup без / с меткой Mark of the Web, служба продукта, события CodeIntegrity 3076 / 3077, снимки экрана
# ПОСЛЕ включения SAC без сброса Windows выключить нельзя: запускать ПОСЛЕДНЕЙ фазой перед удалением ВМ. Ничего, кроме этого значения, не меняется.
param([string]$Mode = 'info')
. C:\vm2b\guest\lib.ps1
$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
function Get-Sac { try { return [int](Get-ItemProperty $key -ErrorAction Stop).VerifiedAndReputablePolicyState } catch { return -1 } }
function Show-Sac($tag) { $s = Get-Sac; $t = switch ($s) { 0 { 'Off' } 1 { 'On (enforced)' } 2 { 'Evaluation' } default { 'not present' } }; Write-Host ("INFO  Smart App Control ${tag}: VerifiedAndReputablePolicyState=$s ($t)"); return $s }
function Show-CI {
    foreach ($log in 'Microsoft-Windows-CodeIntegrity/Operational') {
        try { Get-WinEvent -LogName $log -MaxEvents 15 -ErrorAction Stop | ForEach-Object { Write-Host ("INFO  CI event {0} {1:HH:mm:ss}: {2}" -f $_.Id, $_.TimeCreated, (($_.Message -split "`r?`n")[0])) } } catch { Write-Host "INFO  CI log: $($_.Exception.Message)" }
    }
}
if ($Mode -eq 'info') { Show-Sac 'now' | Out-Null; Show-CI; exit 0 }
if ($Mode -eq 'enable') {
    $before = Show-Sac 'before'
    if ($before -eq 1) { Write-Host 'INFO  already enforced'; exit 0 }
    # 1) администратор напрямую
    try { Set-ItemProperty -Path $key -Name VerifiedAndReputablePolicyState -Value 1 -Type DWord -ErrorAction Stop; Write-Host 'INFO  direct write as administrator: accepted' } catch { Write-Host ('INFO  direct write as administrator: refused: ' + $_.Exception.Message) }
    Show-Sac 'after the direct write' | Out-Null
    # 2) от SYSTEM (разовая задача планировщика)
    if ((Get-Sac) -ne 1) {
        $cmd = 'reg add HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy /v VerifiedAndReputablePolicyState /t REG_DWORD /d 1 /f > C:\vm2b\sac-system.txt 2>&1'
        & schtasks.exe /create /tn vm2bsac /tr "cmd.exe /c $cmd" /sc once /st 00:00 /ru SYSTEM /rl HIGHEST /f 2>&1 | Out-Null
        & schtasks.exe /run /tn vm2bsac 2>&1 | Out-Null; Start-Sleep -Seconds 8
        Write-Host ('INFO  write as SYSTEM: ' + ((Get-Content C:\vm2b\sac-system.txt -ErrorAction SilentlyContinue) -join ' '))
        & schtasks.exe /delete /tn vm2bsac /f 2>&1 | Out-Null
        Show-Sac 'after the write as SYSTEM' | Out-Null
    }
    if ((Get-Sac) -eq 1) { Write-Host 'INFO  the value is 1: a restart will show whether Windows honours it' } else { Write-Host 'INFO  the value stays as it was: Smart App Control cannot be turned on from the registry on this install' }
    exit 0
}
if ($Mode -eq 'check') {
    $s = Show-Sac 'after the restart'
    Show-CI
    $svc = (& sc.exe query VM2BackupService 2>&1 | Select-String 'STATE') -join ' '
    Write-Host ("INFO  product service under this state: $svc")
    $d = "$env:USERPROFILE\Downloads"; New-Item -ItemType Directory -Force $d | Out-Null
    $motw = "$d\VM2Backup-Setup-motw.exe"; $plain = "$d\VM2Backup-Setup-plain.exe"
    Copy-Item "$env:SRC\pkg\VM2Backup-Setup.exe" $motw -Force; Copy-Item "$env:SRC\pkg\VM2Backup-Setup.exe" $plain -Force
    Set-Content -Path $motw -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3`r`nReferrerUrl=https://example.test/"
    foreach ($f in @($plain, $motw)) {
        $name = [IO.Path]::GetFileNameWithoutExtension($f)
        $err = ''; $started = $false
        try { $p = Start-Process -FilePath $f -PassThru -ErrorAction Stop; Start-Sleep -Seconds 12; $started = -not $p.HasExited; if ($p.HasExited) { $err = "exited with code $($p.ExitCode)" } } catch { $err = $_.Exception.Message }
        & powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\shot.ps1 "sac-$name" | Out-Null
        Write-Host ("INFO  SAC state ${s}, ${name}: started=$started $err")
        Get-Process -Name 'VM2Backup-Setup*' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        Get-Process -Name 'smartscreen' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Show-CI
    if ($s -eq 1) { Write-Host 'PASS  Smart App Control is ENFORCED on this VM: the behaviour of the unsigned Setup is recorded above (INFO lines, screenshots sac-*.png)' }
    else { Write-Host "INFO  Smart App Control could not be enforced (state $s): enforcement NOT tested on this install" }
    exit 0
}
