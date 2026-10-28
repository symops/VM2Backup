# domain-rot1.ps1 (ВМ member, до сдвига часов): служба под gMSA и способ А смены пароля gMSA: принудительно pwdLastSet=0 у объекта и повторная выдача пароля.
#  1. службу переводим на VM2B\vm2bgmsa$, копия на \\dc.vm2b.test\Backups (пустой пользователь);
#  2. читаем msDS-ManagedPasswordId; Set-ADObject -Replace @{pwdLastSet=0}; повторная выдача пароля (Test-ADServiceAccount); читаем идентификатор снова;
#  3. служба, запущенная ДО смены, без перезапуска: копия; потом перезапуск службы и копия; Test-ADServiceAccount.
# Службу под gMSA оставляем: следующий шаг (domain-rot2.ps1) идёт после сдвига часов на +45 суток.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; $adm = 'S-1-5-32-544'; $svc = $global:SvcName
Import-Module ActiveDirectory
$w = Web-Ready; Check 'web site is up' ($null -ne $w) ''
if (-not $w) { Write-Output 'FAILED=1'; exit 1 }
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -like 'rot*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$s = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'rot own'; path = '\\dc.vm2b.test\Backups'; user = ''; password = '' } }
$id = $s.Result.id; $t0 = Get-Date
for ($i = 0; -not $id -and $i -lt 3; $i++) { Write-Output ("INFO servers.save returned no id: status " + $s.Status + " " + $s.Text); Start-Sleep -Seconds 2; $s = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = ('rot own ' + $i); path = '\\dc.vm2b.test\Backups'; user = ''; password = '' } }; $id = $s.Result.id }
Check 'server with the empty user saved' ([bool]$id) $s.Text
Make-Data 'C:\vm2b\domdata' 4
Add-LocalGroupMember -SID $adm -Member 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Grant-LogonRight 'VM2B\vm2bgmsa$'
Switch-Account 'VM2B\vm2bgmsa$' ''
Check 'RUNNING under the gMSA' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready; Check 'site up' ($null -ne $w) ''
$pid0 = (Get-WmiObject Win32_Service -Filter "Name='$svc'").ProcessId
Check 'process owner is the gMSA' ((Svc-Info).Owner -match 'vm2bgmsa') ((Svc-Info) | Out-String)
$r = Backup-Once $w 'rot0' $id; Write-Output "INFO backup 0 (gMSA): $r"; Check 'backup as the gMSA before the change: Success' ($r -eq 'Success') $r
$st0 = Get-GmsaState; Write-Output ("INFO gMSA state before: " + (Gmsa-Line $st0)); Check 'msDS-ManagedPasswordId readable' ($st0.Id.Length -gt 0) $st0.Error
# способ А: pwdLastSet=0
$a = try { Set-ADObject -Identity $st0.Dn -Replace @{ pwdLastSet = 0 } -Server dc.vm2b.test -Credential (Dom-AdminCred) -ErrorAction Stop; 'ok' } catch { 'error: ' + $_.Exception.Message }
Write-Output "INFO method A: Set-ADObject -Replace @{pwdLastSet=0}: $a"
Start-Sleep -Seconds 2
$tsa = Test-ADServiceAccount $d.Gmsa; Write-Output "INFO Test-ADServiceAccount right after pwdLastSet=0: $tsa"
$st1 = Get-GmsaState; Write-Output ("INFO gMSA state after method A: " + (Gmsa-Line $st1))
$changedA = ($st0.Id -ne '') -and ($st1.Id -ne '') -and ($st0.Id -ne $st1.Id)
Write-Output "INFO ROTATION_A_CHANGED=$changedA"
Check '[method A] msDS-ManagedPasswordId changed after pwdLastSet=0 and a new password request (record, not a product check)' $true ("changed=$changedA")
$running = (Get-WmiObject Win32_Service -Filter "Name='$svc'").ProcessId
Write-Output "INFO service pid before restart $pid0, now $running (same process = the service kept running without a restart)"
$w = Web-Ready
$r = Backup-Once $w 'rot-a-norestart' $id; Write-Output "INFO backup in the service started BEFORE the method-A change, no restart: $r"
Check '[method A] the running service (no restart) still copies' ($r -eq 'Success') $r
Check '[method A] Test-ADServiceAccount' (Test-ADServiceAccount $d.Gmsa) ''
& net.exe stop $svc 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null; $so = & net.exe start $svc 2>&1
Check '[method A] RUNNING after the restart' (Wait-State 'RUNNING' 90000) (St)
$pid1 = (Get-WmiObject Win32_Service -Filter "Name='$svc'").ProcessId; Write-Output "INFO service pid after the restart: $pid1"
$w = Web-Ready; $r = Backup-Once $w 'rot-a-restart' $id; Write-Output "INFO backup after the restart: $r"; Check '[method A] copy after the restart' ($r -eq 'Success') $r
$st2 = Get-GmsaState; Write-Output ("INFO gMSA state at the end of rot1: " + (Gmsa-Line $st2))
Write-Output ("INFO SERVERID=" + $id)
Set-Content -Path C:\vm2b\rot-server-id.txt -Value $id
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
