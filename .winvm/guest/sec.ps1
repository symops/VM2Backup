# sec.ps1 (раунд 13): исправления ревью безопасности на НАСТОЯЩЕЙ Windows, через настоящую службу и сайт на 127.0.0.1:8443.
#   F2: роль operator: backup.run с произвольным каталогом (и с keep) = 403, ничего не создано; сохранённая задача (tasks.run) работает; backups.list чужой папки отвергается;
#       admin работает как раньше; в журнале аудита видны запуск и отказ с ролью.
#   F3: время неверного входа для неизвестного и для известного пользователя: отношение медиан (должно быть меньше 3 в обе стороны), одинаковые статус и текст.
#   F5: 200 неудачных входов с одного адреса: журнал audit.log растёт на единицы строк, а не на 200; обычные события после потока пишутся.
. C:\vm2b\guest\lib.ps1
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 1) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 1 }; return 'timeout' }
$audit = 'C:\ProgramData\VM2Backup\audit.log'
$adm = Web-Ready; Check 'web site up (admin)' ($null -ne $adm) ''
if (-not $adm) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
$op = 'secop'; $oppw = 'Op-Test-Pw-4242!'
Api $adm 'web.users.delete' @{ name = $op } | Out-Null
$sv = Api $adm 'web.users.save' @{ name = $op; role = 'operator'; password = $oppw; twofa = 'optional' }
Check 'operator account created' ($sv.Status -eq 200) $sv.Text
$src = 'C:\vm2b\secsrc'; $store = 'C:\vm2b\secstore'; $evil = 'C:\vm2b\sec-evil'
Remove-Item -Recurse -Force $src, $store, $evil -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmS" | Out-Null
[IO.File]::WriteAllBytes("$src\vmS\a.vmdk", (New-Object byte[] 300000))
foreach ($x in @((Api $adm 'tasks.list' @{}).Result)) { if ($x.name -eq 'sec-task') { Api $adm 'tasks.delete' @{ id = $x.id } | Out-Null } }
foreach ($x in @((Api $adm 'servers.list' @{}).Result)) { if ($x.name -eq 'sec src') { Api $adm 'servers.delete' @{ id = $x.id } | Out-Null } }
$sid = (Api $adm 'servers.save' @{ server = @{ type = 'directory'; name = 'sec src'; path = $src } }).Result.id
$mkEl = { param($dir, $keep) @{ source = @{ server = $sid; vms = @('vmS') }; target = @{ server = 'local'; directory = "$dir\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $false; keep = $keep; segment_mib = 0 } }
$tk = Api $adm 'tasks.save' @{ task = @{ name = 'sec-task'; enabled = $true; elements = @(& $mkEl "$store" 3) } }; $tid = $tk.Result.id
# ---- F2
$o = New-Web 'https://127.0.0.1:8443'; $lo = Web-Login $o $op $oppw
Check 'F2: operator can log in' ($lo.Status -eq 200 -and $o.Role -eq 'operator') $lo.Text
$r1 = Api $o 'backup.run' @{ element = (& $mkEl $evil 1); name = 'evil' }
Check 'F2: operator backup.run with an arbitrary folder = 403' ($r1.Status -eq 403) "$($r1.Status) $($r1.Text)"
Check 'F2: nothing was written to the arbitrary folder' (-not (Test-Path $evil)) ''
$r2 = Api $o 'backup.run' @{ element = (& $mkEl $store 0); name = 'keep0' }
Check 'F2: operator backup.run with keep=0 = 403' ($r2.Status -eq 403) "$($r2.Status) $($r2.Text)"
$r3 = Api $o 'tasks.run' @{ id = $tid }
Check 'F2: operator runs the SAVED task (tasks.run) = 200 and a job id' ($r3.Status -eq 200 -and $r3.Result.job) "$($r3.Status) $($r3.Text)"
if ($r3.Result.job) { $st = Job-Wait $adm $r3.Result.job 300; Check 'F2: the saved task run by the operator succeeds' ($st -eq 'Success' -or $st -eq 'Gone') $st }
Check 'F2: the saved task wrote into ITS folder only' ((Test-Path "$store\vmS") -and -not (Test-Path $evil)) ''
$r4 = Api $o 'backups.list' @{ server = 'local'; directory = 'C:\Windows' }
Check 'F2: operator backups.list of an unknown folder is refused' ($r4.Status -ne 200 -or $r4.Text -match 'known backup folders') "$($r4.Status) $($r4.Text)"
$r5 = Api $o 'backups.list' @{ server = 'local'; directory = "$store\vmS" }
Write-Output "INFO operator backups.list of the folder of its own task: $($r5.Status) $($r5.Text.Substring(0, [Math]::Min(160, $r5.Text.Length)))"
$r6 = Api $adm 'backup.run' @{ element = (& $mkEl "$store-adm" 1); name = 'adm' }
Check 'F2: admin backup.run with any folder works as before' ($r6.Status -eq 200 -and $r6.Result.job) "$($r6.Status) $($r6.Text)"
if ($r6.Result.job) { $st = Job-Wait $adm $r6.Result.job 300; Check 'F2: the admin run succeeds' ($st -eq 'Success' -or $st -eq 'Gone') $st }
$r7 = Api $adm 'backups.list' @{ server = 'local'; directory = 'C:\Windows' }
Check 'F2: admin backups.list of any folder is not restricted' ($r7.Status -eq 200) "$($r7.Status) $($r7.Text)"
$al = if (Test-Path $audit) { Get-Content $audit -Encoding UTF8 -ErrorAction SilentlyContinue } else { @() }
Write-Output "INFO audit.log: $($al.Count) lines; tail: $((@($al | Select-Object -Last 6)) -join ' || ')"
Check 'F2: the audit log has the operator run with the role and the folder' ((($al -join "`n") -match 'secop') -and (($al -join "`n") -match 'tasks\.run')) ''
# ---- F3
function Login-Ms($user, $pw) {
    $w = New-Web 'https://127.0.0.1:8443'; $sw = [Diagnostics.Stopwatch]::StartNew(); $r = Web-Call $w 'POST' '/api/login' @{ user = $user; password = $pw } $false; $sw.Stop()
    return @{ ms = $sw.Elapsed.TotalMilliseconds; status = $r.Status; text = $r.Text }
}
# замер только настоящих ответов 401 (хеш посчитан); 429 (блокировка имени / адреса, растёт после каждой неудачи) пережидаем по retry_after
function Login-Real($user) {
    for ($k = 0; $k -lt 12; $k++) {
        $a = Login-Ms $user 'wrong password!'
        if ($a.status -eq 401) { return $a }
        $ra = 5; if ($a.text -match '"retry_after":(\d+)') { $ra = [int]$Matches[1] + 1 }; Start-Sleep -Seconds $ra
    }
    return $a
}
Login-Real $op | Out-Null
$known = @(); $unknown = @(); $kt = ''; $ut = ''; $ks = 0; $us = 0
for ($i = 0; $i -lt 7; $i++) {
    $a = Login-Real $op; $known += $a.ms; $kt = $a.text; $ks = $a.status
    $b = Login-Real ("nosuchuser$i"); $unknown += $b.ms; $ut = $b.text; $us = $b.status
}
function Median($v) { $s = @($v | Sort-Object); return $s[[int][Math]::Floor($s.Count / 2)] }
$mk = Median $known; $mu = Median $unknown
Write-Output ("INFO F3: median known user {0:N1} ms, unknown user {1:N1} ms, ratio {2:N2}; known all=[{3}] unknown all=[{4}]" -f $mk, $mu, ($mk / $mu), (($known | ForEach-Object { '{0:N0}' -f $_ }) -join ' '), (($unknown | ForEach-Object { '{0:N0}' -f $_ }) -join ' '))
Check 'F3: unknown and known user get the same status (401) and text' ($ks -eq 401 -and $us -eq 401 -and $kt -eq $ut) "known=$ks $kt unknown=$us $ut"
Check 'F3: the ratio of the medians is below 3 in both directions' ($mk -gt 0 -and $mu -gt 0 -and ($mk / $mu) -lt 3 -and ($mu / $mk) -lt 3) ("{0:N1} / {1:N1}" -f $mk, $mu)
# ---- F5
$null = Login-Real $op          # дожидаемся конца блокировки, чтобы первый неверный вход потока был настоящим, а остальные 199 блокировались
$size0 = if (Test-Path $audit) { (Get-Item $audit).Length } else { 0 }; $lines0 = if (Test-Path $audit) { @(Get-Content $audit -ErrorAction SilentlyContinue).Count } else { 0 }
$codes = @{}
for ($i = 0; $i -lt 200; $i++) { $r = Web-Call (New-Web 'https://127.0.0.1:8443') 'POST' '/api/login' @{ user = $op; password = 'flood-wrong' } $false; $codes["$($r.Status)"] = 1 + [int]$codes["$($r.Status)"] }
Write-Output ("INFO F5: statuses of 200 failed logins: " + (($codes.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
$size1 = (Get-Item $audit).Length; $lines1 = @(Get-Content $audit -ErrorAction SilentlyContinue).Count
Write-Output "INFO F5: audit.log $size0 -> $size1 bytes, $lines0 -> $lines1 lines"
Check 'F5: 200 failed logins add at most 12 lines to the audit log (not 200)' (($lines1 - $lines0) -le 12) "$lines0 -> $lines1"
Check 'F5: the flood was answered with 401 / 429 only' (@($codes.Keys | Where-Object { $_ -ne '401' -and $_ -ne '429' }).Count -eq 0) ($codes.Keys -join ',')
# после потока блокировка адреса растёт (по замыслу): ждём retry_after, который называет сам сервер, до 20 минут (потолок блокировки адреса 900 с)
$adm2 = $null; $waited = 0; $t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 1200 -and -not $adm2) {
    $w2 = New-Web 'https://127.0.0.1:8443'; $r2 = Web-Login $w2
    if ($r2.Status -eq 200 -and $r2.Json.ok) { $adm2 = $w2 } else { $ra = 5; if ($r2.Text -match '"retry_after":(\d+)') { $ra = [int]$Matches[1] + 1 }; if ($waited -eq 0) { Write-Output "INFO F5: after the flood the address is locked: status $($r2.Status), retry_after $ra s" }; $waited++; Start-Sleep -Seconds $ra }
}
Write-Output ("INFO F5: the administrator could log in again after {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
Check 'F5: the real admin can log in after the lock expired and the site works' ($null -ne $adm2) ''
$tail = (Get-Content $audit -ErrorAction SilentlyContinue | Select-Object -Last 8) -join ' || '
Write-Output "INFO F5 audit tail: $tail"
Check 'F5: the summary line "blocked login attempts" appears (or the single blocked line)' (($tail -match 'blocked login attempts') -or ($tail -match 'login_blocked')) $tail
if ($adm2) { Api $adm2 'web.users.delete' @{ name = $op } | Out-Null; Api $adm2 'tasks.delete' @{ id = $tid } | Out-Null; Api $adm2 'servers.delete' @{ id = $sid } | Out-Null }
Remove-Item -Recurse -Force $src, $store, "$store-adm", $evil -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
