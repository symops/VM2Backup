# domain-chpwd.ps1 <пользователь> <старый пароль> <новый пароль>: смена пароля пользователя в домене (то же, что сделал бы пользователь по требованию «пароль истёк»); печатает CHPWD=OK или причину.
param([string]$User, [string]$Old, [string]$New)
. C:\vm2b\guest\domain-lib.ps1
$u = ($User -split '[\\@]')[-1]
Import-Module ActiveDirectory -ErrorAction SilentlyContinue
try {
    Set-ADAccountPassword -Identity $u -OldPassword (ConvertTo-SecureString $Old -AsPlainText -Force) -NewPassword (ConvertTo-SecureString $New -AsPlainText -Force) -Server dc.vm2b.test -Credential (Dom-AdminCred) -ErrorAction Stop
    Write-Output 'CHPWD=OK (change with the old password)'
} catch {
    $m1 = $_.Exception.Message
    try { Set-ADAccountPassword -Identity $u -Reset -NewPassword (ConvertTo-SecureString $New -AsPlainText -Force) -Server dc.vm2b.test -Credential (Dom-AdminCred) -ErrorAction Stop; Write-Output ('CHPWD=OK (administrator reset; the change with the old password failed: ' + $m1 + ')') }
    catch { Write-Output ('CHPWD=FAILED ' + $m1 + ' / ' + $_.Exception.Message) }
}
