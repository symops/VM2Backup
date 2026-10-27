# domain-dc1.ps1 (ВМ vm2b-dc): роли AD DS + DNS, лес vm2b.test (NetBIOS VM2B). Перезагрузку делает хост (reboot_guest): задание её не переживает.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom
Write-Output "INFO computer: $env:COMPUTERNAME, adapters:"; Get-NetAdapter | ForEach-Object { Write-Output ("INFO   " + $_.Name + " " + $_.MacAddress + " " + $_.Status) }
Get-NetIPAddress -AddressFamily IPv4 | ForEach-Object { Write-Output ("INFO   ip " + $_.InterfaceAlias + " " + $_.IPAddress) }
Check 'the computer is named DC' ($env:COMPUTERNAME -eq 'DC') $env:COMPUTERNAME
$f = Install-WindowsFeature AD-Domain-Services, DNS, RSAT-AD-PowerShell, RSAT-ADDS-Tools, RSAT-DNS-Server -IncludeManagementTools
Check 'AD DS + DNS installed' ($f.Success) ($f | Out-String)
Import-Module ADDSDeployment
$sm = ConvertTo-SecureString $d.DsrmPw -AsPlainText -Force
$r = Install-ADDSForest -DomainName $d.Name -DomainNetbiosName $d.Netbios -SafeModeAdministratorPassword $sm -InstallDns:$true -CreateDnsDelegation:$false `
    -DomainMode WinThreshold -ForestMode WinThreshold -NoRebootOnCompletion:$true -Force:$true -SkipPreChecks:$false -WarningAction SilentlyContinue
Write-Output ("INFO Install-ADDSForest: " + (($r | Out-String) -replace '\s+', ' '))
Check 'forest created (Status Success / needs restart)' ($r.Status -match 'Success|Error' -and $r.Status -notmatch 'Error') ($r | Out-String)
# автовход после повышения: уже доменный Administrator (локального больше нет)
$wl = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Set-ItemProperty $wl AutoAdminLogon '1'; Set-ItemProperty $wl DefaultUserName 'Administrator'; Set-ItemProperty $wl DefaultDomainName $d.Netbios; Set-ItemProperty $wl DefaultPassword $d.AdminPw
Set-ItemProperty $wl AutoLogonCount 65000 -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
