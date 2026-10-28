# domain-dcinfo.ps1 (ВМ dc): состояние учётных записей и gMSA с самого контроллера (до и после сдвига часов): срок пароля expuser, gMSA.  Параметр: метка в выводе.
param([string]$Tag = '')
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
Import-Module ActiveDirectory
Write-Output ("INFO [$Tag] clock on DC: " + (Get-Date).ToString('s') + "; MaxPasswordAge " + (Get-ADDefaultDomainPasswordPolicy).MaxPasswordAge)
foreach ($u in 'expuser', 'bkuser', 'svcbackup') {
    $a = Get-ADUser $u -Properties PasswordExpired, PasswordLastSet, PasswordNeverExpires, 'msDS-UserPasswordExpiryTimeComputed', LockedOut
    $exp = try { [DateTime]::FromFileTime($a.'msDS-UserPasswordExpiryTimeComputed').ToString('s') } catch { '?' }
    Write-Output ("INFO [$Tag] user $u : PasswordExpired=" + $a.PasswordExpired + " PasswordNeverExpires=" + $a.PasswordNeverExpires + " PasswordLastSet=" + $a.PasswordLastSet + " expiry=" + $exp + " lockedOut=" + $a.LockedOut)
}
$g = Get-GmsaState; Write-Output ("INFO [$Tag] gMSA: " + (Gmsa-Line $g))
$k = Get-KdsRootKey; Write-Output ("INFO [$Tag] KDS root key: " + (($k | ForEach-Object { $_.CreationTime.ToString('s') + ' effective ' + $_.EffectiveTime.ToString('s') }) -join '; '))
exit 0
