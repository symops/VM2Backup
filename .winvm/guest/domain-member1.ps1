# domain-member1.ps1 (ВМ vm2b-member): проверка сети и ввод в домен vm2b.test (перезагрузку делает хост).
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom
Check 'the computer is named MEMBER' ($env:COMPUTERNAME -eq 'MEMBER') $env:COMPUTERNAME
Get-NetIPAddress -AddressFamily IPv4 | ForEach-Object { Write-Output ("INFO   ip " + $_.InterfaceAlias + " " + $_.IPAddress) }
Check 'the DC answers on the internal network (ping 10.77.0.1)' (Test-Connection -Count 2 -Quiet $d.DcIp) ''
Check 'DNS resolves dc.vm2b.test to 10.77.0.1' ((Wait-Dns 'dc.vm2b.test' 300) -and ((Resolve-DnsName dc.vm2b.test -Type A).IPAddress -contains $d.DcIp)) ((Resolve-DnsName dc.vm2b.test -ErrorAction SilentlyContinue | Out-String))
Add-Computer -DomainName $d.Name -Credential (Dom-AdminCred) -Force -ErrorAction Stop -WarningAction SilentlyContinue
Check 'Add-Computer' $? ''
Set-AutoLogon $d.Netbios (Get-BuiltinAdminName) $d.AdminPw      # после ввода в домен: VM2B\<имя из SID -500>
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
