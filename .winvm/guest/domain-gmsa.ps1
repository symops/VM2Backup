# domain-gmsa.ps1: короткая проверка службы под gMSA с копией на \\dc.vm2b.test\Backups (пустой пользователь) до и после перезапуска, потом возврат на LocalSystem.
# (Дополняет domain-svc.ps1: там запись сервера с пустым пользователем на ту же шару, что и явная, в одном из прогонов не получила идентификатор.)
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; $adm = 'S-1-5-32-544'
$w = Web-Ready
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -like 'diag*' -or $x.name -like 'gm*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$s = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'gm own'; path = '\\dc.vm2b.test\Backups'; user = ''; password = '' } }; $id = $s.Result.id
Check 'server with the empty user saved' ([bool]$id) $s.Text
Make-Data 'C:\vm2b\domdata' 4
Add-LocalGroupMember -SID $adm -Member 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Grant-LogonRight 'VM2B\vm2bgmsa$'
Switch-Account 'VM2B\vm2bgmsa$' ''
Check 'RUNNING under the gMSA' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready; Check 'site up' ($null -ne $w) ''
Check 'process owner is the gMSA' ((Svc-Info).Owner -match 'vm2bgmsa') ((Svc-Info) | Out-String)
$t = Api $w 'servers.test' @{ id = $id }; Check 'servers.test as the gMSA' ($t.Status -eq 200) $t.Text
$h = Run-Backup $w ('gmsa1-' + (Get-Date).ToString('HHmmss')) 'C:\vm2b\domdata' 'vmS' $id '{VM}'; Write-Output ("INFO gMSA backup 1: " + ($h | ConvertTo-Json -Compress)); Check 'backup as the gMSA: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
& net.exe stop $global:SvcName 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null; & net.exe start $global:SvcName 2>&1 | Out-Null
Check 'RUNNING after a restart' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready
$h = Run-Backup $w ('gmsa2-' + (Get-Date).ToString('HHmmss')) 'C:\vm2b\domdata' 'vmS' $id '{VM}'; Check 'backup as the gMSA after the restart: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
# пересчёт пароля: Reset-ADServiceAccountPassword для gMSA и сразу Test-ADServiceAccount, перезапуск службы, копия
Import-Module ActiveDirectory
$rs = try { Reset-ADServiceAccountPassword $d.Gmsa -ErrorAction Stop; 'ok' } catch { 'error: ' + $_.Exception.Message }; Write-Output "INFO Reset-ADServiceAccountPassword: $rs"
Check 'Test-ADServiceAccount after the reset' (Test-ADServiceAccount $d.Gmsa) ''
& net.exe stop $global:SvcName 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null; & net.exe start $global:SvcName 2>&1 | Out-Null
Check 'RUNNING after the reset + restart (no reboot)' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready; $h = Run-Backup $w ('gmsa3-' + (Get-Date).ToString('HHmmss')) 'C:\vm2b\domdata' 'vmS' $id '{VM}'; Check 'backup as the gMSA after the reset + restart: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
Api $w 'servers.delete' @{ id = $id } | Out-Null
Switch-Account '' $null; Check 'back on LocalSystem: RUNNING' (Wait-State 'RUNNING' 90000) (St)
Remove-LocalGroupMember -SID $adm -Member 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
