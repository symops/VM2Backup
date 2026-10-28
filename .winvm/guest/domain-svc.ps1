# domain-svc.ps1 (ВМ vm2b-member, продукт установлен): служба VM2Backup под LocalSystem / доменным пользователем / gMSA на настоящем домене AD.
#  1. LocalSystem: пустой пользователь в записи SMB = учётная запись КОМПЬЮТЕРА MEMBER$ (Kerberos): копия, проверка, восстановление на \\dc\Backups; отказ на Restricted;
#  2. VM2B\svcbackup (обычный доменный пользователь, в локальных администраторах member): то же + шара на самом member;
#  3. VM2B\vm2bgmsa$ (gMSA, пароль пустой): запуск, копия, перезапуск, ротация пароля (Reset-ADServiceAccountPassword / сдвиг времени), снова запуск и копия;
#  6. DPAPI: секреты, записанные под одной учётной записью, читаются под другими (машинная область);
#  7. папка данных: права SYSTEM и администраторов; доменный обычный пользователь не читает config.json;
# 10. служба под доменной записью без права на папку данных: STOPPED и понятная причина в журнале событий Windows.
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$d = $global:Dom; $data = $global:DataDir; $svc = $global:SvcName
$fq = '\\dc.vm2b.test\Backups'; $short = '\\dc\Backups'; $ipu = '\\10.77.0.1\Backups'; $restr = '\\dc.vm2b.test\Restricted'; $loc = '\\member.vm2b.test\memberlocal'
$adm = 'S-1-5-32-544'
function Cycle($w, [string]$tag, [string]$srv) {       # копия + проверка + восстановление; результат: строка с причиной сбоя или '' при успехе
    $name = "dom-$tag-" + (Get-Date).ToString('HHmmss')
    $h = Run-Backup $w $name 'C:\vm2b\domdata' 'vmS' $srv '{VM}'
    if (-not $h -or $h.status -ne 'Success') { return "backup: " + ($h | ConvertTo-Json -Compress) }
    $bl = Api $w 'backups.list' @{ server = $srv }; $b = @($bl.Result.backups) | Select-Object -Last 1
    if (-not $b) { return 'backups.list: nothing on the share: ' + $bl.Text }
    $dest = "C:\vm2b\domrestore-$tag"; Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
    foreach ($op in 'verify', 'restore') {
        $a = @{ server = $srv; directory = $b.directory; archive = $b.archive }; if ($op -eq 'restore') { $a.dest = $dest }
        $r = Api $w "backups.$op" $a; if ($r.Status -ne 200 -or -not $r.Result.job) { return "backups.$op : " + $r.Text }
        $st = ''; for ($i = 0; $i -lt 120; $i++) { Start-Sleep -Seconds 1; $j = (Api $w 'jobs.list' @{}).Result | Where-Object { $_.id -eq $r.Result.job }; if (-not $j) { $st = 'Gone'; break }; if ($j.status -ne 'Running' -and $j.status -ne 'Waiting') { $st = $j.status; break } }
        if ($st -ne 'Success' -and $st -ne 'Gone') { return "backups.$op status=$st " + ($j | ConvertTo-Json -Compress) }
    }
    $f1 = (Get-FileHash 'C:\vm2b\domdata\vmS\disk.vmdk').Hash; $rf = Get-ChildItem $dest -Recurse -Filter 'disk.vmdk' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $rf) { return 'restored file not found' }; if ((Get-FileHash $rf.FullName).Hash -ne $f1) { return 'restored data differs' }
    return ''
}
function Own-Info($w, [string]$id, [string]$what) { $t = Api $w 'servers.test' @{ id = $id }; Write-Output ("INFO servers.test $what : status=" + $t.Status + " " + $t.Text.Substring(0, [Math]::Min(260, $t.Text.Length))); return $t }
function Save-Smb($w, [string]$name, [string]$unc, [string]$user, [string]$pw) {
    $t0 = Get-Date; $s = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = $name; path = $unc; user = $user; password = $pw } }; $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    if ($s.Result -and $s.Result.id) { if ($ms -gt 3000) { Write-Output "INFO servers.save '$name' was slow: $ms ms" }; return $s.Result.id }
    # разбор F: ответ без идентификатора. Сохраняем текст ответа и состояние на диске/в памяти, потом берём идентификатор записи, если она всё же есть (устойчивость скрипта)
    Write-Output ("INFO SAVE-NO-ID '$name': status " + $s.Status + " after $ms ms; body: " + $s.Text)
    $l = Api $w 'servers.list' @{}; $f = @($l.Result) | Where-Object { $_.name -eq $name } | Select-Object -First 1
    Write-Output ("INFO SAVE-NO-ID '$name': record in servers.list: " + $(if ($f) { $f.id } else { 'no' }))
    try { $cj = Get-Content "$env:ProgramData\VM2Backup\config.json" -Raw -ErrorAction Stop; Write-Output ("INFO SAVE-NO-ID '$name': on disk: " + $(if ($cj -match [regex]::Escape($name)) { 'yes' } else { 'NO (memory only)' })) } catch { Write-Output ("INFO config.json not readable: " + $_.Exception.Message) }
    if ($f) { return $f.id }; return $null
}
function Common-Checks($w, [string]$who, [string]$label) {
    $i = Svc-Info; Write-Output ("INFO $label : " + (($i | Out-String) -replace '\s+', ' ')); Check "[$label] the process owner is $who" ($i.Owner -match [regex]::Escape($who.Split('\')[-1].TrimEnd('$'))) ($i | Out-String)
    $l = Api $w 'servers.list' @{}; $now = @($l.Result | ForEach-Object { $_.id }); $miss = @($script:ids.Values | Where-Object { $now -notcontains $_ })
    Check "[$label] all servers are still there" ($miss.Count -eq 0) $l.Text
    $t = Api $w 'servers.test' @{ id = $script:ids['explicit'] }; Check "[$label] DPAPI: the secret saved by LocalSystem works (explicit VM2B\bkuser)" ($t.Status -eq 200) $t.Text
}

# ---- 0. исходное состояние
$w = Web-Ready; Check 'web site is up' ($null -ne $w) ''
if (-not $w) { Write-Output 'FAILED=1'; exit 1 }
Check 'the service runs as LocalSystem at the start' ((Svc-Info).StartName -match 'LocalSystem') ((Svc-Info) | Out-String)
$cs = Get-WmiObject Win32_ComputerSystem; Check 'MEMBER is in the domain' ($cs.PartOfDomain) ($cs | Out-String)
$old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -like 'dom*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$script:ids = @{}
$script:ids['explicit'] = Save-Smb $w 'dom explicit bkuser' $fq 'bkuser@vm2b.test' $d.BkPw
$script:ids['own-fqdn'] = Save-Smb $w 'dom own fqdn' $fq '' ''
$script:ids['own-short'] = Save-Smb $w 'dom own short' $short '' ''
$script:ids['own-ip'] = Save-Smb $w 'dom own ip' $ipu '' ''
$script:ids['own-restricted'] = Save-Smb $w 'dom own restricted' $restr '' ''
$script:ids['own-local'] = Save-Smb $w 'dom own member local' $loc '' ''
Check 'six servers saved' (@($script:ids.Values | Where-Object { $_ }).Count -eq 6) ($script:ids | Out-String)
Make-Data 'C:\vm2b\domdata' 6
Write-Output ("INFO icacls data folder:`n" + ((& icacls.exe $data) -join "`n"))

# ---- 1. LocalSystem = MEMBER$
$t = Own-Info $w $script:ids['explicit'] 'explicit bkuser (SYSTEM)'; Check '[LocalSystem] explicit domain user works' ($t.Status -eq 200) $t.Text
$t = Own-Info $w $script:ids['own-fqdn'] 'empty user, FQDN, SYSTEM = MEMBER$'; Check '[LocalSystem] empty user on \\dc.vm2b.test\Backups: the computer account MEMBER$ gets in (Kerberos)' ($t.Status -eq 200) $t.Text
$t = Own-Info $w $script:ids['own-short'] 'empty user, short name, SYSTEM'; Check '[LocalSystem] empty user on \\dc\Backups works' ($t.Status -eq 200) $t.Text
$t = Own-Info $w $script:ids['own-ip'] 'empty user, IP address, SYSTEM (NTLM)'; Write-Output ("INFO [LocalSystem] by IP address (no SPN -> NTLM, a machine account cannot): status " + $t.Status)
$t = Own-Info $w $script:ids['own-restricted'] 'empty user, Restricted, SYSTEM'
Check '[LocalSystem] the share where MEMBER$ has no right: a clear refusal (access denied)' ($t.Status -ne 200 -or $t.Json.ok -eq $false) $t.Text
Check '[LocalSystem] the refusal says access denied / no right, no secret' ($t.Text -match '(?i)access denied|denied|no right' -and -not $t.Text.Contains($d.BkPw)) $t.Text
$e = Cycle $w 'sys-fqdn' $script:ids['own-fqdn']; Check '[LocalSystem] backup + verify + restore on \\dc.vm2b.test\Backups with the empty user (MEMBER$)' ($e -eq '') $e
$e = Cycle $w 'sys-short' $script:ids['own-short']; Check '[LocalSystem] backup + verify + restore on \\dc\Backups with the empty user' ($e -eq '') $e
$h = Run-Backup $w ('dom-sys-restr-' + (Get-Date).ToString('HHmmss')) 'C:\vm2b\domdata' 'vmS' $script:ids['own-restricted'] '{VM}'
Write-Output ("INFO [LocalSystem] backup to Restricted: " + ($h | ConvertTo-Json -Compress)); Check '[LocalSystem] backup to the share without rights: Failed, not Success' ($h -and $h.status -ne 'Success') ($h | ConvertTo-Json -Compress)
$e = Cycle $w 'sys-explicit' $script:ids['explicit']; Check '[LocalSystem] backup + verify + restore with the explicit VM2B bkuser@vm2b.test' ($e -eq '') $e

# ---- 2. доменный пользователь VM2B\svcbackup
Add-LocalGroupMember -SID $adm -Member 'VM2B\svcbackup' -ErrorAction SilentlyContinue
Grant-LogonRight 'VM2B\svcbackup'
Switch-Account 'VM2B\svcbackup' $d.SvcPw
Check '[svcbackup] service RUNNING under VM2B\svcbackup' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready; Check '[svcbackup] web site up' ($null -ne $w) ''
if ($w) {
    Check '[svcbackup] StartName is the domain account' ((Svc-Info).StartName -match 'svcbackup') ((Svc-Info) | Out-String)
    Common-Checks $w 'VM2B\svcbackup' 'svcbackup'
    $t = Own-Info $w $script:ids['own-fqdn'] 'empty user, FQDN, svcbackup'; Check '[svcbackup] empty user = the rights of VM2B\svcbackup (not MEMBER$)' ($t.Status -eq 200) $t.Text
    $e = Cycle $w 'svc-fqdn' $script:ids['own-fqdn']; Check '[svcbackup] backup + verify + restore on \\dc.vm2b.test\Backups, empty user' ($e -eq '') $e
    $e = Cycle $w 'svc-short' $script:ids['own-short']; Check '[svcbackup] backup + verify + restore on \\dc\Backups, empty user' ($e -eq '') $e
    $e = Cycle $w 'svc-local' $script:ids['own-local']; Write-Output ("INFO [svcbackup] share on MEMBER itself (loopback): " + $(if ($e) { $e } else { 'OK' })); Check '[svcbackup] backup to the share on this very computer (memberlocal)' ($e -eq '') $e
    $t = Own-Info $w $script:ids['own-restricted'] 'empty user, Restricted, svcbackup'; Check '[svcbackup] Restricted: refused (the group has no right there)' ($t.Status -ne 200 -or $t.Json.ok -eq $false) $t.Text
    $e = Cycle $w 'svc-explicit' $script:ids['explicit']; Check '[svcbackup] explicit bkuser: backup + verify + restore' ($e -eq '') $e
    # DPAPI в обратную сторону: секрет сохранён ДОМЕННОЙ службой, потом читается под LocalSystem
    $script:ids['made-by-svc'] = Save-Smb $w 'dom saved by svcbackup' $fq 'VM2B\bkuser' $d.BkPw
    $t = Api $w 'servers.test' @{ id = $script:ids['made-by-svc'] }; Check '[svcbackup] a server with a password saved under the domain account works right away' ($t.Status -eq 200) $t.Text
}
# ---- 7. права на папку данных, обычный доменный пользователь nouser (без прав администратора)
Grant-LogonRight 'VM2B\nouser'
$ur = 'SeInteractiveLogonRight'; $sid = (New-Object Security.Principal.NTAccount('VM2B\nouser')).Translate([Security.Principal.SecurityIdentifier]).Value
& secedit.exe /export /cfg C:\vm2b\sec-in.inf /areas USER_RIGHTS | Out-Null
$tx = Get-Content C:\vm2b\sec-in.inf -Encoding Unicode; $tx = $tx | ForEach-Object { if ($_ -match "^$ur\s*=" -and $_ -notmatch [regex]::Escape($sid)) { $_ + ",*$sid" } else { $_ } }; $tx | Set-Content C:\vm2b\sec-out.inf -Encoding Unicode
& secedit.exe /configure /db C:\vm2b\sec.sdb /cfg C:\vm2b\sec-out.inf /areas USER_RIGHTS 2>&1 | Out-Null
$out = 'C:\vm2b\acl-dom.txt'; Remove-Item $out -ErrorAction SilentlyContinue
$cred = New-Object Management.Automation.PSCredential('VM2B\nouser', (ConvertTo-SecureString $d.NoPw -AsPlainText -Force))
try { Start-Process powershell.exe -Credential $cred -WorkingDirectory C:\vm2b -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\vm2b\guest\acltest.ps1', $out -Wait -WindowStyle Hidden -ErrorAction Stop } catch { Write-Output ("INFO START FAILED " + $_.Exception.Message) }
$acl = if (Test-Path $out) { (Get-Content $out) -join ' | ' } else { 'NO RESULT' }; Write-Output ("INFO a standard DOMAIN user vs the data folder: $acl")
Check '[data folder] a standard domain user cannot read config.json' ($acl -match 'READ_DENIED config.json') $acl
Check '[data folder] a standard domain user cannot decrypt the machine-scope secrets (no read access)' ($acl -notmatch 'UNPROTECT_OK') $acl
Check '[data folder] a standard domain user cannot write config.json' ($acl -match 'WRITE_DENIED config.json') $acl
Check '[data folder] a standard domain user cannot read web.json' ($acl -notmatch 'READ_OK web.json') $acl
$dacl = (& icacls.exe $data) -join ' '
$sysN = (New-Object Security.Principal.SecurityIdentifier('S-1-5-18')).Translate([Security.Principal.NTAccount]).Value
$admN = (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')).Translate([Security.Principal.NTAccount]).Value
Check '[data folder] only SYSTEM and Administrators have rights (no Users / Authenticated Users / Everyone / domain accounts on the folder itself)' (($dacl -match [regex]::Escape($sysN)) -and ($dacl -match [regex]::Escape($admN)) -and ($dacl -notmatch 'Everyone|Authenticated|BUILTIN\\Users|VM2B\\')) $dacl

# ---- 3. gMSA
Add-LocalGroupMember -SID $adm -Member 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Grant-LogonRight 'VM2B\vm2bgmsa$'
Import-Module ActiveDirectory
Check '[gMSA] Test-ADServiceAccount vm2bgmsa$ before the switch' (Test-ADServiceAccount $d.Gmsa) ''
Switch-Account 'VM2B\vm2bgmsa$' ''
Check '[gMSA] service RUNNING under VM2B\vm2bgmsa$ (empty password)' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready; Check '[gMSA] web site up' ($null -ne $w) ''
if ($w) {
    Check '[gMSA] StartName is vm2bgmsa$' ((Svc-Info).StartName -match 'vm2bgmsa') ((Svc-Info) | Out-String)
    Common-Checks $w 'VM2B\vm2bgmsa$' 'gMSA'
    $t = Api $w 'servers.test' @{ id = $script:ids['made-by-svc'] }; Check '[gMSA] DPAPI: the secret saved under svcbackup works under the gMSA' ($t.Status -eq 200) $t.Text
    $t = Own-Info $w $script:ids['own-fqdn'] 'empty user, FQDN, gMSA'; Check '[gMSA] empty user = the rights of the gMSA (it is in VM2B-Backup-Operators)' ($t.Status -eq 200) $t.Text
    $e = Cycle $w 'gmsa-fqdn' $script:ids['own-fqdn']; Check '[gMSA] backup + verify + restore on \\dc.vm2b.test\Backups, empty user' ($e -eq '') $e
    $t = Own-Info $w $script:ids['own-restricted'] 'empty user, Restricted, gMSA'; Check '[gMSA] Restricted: refused' ($t.Status -ne 200 -or $t.Json.ok -eq $false) $t.Text
}
# перезапуск службы
& net.exe stop $svc 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null; & net.exe start $svc 2>&1 | Out-Null
Check '[gMSA] RUNNING after a service restart' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready
if ($w) { $e = Cycle $w 'gmsa-restart' $script:ids['own-fqdn']; Check '[gMSA] backup works after the restart' ($e -eq '') $e }
# (ротация пароля gMSA: фаза rotate, domain-rot1.ps1 / domain-rot2.ps1)

# ---- 10. доменная запись без права на папку данных
Switch-Account 'VM2B\nouser' $d.NoPw
Start-Sleep -Seconds 12; $state = St; Write-Output "INFO service state under VM2B\nouser (no rights to the data folder): $state"
Check '[no rights] the service does NOT run half-working (STOPPED)' ($state -match 'STOPPED') $state
$evs = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VM2BackupService' } -MaxEvents 5 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'cannot start' })
Write-Output ("INFO Application log: " + $(if ($evs.Count) { ($evs[0].Message -split "`r?`n")[0] } else { '(no event)' }))
Check '[no rights] the Windows event log has the start failure with its reason (source VM2BackupService)' ($evs.Count -ge 1) 'no event "cannot start"'

# ---- возврат на LocalSystem
Switch-Account '' $null
Check 'service RUNNING again as LocalSystem' (Wait-State 'RUNNING' 90000) (St)
$w = Web-Ready
if ($w) { $t = Api $w 'servers.test' @{ id = $script:ids['made-by-svc'] }; Check '[LocalSystem again] DPAPI: the secret saved under svcbackup works under LocalSystem' ($t.Status -eq 200) $t.Text
    foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -like 'dom*' -or $x.name -like 'src dom*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } } }
Remove-LocalGroupMember -SID $adm -Member 'VM2B\svcbackup', 'VM2B\vm2bgmsa$' -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
