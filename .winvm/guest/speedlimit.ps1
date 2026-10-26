# speedlimit.ps1: ограничение скорости на НАСТОЯЩИХ данных на госте (раунд 2, русская Windows 10 x64). Мерим время и скорость записи копии на цель:
#   без лимита, с лимитом элемента задачи (speed_mbit), с глобальной настройкой (settings.set speed_limit_mbit), с окном расписания (windows[].mbit),
#   и с двумя лимитами сразу (действует меньший). 8 Мбит/с = 1 МБ/с; данные несжимаемые, xz уровня 1, проверка выключена (иначе читалась бы ещё раз).
# История: в первом раунде speed_mbit в записи элемента НЕ действовал при запуске всей задачи (limit_mbit оставался 0): ошибка продукта (svccore.cpp, backupElement).
. C:\vm2b\guest\lib.ps1
$src = 'C:\vm2b\spddata'; $store = 'C:\vm2b\spdstore'; $mb = 12
function Web-Ready { for ($i = 0; $i -lt 90; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if (-not $w) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmL" | Out-Null
$rnd = New-Object Random 5; $buf = New-Object byte[] (1MB); $fs = [IO.File]::Create("$src\vmL\disk-flat.vmdk"); for ($i = 0; $i -lt $mb; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close()
$old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq 'speed src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'speed src'; path = $src } }; $srvId = $sv.Result.id
Api $w 'settings.set' @{ speed_limit_mbit = 0 } | Out-Null
function Run-Case($name, $elMbit, $windowMbit) {
    $dir = "$store\$name"; Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    $el = @{ source = @{ server = $srvId; vms = @('vmL') }; target = @{ server = 'local'; directory = "$dir\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $false; keep = 0; segment_mib = 64; speed_mbit = $elMbit }
    $task = @{ name = "spd-$name"; enabled = $true; elements = @($el) }
    if ($windowMbit -gt 0) { $task.windows = @(@{ days = @(0, 1, 2, 3, 4, 5, 6); from = '00:00'; to = '23:59'; mbit = $windowMbit }) }
    $tk = Api $w 'tasks.save' @{ task = $task }
    if (-not $tk.Result.id) { Write-Output ("tasks.save: " + $tk.Text); return $null }
    $sw = [Diagnostics.Stopwatch]::StartNew(); $r = Api $w 'tasks.run' @{ id = $tk.Result.id }
    $seen = 0.0; $hit = $null
    for ($i = 0; $i -lt 400 -and -not $hit; $i++) {
        $j = Api $w 'jobs.list' @{}; foreach ($x in @($j.Result)) { if ($x.name -eq "spd-$name" -and ($x.status -eq 'Running' -or $x.status -eq 'Waiting') -and [double]$x.limit_mbit -gt $seen) { $seen = [double]$x.limit_mbit } }
        $h = Api $w 'history.list' @{ limit = 50 }; $hit = @($h.Result) | Where-Object { $_.name -eq "spd-$name" } | Select-Object -First 1
        if (-not $hit) { Start-Sleep -Milliseconds 400 }
    }
    $sec = $sw.Elapsed.TotalSeconds
    $bytes = (Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '\.(json|manifest)$' } | Measure-Object Length -Sum).Sum
    Api $w 'tasks.delete' @{ id = $tk.Result.id } | Out-Null
    $o = [pscustomobject]@{ Name = $name; Sec = $sec; Bytes = [double]$bytes; MBps = [Math]::Round(([double]$bytes / 1MB) / [Math]::Max($sec, 0.1), 2); LimitSeen = $seen; Status = $(if ($hit) { $hit.status } else { 'timeout' }) }
    Write-Host ("INFO case {0}: {1:N1} s, {2:N1} MiB written, {3} MiB/s, limit_mbit seen {4}, status {5}" -f $name, $o.Sec, ($o.Bytes / 1MB), $o.MBps, $o.LimitSeen, $o.Status)
    return $o
}
$free = Run-Case 'free' 0 0
Check 'without a limit the task succeeds and no limit is shown' ($free -and $free.Status -eq 'Success' -and $free.LimitSeen -eq 0) ($free | Out-String)
$el = Run-Case 'element8' 8 0
Check 'element speed_mbit=8: the job shows limit_mbit 8' ($el -and $el.LimitSeen -gt 7.5 -and $el.LimitSeen -lt 8.5) ($el | Out-String)
Check 'element speed_mbit=8: 12 MiB of incompressible data take at least 9 s (1 MB/s)' ($el -and $el.Status -eq 'Success' -and $el.Sec -ge 9) ($el | Out-String)
Check 'element speed_mbit=8: the measured write speed does not exceed ~1.5 MiB/s' ($el -and $el.MBps -le 1.5) ($el | Out-String)
Check 'the limited run is clearly slower than the free run (at least 2 times)' ($free -and $el -and $el.Sec -ge 2 * $free.Sec) ("free=$($free.Sec) limited=$($el.Sec)")
Api $w 'settings.set' @{ speed_limit_mbit = 8 } | Out-Null
$gl = Run-Case 'global8' 0 0
Check 'global speed_limit_mbit=8 (settings.set): the job shows limit_mbit 8 and is slow' ($gl -and $gl.LimitSeen -gt 7.5 -and $gl.LimitSeen -lt 8.5 -and $gl.Sec -ge 9 -and $gl.MBps -le 1.5) ($gl | Out-String)
Api $w 'settings.set' @{ speed_limit_mbit = 64 } | Out-Null
$both = Run-Case 'both' 8 0
Check 'global 64 + element 8: the lower one (8) applies' ($both -and $both.LimitSeen -gt 7.5 -and $both.LimitSeen -lt 8.5 -and $both.Sec -ge 9) ($both | Out-String)
Api $w 'settings.set' @{ speed_limit_mbit = 0 } | Out-Null
$win = Run-Case 'window8' 0 8
Check 'a schedule window with mbit=8 limits a manual run of the task as well' ($win -and $win.LimitSeen -gt 7.5 -and $win.LimitSeen -lt 8.5 -and $win.Sec -ge 9) ($win | Out-String)
$fast = Run-Case 'free2' 0 0
Check 'after the limits are removed the next run is fast again' ($fast -and $fast.LimitSeen -eq 0 -and $fast.Sec -lt $el.Sec / 1.5) ("fast=$($fast.Sec) limited=$($el.Sec)")
Api $w 'servers.delete' @{ id = $srvId } | Out-Null
Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
