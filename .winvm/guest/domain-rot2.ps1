# domain-rot2.ps1 (ВМ member, ПОСЛЕ синхронного сдвига часов обеих ВМ на +45 суток; интервал пароля gMSA 1 сутки): настоящая ротация пароля gMSA.
#  - msDS-ManagedPasswordId до и после повторной выдачи пароля (Test-ADServiceAccount делает это от имени компьютера MEMBER$);
#  - служба, запущенная ДО ротации, БЕЗ перезапуска: копия на \\dc.vm2b.test\Backups; потом Test-ADServiceAccount, перезапуск службы, копия.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; $adm = 'S-1-5-32-544'; $svc = $global:SvcName
Import-Module ActiveDirectory
Write-Output ("INFO clock on member: " + (Get-Date).ToString('s'))
$id = (Get-Content C:\vm2b\rot-server-id.txt -ErrorAction SilentlyContinue | Select-Object -First 1)
$pid0 = (Get-WmiObject Win32_Service -Filter "Name='$svc'").ProcessId
Check 'the service started before the shift is still RUNNING' ((St) -match 'RUNNING') (St)
Check 'process owner is still the gMSA' ((Svc-Info).Owner -match 'vm2bgmsa') ((Svc-Info) | Out-String)
$st0 = Get-GmsaState; Write-Output ("INFO gMSA state after the shift, before the new password request: " + (Gmsa-Line $st0))
$tsa = Test-ADServiceAccount $d.Gmsa; Write-Output "INFO Test-ADServiceAccount after the shift: $tsa"
Check 'Test-ADServiceAccount after the shift' $tsa ''
$st1 = Get-GmsaState; Write-Output ("INFO gMSA state after Test-ADServiceAccount: " + (Gmsa-Line $st1))
# Test-ADServiceAccount только проверяет, что машина МОЖЕТ получить пароль, и новый пароль не порождает: настоящая ротация случается при первой ВЫДАЧЕ пароля после истечения интервала,
# то есть когда служба стартует под gMSA (LSA запрашивает пароль). Поэтому сравнение идентификатора делается ПОСЛЕ перезапуска службы (см. ниже)
# служба, запущенная ДО ротации, без перезапуска
$w = Web-Ready; Check 'site up (the old process)' ($null -ne $w) ''
$pidNow = (Get-WmiObject Win32_Service -Filter "Name='$svc'").ProcessId; Write-Output "INFO service pid before the shift $pid0, now $pidNow"
if ($w) { $r = Backup-Once $w 'rot-b-norestart' $id; Write-Output "INFO backup in the service started BEFORE the rotation, no restart: $r"; Check '[method B] the service started before the rotation still copies, no restart' ($r -eq 'Success') $r }
& klist.exe -li 0x3e7 purge 2>&1 | Out-Null
Check '[method B] Test-ADServiceAccount again' (Test-ADServiceAccount $d.Gmsa) ''
& net.exe stop $svc 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null; $so = & net.exe start $svc 2>&1; Write-Output ("INFO net start: " + ($so -join ' ' -replace '\s+', ' '))
Check '[method B] service RUNNING after the rotation + restart (no reboot)' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready
if ($w) { $r = Backup-Once $w 'rot-b-restart' $id; Write-Output "INFO backup after the rotation and the restart: $r"; Check '[method B] copy to the share after the rotation and the restart' ($r -eq 'Success') $r }
$st2 = Get-GmsaState; Write-Output ("INFO gMSA state at the end: " + (Gmsa-Line $st2))
$changed = ($st0.Id -ne '') -and ($st2.Id -ne '') -and ($st0.Id -ne $st2.Id); $pl = $st2.PwdLastSet -ne $st0.PwdLastSet
Write-Output "INFO ROTATION_B_CHANGED=$changed (pwdLastSet changed: $pl)"
Check '[method B: +45 days] msDS-ManagedPasswordId CHANGED after the first password issue (service start): a real rotation of the gMSA password' ($changed -and $pl) ((Gmsa-Line $st0) + ' -> ' + (Gmsa-Line $st2))
# возврат службы на LocalSystem (часы вернёт хост)
if ($w) { foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -like 'rot*' -or $x.name -like 'src rot*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } } }
Switch-Account '' $null; Check 'back on LocalSystem: RUNNING' (Wait-State 'RUNNING' 90000) (St)
Remove-LocalGroupMember -SID $adm -Member 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
