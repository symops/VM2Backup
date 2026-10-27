# domain-member2.ps1 (ВМ vm2b-member, после ввода в домен): RSAT, gMSA на этой машине, локальная шара «memberlocal» с правами домена, проверка времени и Kerberos.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom
$cs = Get-WmiObject Win32_ComputerSystem
Check 'MEMBER is a domain member (vm2b.test)' ($cs.PartOfDomain -and $cs.Domain -eq 'vm2b.test') ($cs | Out-String)
Check 'secure channel to the domain (Test-ComputerSecureChannel)' (Test-ComputerSecureChannel) ''
$f = Install-WindowsFeature RSAT-AD-PowerShell; Check 'RSAT-AD-PowerShell installed' ($f.Success) ($f | Out-String)
Import-Module ActiveDirectory
$o = Install-ADServiceAccount $d.Gmsa -ErrorAction SilentlyContinue 2>&1; Write-Output ("INFO Install-ADServiceAccount: " + ($o | Out-String).Trim())
Check 'Test-ADServiceAccount vm2bgmsa$ = True (this computer may use the gMSA)' (Test-ADServiceAccount $d.Gmsa) ''
Write-Output ("INFO w32tm status:`n" + ((& w32tm.exe /query /status) -join "`n"))
Write-Output ("INFO time here: " + (Get-Date).ToString('s') + "; w32tm /monitor: " + ((& w32tm.exe /monitor /computers:dc.vm2b.test 2>&1) -join ' ' -replace '\s+', ' '))
$dcTime = Invoke-Command -ComputerName dc.vm2b.test -Credential (Dom-AdminCred) -ScriptBlock { (Get-Date).ToString('s') } -ErrorAction SilentlyContinue
Write-Output ("INFO time on DC: $dcTime")
if ($dcTime) { Check 'clocks of DC and MEMBER differ by under 60 s' ([Math]::Abs(((Get-Date) - [datetime]$dcTime).TotalSeconds) -lt 60) $dcTime }
# локальная шара на member: C:\Shares\Local, права группе VM2B-Backup-Operators
New-Item -ItemType Directory -Force C:\Shares\Local | Out-Null
& icacls.exe C:\Shares\Local /inheritance:r /grant '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' "VM2B\$($d.Group):(OI)(CI)M" | Out-Null
if (Get-SmbShare -Name 'memberlocal' -ErrorAction SilentlyContinue) { Remove-SmbShare -Name memberlocal -Force }
New-SmbShare -Name 'memberlocal' -Path C:\Shares\Local -FullAccess "VM2B\$($d.Group)", (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value | Out-Null
Check 'local share memberlocal created' ($null -ne (Get-SmbShare -Name memberlocal -ErrorAction SilentlyContinue)) ''
Write-Output ("INFO klist (computer tickets):`n" + ((& klist.exe -li 0x3e7 2>&1 | Select-Object -First 12) -join "`n"))
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
