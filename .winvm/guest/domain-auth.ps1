# domain-auth.ps1 <секунды назад>: Kerberos или NTLM? События безопасности контроллера домена за последние N секунд: входы (4624, тип 3) пользователей bkuser / member$
# с пакетом проверки подлинности, и запросы билетов Kerberos (4768 / 4769). Печатает строки INFO для разбора и проверяет, что по именам (FQDN, короткое) шёл Kerberos.
param([int]$Seconds = 900)
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$since = (Get-Date).AddSeconds(-$Seconds)
$r = Invoke-Command -ComputerName dc.vm2b.test -Credential (Dom-AdminCred) -ArgumentList $since -ScriptBlock {
    param($since)
    $o = @()
    Get-WinEvent -FilterHashtable @{ LogName = 'Security'; Id = 4624, 4625, 4768, 4769, 4771, 4776; StartTime = $since } -ErrorAction SilentlyContinue | Sort-Object TimeCreated | ForEach-Object {
        $x = [xml]$_.ToXml(); $d = @{}; foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
        $user = if ($d.TargetUserName) { $d.TargetUserName } else { $d.Account }
        if ($_.Id -eq 4624 -and $d.LogonType -ne '3') { return }
        if ($user -notmatch 'bkuser|lockuser|nouser|svcbackup|MEMBER\$|vm2bgmsa|expuser|chguser|disuser') { return }
        $o += ("{0} id={1} user={2} pkg={3} logonproc={4} ip={5} svc={6} status={7} type={8}" -f $_.TimeCreated.ToString('HH:mm:ss.fff'), $_.Id, $user, $d.AuthenticationPackageName, $d.LogonProcessName, $d.IpAddress, $d.ServiceName, $d.Status, $d.TicketEncryptionType)
    }
    $o
}
$lines = @($r)
Write-Output "INFO security events on the DC in the last $Seconds s: $($lines.Count)"
$lines | ForEach-Object { Write-Output ("INFO ev " + $_) }
$k = @($lines | Where-Object { $_ -match 'id=4624' -and $_ -match 'user=bkuser' -and $_ -match 'pkg=Kerberos' }).Count
$n = @($lines | Where-Object { $_ -match 'id=4624' -and $_ -match 'user=bkuser' -and $_ -match 'pkg=NTLM' }).Count
Write-Output "INFO bkuser network logons: Kerberos=$k NTLM=$n"
Check 'bkuser network logons appear in the DC log (the audit works)' (($k + $n) -ge 1) ''
Check 'Kerberos was used for at least one logon by name (FQDN / short name)' ($k -ge 1) "Kerberos=$k NTLM=$n"
Check 'NTLM was used for at least one logon (the IP address has no SPN)' ($n -ge 1) "Kerberos=$k NTLM=$n"
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
