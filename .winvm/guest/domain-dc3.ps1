# domain-dc3.ps1 (ВМ vm2b-dc, ПОСЛЕ ввода member в домен): права учётной записи компьютера MEMBER$ на шару Backups (напрямую, не через группу), gMSA vm2bgmsa$.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; Import-Module ActiveDirectory
$c = Get-ADComputer -Identity MEMBER -ErrorAction SilentlyContinue
Check 'the computer account MEMBER$ exists in the domain' ($null -ne $c) ''
if (-not $c) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
& icacls.exe 'C:\Shares\Backups' /grant 'VM2B\MEMBER$:(OI)(CI)M' | Out-Null
Grant-SmbShareAccess -Name Backups -AccountName 'VM2B\MEMBER$' -AccessRight Full -Force | Out-Null
Write-Output ("INFO share Backups: " + ((Get-SmbShareAccess -Name Backups | ForEach-Object { $_.AccountName + '=' + $_.AccessRight }) -join '; '))
$g = Get-ADServiceAccount -Filter "Name -eq '$($d.Gmsa)'"; if ($g) { Remove-ADServiceAccount $g -Confirm:$false }
New-ADServiceAccount -Name $d.Gmsa -DNSHostName "$($d.Gmsa).vm2b.test" -PrincipalsAllowedToRetrieveManagedPassword 'MEMBER$' -KerberosEncryptionType AES128, AES256 -ManagedPasswordIntervalInDays 1 -Enabled $true
$g = Get-ADServiceAccount $d.Gmsa -Properties PrincipalsAllowedToRetrieveManagedPassword, msDS-ManagedPasswordInterval
Check 'gMSA vm2bgmsa$ created, MEMBER$ may retrieve its password' ($g -and ($g.PrincipalsAllowedToRetrieveManagedPassword -join ' ') -match 'MEMBER') ($g | Out-String)
Write-Output ("INFO gMSA: " + $g.DistinguishedName + "  password interval (days): " + $g.'msDS-ManagedPasswordInterval')
Add-ADGroupMember -Identity $d.Group -Members ($d.Gmsa + '$')
& icacls.exe 'C:\Shares\Backups' /grant "VM2B\$($d.Gmsa)`$:(OI)(CI)M" | Out-Null
Write-Output ("INFO group members: " + ((Get-ADGroupMember $d.Group | ForEach-Object { $_.SamAccountName }) -join ', '))
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
