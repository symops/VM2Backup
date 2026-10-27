# domain-unlock.ps1: после тестов блокировки - разблокировать lockuser (порядок для следующих прогонов) и показать состояние.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
Import-Module ActiveDirectory
$a = Get-ADUser lockuser -Server dc.vm2b.test -Credential (Dom-AdminCred) -Properties LockedOut, badPwdCount
Write-Output ("INFO lockuser before: LockedOut=" + $a.LockedOut + " badPwdCount=" + $a.badPwdCount)
Unlock-ADAccount lockuser -Server dc.vm2b.test -Credential (Dom-AdminCred)
$a = Get-ADUser lockuser -Server dc.vm2b.test -Credential (Dom-AdminCred) -Properties LockedOut, badPwdCount
Check 'lockuser unlocked' (-not $a.LockedOut) ''
exit 0
