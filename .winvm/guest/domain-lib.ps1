# domain-lib.ps1: общие константы и функции тестового домена AD (docs/WINDOWS-TESTS.md, "Домен AD"). Подключается точкой после lib.ps1.
# Домен - одноразовый тестовый стенд (две ВМ Server 2022, удаляются после прогона): пароли здесь - константы стенда, секретов нет.
$global:Dom = @{
    Name = 'vm2b.test'; Netbios = 'VM2B'; DcIp = '10.77.0.1'; MemberIp = '10.77.0.2'
    AdminUser = ('VM2B\' + (-join ([char[]](0x410,0x434,0x43C,0x438,0x43D,0x438,0x441,0x442,0x440,0x430,0x442,0x43E,0x440))));   # на русской Windows встроенная запись называется «Администратор»
    AdminPw = 'Adm1n-Vm2b#2026'; DsrmPw = 'Dsrm-Vm2b#2026'
    Svc = 'svcbackup';  SvcPw = 'Svc-Back#2026a'
    Bk = 'bkuser';      BkPw = 'Bk-User#2026a';  BkWrongPw = 'Wr0ng-Pa55#not'
    No = 'nouser';      NoPw = 'No-User#2026a'
    Lock = 'lockuser';  LockPw = 'Lock-User#2026a'
    MustChange = 'chguser'; MustChangePw = 'Chg-User#2026a'
    Expired = 'expuser';    ExpiredPw = 'Exp-User#2026a'
    Disabled = 'disuser';   DisabledPw = 'Dis-User#2026a'
    Group = 'VM2B-Backup-Operators'; Gmsa = 'vm2bgmsa'
    OU = 'OU=VM2B-Test,DC=vm2b,DC=test'
}
# Имя встроенного администратора (SID -500) на этой машине: «Администратор» на русской Windows; в домене то же имя носит доменный администратор.
function Get-BuiltinAdminName { $a = Get-WmiObject Win32_UserAccount -Filter "LocalAccount=True" | Where-Object { $_.SID -like 'S-1-5-21-*-500' } | Select-Object -First 1; if ($a) { return $a.Name }; return 'Administrator' }
# Автовход Winlogon: $domain - '.' (локальная запись) или NetBIOS-имя домена
function Set-AutoLogon([string]$domain, [string]$name, [string]$pw) {
    $wl = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
    Set-ItemProperty $wl AutoAdminLogon '1'; Set-ItemProperty $wl DefaultUserName $name; Set-ItemProperty $wl DefaultDomainName $domain; Set-ItemProperty $wl DefaultPassword $pw
    Remove-ItemProperty $wl AutoLogonCount -ErrorAction SilentlyContinue
    Write-Output ("INFO autologon set: $domain\$name")
}
function Dom-AdminCred { New-Object Management.Automation.PSCredential($global:Dom.AdminUser, (ConvertTo-SecureString $global:Dom.AdminPw -AsPlainText -Force)) }
# сервис и данные продукта
$global:SvcName = 'VM2BackupService'; $global:DataDir = "$env:ProgramData\VM2Backup"
function St { $s = (& sc.exe query $global:SvcName | Select-String '\b(STOPPED|START_PENDING|STOP_PENDING|RUNNING)\b' | Select-Object -First 1); if ($s) { ($s.ToString() -replace '\s+', ' ').Trim() } else { 'none' } }
function Wait-State($want, $ms) { $t0 = Get-Date; while (((Get-Date) - $t0).TotalMilliseconds -lt $ms) { if ((St) -match $want) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function Svc-Info { $o = Get-WmiObject Win32_Service -Filter "Name='$global:SvcName'"; $own = ''; if ($o -and $o.ProcessId) { $p = Get-WmiObject Win32_Process -Filter "ProcessId=$($o.ProcessId)"; if ($p) { $r = $p.GetOwner(); $own = "$($r.Domain)\$($r.User)" } }; return @{ StartName = $o.StartName; Owner = $own } }
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
# право «Вход в качестве службы» по SID (secedit): на русской Windows имена групп и прав локализованы, SID - нет
function Grant-LogonRight([string]$account) {
    $sid = (New-Object Security.Principal.NTAccount($account)).Translate([Security.Principal.SecurityIdentifier]).Value
    & secedit.exe /export /cfg C:\vm2b\sec-in.inf /areas USER_RIGHTS | Out-Null
    $t = Get-Content C:\vm2b\sec-in.inf -Encoding Unicode
    $has = $false; $t = $t | ForEach-Object { if ($_ -match '^SeServiceLogonRight\s*=') { $has = $true; if ($_ -notmatch [regex]::Escape($sid)) { $_ + ",*$sid" } else { $_ } } else { $_ } }
    if (-not $has) { $t = $t | ForEach-Object { $_; if ($_ -match '^\[Privilege Rights\]') { "SeServiceLogonRight = *$sid" } } }
    $t | Set-Content C:\vm2b\sec-out.inf -Encoding Unicode
    $o = (& secedit.exe /configure /db C:\vm2b\sec.sdb /cfg C:\vm2b\sec-out.inf /areas USER_RIGHTS 2>&1) -join ' '
    Write-Output ("INFO secedit (SeServiceLogonRight $account = $sid): " + $o.Substring(0, [Math]::Min(120, $o.Length)))
}
# Смена учётной записи службы: $user = '' -> LocalSystem; $pw = $null -> параметр password= не передаётся, '' -> пустой (gMSA)
function Switch-Account([string]$user, $pw) {
    & net.exe stop $global:SvcName 2>&1 | Out-Null; Wait-State 'STOPPED' 60000 | Out-Null
    if ($user) { if ($pw -eq $null) { $o = & sc.exe config $global:SvcName obj= $user 2>&1 } elseif ($pw -eq '') { $o = & cmd.exe /c ('sc.exe config ' + $global:SvcName + ' obj= "' + $user + '" password= ""') 2>&1 } else { $o = & sc.exe config $global:SvcName obj= $user password= $pw 2>&1 } }      # пустой пароль gMSA: PowerShell выбрасывает пустой аргумент, поэтому через cmd
    else { $o = & sc.exe config $global:SvcName obj= LocalSystem 2>&1 }
    Write-Output ("INFO sc config obj=" + $(if ($user) { $user } else { 'LocalSystem' }) + ": " + (($o -join ' ') -replace '\s+', ' '))
    $o2 = & sc.exe start $global:SvcName 2>&1; Write-Output ("INFO sc start: " + (($o2 -join ' ') -replace '\s+', ' '))
}
function Run-Backup($w, $name, $srcDir, $vmName, $targetServer, $targetDir) {
    Api $w 'scheduler.set' @{ enabled = $true } | Out-Null
    $old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq "src $name") { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = "src $name"; path = $srcDir } }; $srcId = $sv.Result.id
    $el = @{ source = @{ server = $srcId; vms = @($vmName) }; target = @{ server = $targetServer; directory = $targetDir }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 0; segment_mib = 4 }
    $tk = Api $w 'tasks.save' @{ task = @{ name = $name; enabled = $true; elements = @($el) } }; $tid = $tk.Result.id
    $r = Api $w 'tasks.run' @{ id = $tid }
    $res = $null
    for ($i = 0; $i -lt 180; $i++) { Start-Sleep -Seconds 2; $h = Api $w 'history.list' @{ limit = 50 }; $hit = @($h.Result) | Where-Object { $_.name -eq $name } | Select-Object -First 1; if ($hit) { $res = $hit; break } }
    Api $w 'tasks.delete' @{ id = $tid } | Out-Null
    return $res
}
function Make-Data($dir, $mb) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$dir\vmS" | Out-Null; $rnd = New-Object Random 7; $buf = New-Object byte[] (1MB); $fs = [IO.File]::Create("$dir\vmS\disk.vmdk"); for ($i = 0; $i -lt $mb; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close() }
function Wait-Dns([string]$name, [int]$sec = 300) { $t0 = Get-Date; while (((Get-Date) - $t0).TotalSeconds -lt $sec) { try { if (Resolve-DnsName $name -ErrorAction Stop) { return $true } } catch {}; Start-Sleep -Seconds 5 }; return $false }
# --- gMSA: состояние пароля (msDS-ManagedPasswordId, pwdLastSet) с контроллера под учётной записью администратора домена
function Get-GmsaState {
    try { $o = Get-ADServiceAccount $global:Dom.Gmsa -Properties 'msDS-ManagedPasswordId', pwdLastSet, 'msDS-ManagedPasswordInterval' -Server dc.vm2b.test -Credential (Dom-AdminCred) -ErrorAction Stop
          $id = $o.'msDS-ManagedPasswordId'; $hex = if ($id) { [BitConverter]::ToString([byte[]]$id) } else { '' }
          return @{ Id = $hex; PwdLastSet = [Int64]$o.pwdLastSet; Interval = $o.'msDS-ManagedPasswordInterval'; Dn = $o.DistinguishedName; Error = '' } }
    catch { return @{ Id = ''; PwdLastSet = 0; Interval = 0; Dn = ''; Error = $_.Exception.Message } }
}
function Gmsa-Line($s) { if ($s.Error) { return 'error: ' + $s.Error } else { return ('id=' + $s.Id.Substring(0, [Math]::Min(100, $s.Id.Length)) + ' pwdLastSet=' + $s.PwdLastSet + ' interval=' + $s.Interval) } }
# одна копия через службу с пустым пользователем на \\dc.vm2b.test\Backups; результат: 'Success' или текст сбоя (запись истории)
function Backup-Once($w, [string]$tag, [string]$srv) {
    $h = Run-Backup $w ($tag + '-' + (Get-Date).ToString('HHmmss')) 'C:\vm2b\domdata' 'vmS' $srv '{VM}'
    if (-not $h) { return 'no history record' }
    if ($h.status -eq 'Success') { return 'Success' }
    $msg = ''; try { $a = @{ id = $h.id }; $r = Api $w 'history.get' $a; $msg = ($r.Result.results | ForEach-Object { $_.message }) -join ' | ' } catch {}
    return ('status=' + $h.status + ' ' + $msg)
}
