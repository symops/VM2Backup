# domain-badpwd.ps1 <пользователь>: печатает BADPWD=<badPwdCount> и LOCKED=<0|1> учётной записи, спрашивая контроллер домена (его же вызывают автотесты группы domainlock).
param([string]$User)
. C:\vm2b\guest\domain-lib.ps1
$u = ($User -split '[\\@]')[-1]
Import-Module ActiveDirectory -ErrorAction SilentlyContinue
try { $a = Get-ADUser $u -Server dc.vm2b.test -Credential (Dom-AdminCred) -Properties badPwdCount, LockedOut -ErrorAction Stop; Write-Output ("BADPWD=" + $a.badPwdCount); Write-Output ("LOCKED=" + [int]$a.LockedOut) }
catch { Write-Output ("BADPWD=-1 " + $_.Exception.Message) }
