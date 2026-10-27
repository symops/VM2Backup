# domain-dc2.ps1 (ВМ vm2b-dc, после повышения до контроллера): DNS только на внутренней карте, KDS root key, OU, пользователи, группы, PSO блокировки, шары.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; Import-Module ActiveDirectory -ErrorAction Stop
$t0 = Get-Date; $ok = $false
while (((Get-Date) - $t0).TotalSeconds -lt 600) { try { $x = Get-ADDomain -ErrorAction Stop; $ok = $true; break } catch { Start-Sleep -Seconds 10 } }
Check 'AD answers (Get-ADDomain)' $ok ''
if (-not $ok) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
Write-Output ("INFO domain: " + $x.DNSRoot + " NetBIOS " + $x.NetBIOSName + " mode " + $x.DomainMode + "; forest " + (Get-ADForest).Name)
Check 'domain vm2b.test / NetBIOS VM2B' ($x.DNSRoot -eq 'vm2b.test' -and $x.NetBIOSName -eq 'VM2B') ($x | Out-String)
# DNS слушает только внутренний адрес; запись A хоста-карты (10.0.2.15) убираем: у member тот же 10.0.2.15 на его карте хоста
& dnscmd.exe /ResetListenAddresses $d.DcIp | Out-Null
Restart-Service DNS -ErrorAction SilentlyContinue
Get-DnsServerResourceRecord -ZoneName $d.Name -Name '@' -RRType A -ErrorAction SilentlyContinue | Where-Object { $_.RecordData.IPv4Address.ToString() -ne $d.DcIp } | Remove-DnsServerResourceRecord -ZoneName $d.Name -Force -ErrorAction SilentlyContinue
Get-DnsServerResourceRecord -ZoneName $d.Name -Name 'dc' -RRType A -ErrorAction SilentlyContinue | Where-Object { $_.RecordData.IPv4Address.ToString() -ne $d.DcIp } | Remove-DnsServerResourceRecord -ZoneName $d.Name -Force -ErrorAction SilentlyContinue
Get-DnsServerResourceRecord -ZoneName $d.Name -RRType A -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("INFO dns A: " + $_.HostName + " -> " + $_.RecordData.IPv4Address) }
& ipconfig.exe /registerdns | Out-Null; & nltest.exe /dsregdns 2>&1 | Out-Null
# KDS root key: действует сразу (EffectiveTime в прошлом) - иначе gMSA ждёт 10 часов репликации
$k = Get-KdsRootKey; if (-not $k) { Add-KdsRootKey -EffectiveTime ((Get-Date).AddHours(-10)) | Out-Null; $k = Get-KdsRootKey }
Check 'KDS root key exists and is already effective' ($k -and ($k | Select-Object -First 1).EffectiveTime -lt (Get-Date)) ($k | Out-String)
# OU, группы, пользователи
if (-not (Get-ADOrganizationalUnit -Filter "Name -eq 'VM2B-Test'")) { New-ADOrganizationalUnit -Name 'VM2B-Test' -Path 'DC=vm2b,DC=test' -ProtectedFromAccidentalDeletion $false }
if (-not (Get-ADGroup -Filter "Name -eq '$($d.Group)'")) { New-ADGroup -Name $d.Group -GroupScope Global -Path $d.OU }
function New-U($name, $pw, [bool]$enabled = $true) {
    if (Get-ADUser -Filter "SamAccountName -eq '$name'") { Remove-ADUser -Identity $name -Confirm:$false }
    New-ADUser -Name $name -SamAccountName $name -UserPrincipalName "$name@vm2b.test" -Path $d.OU -AccountPassword (ConvertTo-SecureString $pw -AsPlainText -Force) -Enabled $true -PasswordNeverExpires $true -CannotChangePassword $false | Out-Null
}
New-U $d.Svc $d.SvcPw; New-U $d.Bk $d.BkPw; New-U $d.No $d.NoPw; New-U $d.Lock $d.LockPw; New-U $d.MustChange $d.MustChangePw; New-U $d.Expired $d.ExpiredPw; New-U $d.Disabled $d.DisabledPw
Add-ADGroupMember -Identity $d.Group -Members $d.Svc, $d.Bk, $d.Lock, $d.MustChange, $d.Expired, $d.Disabled
# 1907: «требуется смена пароля при входе»; 1330: пароль истёк (PasswordNeverExpires выключен, pwdLastSet 200 суток назад при максимуме 42); 1331: учётная запись отключена
Set-ADUser $d.MustChange -PasswordNeverExpires $false -ChangePasswordAtLogon $true
Set-ADUser $d.Expired -PasswordNeverExpires $false
# 1330 (срок действия пароля истёк) в домене без ожидания 42 суток получить нельзя: pwdLastSet принимает только 0 или -1; expuser получает то же состояние, что chguser
Set-ADUser $d.Expired -ChangePasswordAtLogon $true
Disable-ADAccount $d.Disabled
# PSO: блокировка после 5 неверных попыток - только у lockuser (остальные учётные записи не блокируются: домен по умолчанию порога не имеет)
$pso = Get-ADFineGrainedPasswordPolicy -Filter "Name -eq 'VM2B-Lockout'"; if ($pso) { Remove-ADFineGrainedPasswordPolicy $pso -Confirm:$false }
New-ADFineGrainedPasswordPolicy -Name 'VM2B-Lockout' -Precedence 10 -LockoutThreshold 5 -LockoutObservationWindow '00:30:00' -LockoutDuration '00:30:00' -ComplexityEnabled $true -MinPasswordLength 7 -PasswordHistoryCount 1 -MinPasswordAge '0.00:00:00' -MaxPasswordAge '42.00:00:00' -ReversibleEncryptionEnabled $false
Add-ADFineGrainedPasswordPolicySubject 'VM2B-Lockout' -Subjects $d.Lock
Check 'users created' ((Get-ADUser -Filter * -SearchBase $d.OU).Count -ge 7) ''
Get-ADUser -Filter * -SearchBase $d.OU -Properties Enabled, PasswordExpired, pwdLastSet, LockedOut | ForEach-Object { Write-Output ("INFO user " + $_.SamAccountName + " enabled=" + $_.Enabled + " pwdExpired=" + $_.PasswordExpired + " lockedOut=" + $_.LockedOut) }
Write-Output ("INFO default domain policy: " + ((Get-ADDefaultDomainPasswordPolicy | Select-Object LockoutThreshold, MaxPasswordAge, MinPasswordLength | Out-String) -replace '\s+', ' '))
# шары: Backups (группа + учётная запись компьютера MEMBER$ напрямую), Restricted (только администраторы: у member$ и обычных пользователей права нет)
function New-Dir($path, $share, $grant) {
    New-Item -ItemType Directory -Force $path | Out-Null
    if (Get-SmbShare -Name $share -ErrorAction SilentlyContinue) { Remove-SmbShare -Name $share -Force }
    & icacls.exe $path /inheritance:r /grant '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' | Out-Null
    foreach ($g in $grant) { & icacls.exe $path /grant "${g}:(OI)(CI)M" | Out-Null }
}
$admName = (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value   # на русской Windows группа называется иначе
$grantNames = @($d.Netbios + '\' + $d.Group)
New-Dir 'C:\Shares\Backups' 'Backups' $grantNames
New-Dir 'C:\Shares\Restricted' 'Restricted' @()
New-SmbShare -Name 'Backups' -Path 'C:\Shares\Backups' -FullAccess "$($d.Netbios)\$($d.Group)" -ChangeAccess $admName | Out-Null
New-SmbShare -Name 'Restricted' -Path 'C:\Shares\Restricted' -FullAccess $admName | Out-Null
Get-SmbShare | Where-Object { $_.Name -in 'Backups', 'Restricted' } | ForEach-Object { Write-Output ("INFO share " + $_.Name + " " + $_.Path) }
Check 'shares Backups / Restricted exist' ((Get-SmbShare -Name Backups, Restricted -ErrorAction SilentlyContinue).Count -eq 2) ''
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
