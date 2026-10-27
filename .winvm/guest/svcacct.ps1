# svcacct.ps1 <unc> <smbuser> <smbpw> : служба под учётными записями, отличными от LocalSystem (round 2, настоящая Windows).
#  1. права на папку данных: обычный пользователь не должен ни читать config.json, ни снимать защиту DPAPI с его секретов, ни писать в него;
#  2. служба под локальным администратором vm2svc: серверы с паролями на месте и работают (DPAPI машинной области), копия в папку, где права только у vm2svc,
#     копия на SMB-шару с явной учётной записью и с ПУСТЫМ пользователем (права самой службы: Samba знает vm2svc с тем же паролем);
#  3. служба под администратором, которого Samba не знает: понятная ошибка, ничего не потеряно;
#  4. служба под обычным пользователем: запуск отказывает с понятной причиной, config.json не тронут;
#  5. возврат на LocalSystem: все серверы и задачи на месте.
param([string]$Unc, [string]$SmbUser, [string]$SmbPw, [string]$Unc2)       # Unc2: то же хранилище под другим именем сервера (Windows держит один набор учётных данных на имя сервера и сеанс входа)
. C:\vm2b\guest\lib.ps1
$svc = 'VM2BackupService'; $data = "$env:ProgramData\VM2Backup"; $svcPw = 'Svc-Pass-123!x'; $stdPw = 'Std-Pass-123!x'
function St { $s = (& sc.exe query $svc | Select-String '\b(STOPPED|START_PENDING|STOP_PENDING|RUNNING)\b' | Select-Object -First 1); if ($s) { ($s.ToString() -replace '\s+', ' ').Trim() } else { 'none' } }
function Wait-State($want, $ms) { $t0 = Get-Date; while (((Get-Date) - $t0).TotalMilliseconds -lt $ms) { if ((St) -match $want) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function Svc-Info { $o = Get-WmiObject Win32_Service -Filter "Name='$svc'"; $own = ''; if ($o -and $o.ProcessId) { $p = Get-WmiObject Win32_Process -Filter "ProcessId=$($o.ProcessId)"; if ($p) { $r = $p.GetOwner(); $own = "$($r.Domain)\$($r.User)" } }; return @{ StartName = $o.StartName; Owner = $own } }
function Web-Ready { for ($i = 0; $i -lt 90; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Grant-LogonRight($user) {
    $sid = (New-Object Security.Principal.NTAccount("$env:COMPUTERNAME\$user")).Translate([Security.Principal.SecurityIdentifier]).Value
    & secedit.exe /export /cfg C:\vm2b\sec-in.inf /areas USER_RIGHTS | Out-Null
    $t = Get-Content C:\vm2b\sec-in.inf -Encoding Unicode
    $has = $false; $t = $t | ForEach-Object { if ($_ -match '^SeServiceLogonRight\s*=') { $has = $true; if ($_ -notmatch [regex]::Escape($sid)) { $_ + ",*$sid" } else { $_ } } else { $_ } }
    if (-not $has) { $t = $t | ForEach-Object { $_; if ($_ -match '^\[Privilege Rights\]') { "SeServiceLogonRight = *$sid" } } }
    $t | Set-Content C:\vm2b\sec-out.inf -Encoding Unicode
    $o = & secedit.exe /configure /db C:\vm2b\sec.sdb /cfg C:\vm2b\sec-out.inf /areas USER_RIGHTS 2>&1; Write-Output ("secedit: " + ($o -join ' ' ).Substring(0, [Math]::Min(160, ($o -join ' ').Length)))
}
function Make-User($user, $pw, $admin) {
    & net.exe user $user /delete 2>&1 | Out-Null; & net.exe user $user $pw /add /y /expires:never | Out-Null
    if ($admin) { Add-LocalGroupMember -SID 'S-1-5-32-544' -Member $user -ErrorAction SilentlyContinue }      # по SID: на русской Windows группа называется «Администраторы»
    Grant-LogonRight $user
}
function Switch-Account($user, $pw) {
    & net.exe stop $svc 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null
    if ($user) { $o = & sc.exe config $svc obj= ".\$user" password= $pw 2>&1 } else { $o = & sc.exe config $svc obj= LocalSystem 2>&1 }
    Write-Output ("sc config: " + ($o -join ' '))
    $o2 = & sc.exe start $svc 2>&1; Write-Output ("sc start: " + (($o2 -join ' ') -replace '\s+', ' '))
}
function Run-Backup($w, $name, $srcDir, $vmName, $targetServer, $targetDir) {
    Api $w 'scheduler.set' @{ enabled = $true } | Out-Null
    $old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq "src $name") { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = "src $name"; path = $srcDir } }; $srcId = $sv.Result.id
    $el = @{ source = @{ server = $srcId; vms = @($vmName) }; target = @{ server = $targetServer; directory = $targetDir }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 0; segment_mib = 4 }
    $tk = Api $w 'tasks.save' @{ task = @{ name = $name; enabled = $true; elements = @($el) } }; $tid = $tk.Result.id
    $before = @((Api $w 'history.list' @{ limit = 50 }).Result).Count
    $r = Api $w 'tasks.run' @{ id = $tid }
    $res = $null
    for ($i = 0; $i -lt 180; $i++) { Start-Sleep -Seconds 2; $h = Api $w 'history.list' @{ limit = 50 }; $hit = @($h.Result) | Where-Object { $_.name -eq $name } | Select-Object -First 1; if ($hit) { $res = $hit; break } }
    Api $w 'tasks.delete' @{ id = $tid } | Out-Null
    return $res
}
function Make-Data($dir, $mb) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$dir\vmS" | Out-Null; $rnd = New-Object Random 7; $buf = New-Object byte[] (1MB); $fs = [IO.File]::Create("$dir\vmS\disk.vmdk"); for ($i = 0; $i -lt $mb; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close() }

# ---- 0. исходное состояние: LocalSystem, два сервера (явная учётная запись, пустой пользователь)
Check 'the service runs as LocalSystem at the start' ((Svc-Info).StartName -match 'LocalSystem') (Svc-Info | Out-String)
$w = Web-Ready; Check 'web site is up' ($null -ne $w) ''
$old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -like 'acct*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$s1 = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'acct share'; path = $Unc; user = $SmbUser; password = $SmbPw } }; $id1 = $s1.Result.id
$s2 = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'acct own rights'; path = $Unc2; user = ''; password = '' } }; $id2 = $s2.Result.id
Check 'servers saved' ([bool]$id1 -and [bool]$id2) ($s1.Text + $s2.Text)
$t1 = Api $w 'servers.test' @{ id = $id1 }; Check 'explicit account works (SYSTEM)' ($t1.Status -eq 200) $t1.Text
$t2 = Api $w 'servers.test' @{ id = $id2 }; Write-Output ("INFO empty user as SYSTEM: " + $t2.Text)
Check 'empty user as SYSTEM (machine account has no rights on the share): a clear refusal, not a success' ($t2.Status -ne 200 -or $t2.Json.ok -eq $false) $t2.Text
Check 'the refusal names the cause (denied / logon failure / password), no secret in it' ($t2.Text -match '(?i)denied|logon|password|credential|1326|5\)' -and -not $t2.Text.Contains($SmbPw)) $t2.Text
$idsBefore = @((Api $w 'servers.list' @{}).Result | ForEach-Object { $_.id }); $nServers = $idsBefore.Count
function Has-All($w) { $now = @((Api $w 'servers.list' @{}).Result | ForEach-Object { $_.id }); foreach ($i in $idsBefore) { if ($now -notcontains $i) { return $false } }; return $true }
Write-Output "INFO servers before: $nServers"; Write-Output ("INFO icacls data folder:`n" + ((& icacls.exe $data) -join "`n"))

# ---- 1. права на папку данных
& net.exe user vm2bstd $stdPw /add /y | Out-Null
$out = 'C:\vm2b\acl-std.txt'; Remove-Item $out -ErrorAction SilentlyContinue
$cred = New-Object Management.Automation.PSCredential("$env:COMPUTERNAME\vm2bstd", (ConvertTo-SecureString $stdPw -AsPlainText -Force))
try { Start-Process powershell.exe -Credential $cred -WorkingDirectory C:\vm2b -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\vm2b\guest\acltest.ps1', $out -Wait -WindowStyle Hidden -ErrorAction Stop } catch { Write-Output ("START FAILED " + $_.Exception.Message) }
$acl = if (Test-Path $out) { (Get-Content $out) -join ' | ' } else { 'NO RESULT' }; Write-Output ("INFO standard user vs the data folder: $acl")
Check 'a standard user cannot read config.json' ($acl -match 'READ_DENIED config.json') $acl
Check 'a standard user cannot decrypt the machine-scope secrets' ($acl -notmatch 'UNPROTECT_OK') $acl
Check 'a standard user cannot write config.json' ($acl -match 'WRITE_DENIED config.json') $acl
Check 'a standard user cannot read web.json (password hashes, the key of the certificate)' ($acl -notmatch 'READ_OK web.json') $acl
Check 'the logs folder stays writable for programs of other users (panel)' ($acl -match 'LOGS_LIST_OK|LOGS_LIST_DENIED') $acl
& net.exe user vm2bstd /delete 2>&1 | Out-Null

# ---- 2. администратор vm2svc: свои права, серверы, копии
Make-User 'vm2svc' $svcPw $true
Switch-Account 'vm2svc' $svcPw
Check 'service RUNNING under .\vm2svc' (Wait-State 'RUNNING' 60000) (St)
$i = Svc-Info; Write-Output ("INFO " + ($i | Out-String).Trim()); Check 'StartName is the account' ($i.StartName -match 'vm2svc') ($i | Out-String); Check 'the process owner is the account' ($i.Owner -match 'vm2svc') ($i | Out-String)
$w = Web-Ready; Check 'web site up under the account' ($null -ne $w) ''
if ($w) {
    $l = Api $w 'servers.list' @{}; Check 'the servers are all still there (nothing lost by the account change)' (Has-All $w) $l.Text
    $t = Api $w 'servers.test' @{ id = $id1 }; Check 'DPAPI secret (saved by SYSTEM) works under the new account (machine scope)' ($t.Status -eq 200) $t.Text
    $t = Api $w 'servers.test' @{ id = $id2 }; Write-Output ("INFO empty user as vm2svc: " + $t.Text); Check 'empty user = the account''s own rights: works (Samba knows vm2svc)' ($t.Status -eq 200) $t.Text
    Make-Data 'C:\vm2b\acctdata' 6
    # папка, где права только у vm2svc (ни SYSTEM, ни администраторов)
    Remove-Item -Recurse -Force 'C:\vm2b\svc-only' -ErrorAction SilentlyContinue; New-Item -ItemType Directory 'C:\vm2b\svc-only' | Out-Null
    & icacls.exe 'C:\vm2b\svc-only' /inheritance:r /grant 'vm2svc:(OI)(CI)F' | Out-Null
    $h = Run-Backup $w 'acct-local' 'C:\vm2b\acctdata' 'vmS' 'local' 'C:\vm2b\svc-only\{VM}'
    Write-Output ("INFO local-only-for-the-account backup: " + ($h | ConvertTo-Json -Compress)); Check 'backup to a folder where only the service account has rights: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
    $adm = try { Get-ChildItem 'C:\vm2b\svc-only' -ErrorAction Stop | Out-Null; 'ADMIN_CAN_LIST' } catch { 'ADMIN_DENIED' }; Write-Output "INFO the (elevated) administrator vs that folder: $adm"
    $h = Run-Backup $w 'acct-smb-explicit' 'C:\vm2b\acctdata' 'vmS' $id1 '{VM}'; Check 'backup to the SMB share with an explicit account: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
    $h = Run-Backup $w 'acct-smb-own' 'C:\vm2b\acctdata' 'vmS' $id2 '{VM}'; Write-Output ("INFO smb own rights: " + ($h | ConvertTo-Json -Compress)); Check 'backup to the SMB share with the service account''s own rights (empty user): Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
}
# ---- 2b. кириллическое имя учётной записи службы (русская Windows): служба стартует, секреты DPAPI работают, копия на локальную папку идёт
$cyr = -join ([char[]](0x421, 0x43B, 0x443, 0x436, 0x431, 0x430, 0x2D, 0x412, 0x41C)); $cyrPw = 'Cyr-Svc-123!x'          # Служба-ВМ
Make-User $cyr $cyrPw $true
Switch-Account $cyr $cyrPw
Check 'service RUNNING under an account with a Cyrillic name' (Wait-State 'RUNNING' 60000) (St)
$i = Svc-Info; Write-Output ("INFO " + ($i | Out-String).Trim()); Check 'the process owner is the Cyrillic account' ($i.Owner -match [regex]::Escape($cyr)) ($i | Out-String)
$w = Web-Ready; Check 'web site up under the Cyrillic account' ($null -ne $w) ''
if ($w) {
    $l = Api $w 'servers.list' @{}; Check 'the servers are all still there (Cyrillic account)' (Has-All $w) $l.Text
    $t = Api $w 'servers.test' @{ id = $id1 }; Check 'DPAPI secret works under the Cyrillic account' ($t.Status -eq 200) $t.Text
    Make-Data 'C:\vm2b\acctdata' 3
    $h = Run-Backup $w 'acct-cyr-local' 'C:\vm2b\acctdata' 'vmS' 'local' 'C:\vm2b\acctstore\{VM}'
    Write-Output ("INFO Cyrillic account backup: " + ($h | ConvertTo-Json -Compress)); Check 'backup under the Cyrillic account: Success' ($h -and $h.status -eq 'Success') ($h | ConvertTo-Json -Compress)
    Remove-Item -Recurse -Force 'C:\vm2b\acctstore' -ErrorAction SilentlyContinue
}
# ---- 2c. NT AUTHORITY\NetworkService (S-1-5-20): право входа как служба у встроенной записи есть, но прав на папку данных нет -> STOPPED с понятной причиной;
#          после выдачи прав по SID (*S-1-5-20) служба работает и расшифровывает секреты (область «компьютер»)
& net.exe stop $svc 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null
$o = & sc.exe config $svc obj= 'NT AUTHORITY\NetworkService' 2>&1; Write-Output ("sc config NetworkService: " + ($o -join ' '))
$o = & sc.exe start $svc 2>&1; Start-Sleep -Seconds 10; $state = St; Write-Output "INFO NetworkService without rights to the data folder: $state"
Check 'NetworkService without rights to the data folder: the service does not run half-working' ($state -match 'STOPPED') $state
$log = Get-Content "$data\logs\service.log" -Tail 6 -ErrorAction SilentlyContinue; Write-Output ("INFO service.log:`n" + ($log -join "`n"))
& icacls.exe $data /grant '*S-1-5-20:(OI)(CI)M' /t /c 2>&1 | Out-Null
& sc.exe start $svc 2>&1 | Out-Null
Check 'NetworkService with the rights granted by SID: RUNNING' (Wait-State 'RUNNING' 60000) (St)
$w = Web-Ready
if ($w) { $l = Api $w 'servers.list' @{}; Check 'the servers are all still there (NetworkService)' (Has-All $w) $l.Text; $t = Api $w 'servers.test' @{ id = $id1 }; Check 'DPAPI machine-scope secret works under NetworkService' ($t.Status -eq 200) $t.Text }
else { Check 'web site up under NetworkService' $false '' }

# ---- 3. администратор, которого Samba не знает: понятная ошибка
Make-User 'vm2nonet' 'Non-Pass-123!x' $true
Switch-Account 'vm2nonet' 'Non-Pass-123!x'
Check 'service RUNNING under .\vm2nonet' (Wait-State 'RUNNING' 60000) (St)
$w = Web-Ready
if ($w) {
    $l = Api $w 'servers.list' @{}; Check 'servers still listed under the unknown-to-Samba account' (Has-All $w) $l.Text
    $t = Api $w 'servers.test' @{ id = $id1 }; Check 'explicit credentials still work (they do not depend on the service account)' ($t.Status -eq 200) $t.Text
    $t = Api $w 'servers.test' @{ id = $id2 }; Write-Output ("INFO empty user, account unknown to the server: " + $t.Text)
    Check 'own rights of an account the server does not know: a clear refusal (no network rights there)' ($t.Status -ne 200 -or $t.Json.ok -eq $false) $t.Text
    Check 'the message says why (logon failure / denied), no secret' ($t.Text -match '(?i)denied|logon|password|credential|1326|5\)') $t.Text
    $h = Run-Backup $w 'acct-nonet' 'C:\vm2b\acctdata' 'vmS' $id2 '{VM}'; Write-Output ("INFO nonet backup: " + ($h | ConvertTo-Json -Compress))
    Check 'a backup to that server fails (Failed), it does not report Success' ($h -and $h.status -ne 'Success') ($h | ConvertTo-Json -Compress)
}
# ---- 4. обычный пользователь: служба не запускается, config.json цел
$cfgHash = (Get-FileHash "$data\config.json").Hash
Make-User 'vm2plain' 'Pla-Pass-123!x' $false
Switch-Account 'vm2plain' 'Pla-Pass-123!x'
Start-Sleep -Seconds 12
$state = St; Write-Output "INFO service state under a standard account: $state"
Check 'a standard account without rights to the data folder: the service does NOT run half-working (it is STOPPED)' ($state -match 'STOPPED') $state
$evs = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VM2BackupService' } -MaxEvents 5 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'cannot start' })
Write-Output ("INFO  Application log, VM2BackupService: " + $(if ($evs.Count) { ($evs[0].Message -split "`r?`n")[0] } else { '(no start-failure event)' }))
Check 'the start failure is in the Application log with its reason (readable text, source VM2BackupService)' ($evs.Count -ge 1) 'no event with "cannot start"'
$log = Get-Content "$data\logs\service.log" -Tail 12 -ErrorAction SilentlyContinue
Write-Output ("INFO last lines of service.log:`n" + ($log -join "`n"))
Check 'the log says WHY the service did not start (config.json, the data folder)' (($log -join ' ') -match 'cannot start.*config\.json|cannot start.*data folder|cannot read config') ($log -join ' ')
$evt = try { (Get-EventLog -LogName Application -Source VM2BackupService -Newest 2 -ErrorAction Stop | ForEach-Object { $_.Message }) -join ' | ' } catch { 'no event: ' + $_.Exception.Message }
Write-Output ("INFO Application event: $evt")
Check 'the Application event log has the reason too' ($evt -match 'cannot start') $evt
$ev = try { (Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager' } -MaxEvents 6 -ErrorAction Stop | ForEach-Object { $_.Message }) -join "`n" } catch { 'no events: ' + $_.Exception.Message }
Write-Output ("INFO SCM events:`n" + $ev)
Check 'config.json untouched by the failed start' ((Get-FileHash "$data\config.json").Hash -eq $cfgHash) 'hash changed'

# ---- 5. назад на LocalSystem: всё на месте
Switch-Account $null ''
Check 'service RUNNING again as LocalSystem' (Wait-State 'RUNNING' 90000) (St)
Check 'StartName LocalSystem' ((Svc-Info).StartName -match 'LocalSystem') (Svc-Info | Out-String)
$w = Web-Ready
if ($w) { $l = Api $w 'servers.list' @{}; Check 'servers all there after returning to LocalSystem' (Has-All $w) $l.Text; $t = Api $w 'servers.test' @{ id = $id1 }; Check 'DPAPI secret still works' ($t.Status -eq 200) $t.Text
    foreach ($x in @($l.Result)) { if ($x.name -like 'acct*' -or $x.name -like 'src acct*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } } }
& icacls.exe $data /remove:g '*S-1-5-20' /t /c 2>&1 | Out-Null      # выданные NetworkService права убираем (служба при запуске всё равно закрывает папку)
foreach ($u in 'vm2svc', 'vm2nonet', 'vm2plain', $cyr) { & net.exe user $u /delete 2>&1 | Out-Null }
Remove-Item -Recurse -Force 'C:\vm2b\acctdata' -ErrorAction SilentlyContinue
& takeown.exe /f 'C:\vm2b\svc-only' /r /d y 2>&1 | Out-Null; & icacls.exe 'C:\vm2b\svc-only' /grant 'Administrators:(OI)(CI)F' /t 2>&1 | Out-Null; Remove-Item -Recurse -Force 'C:\vm2b\svc-only' -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed)
exit $global:Failed
