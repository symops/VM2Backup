# tzreal.ps1 <prep|scenario|restore>: НАСТОЯЩИЕ часы гостя и настоящая служба через переход на летнее время (w32time отключён, время ставится Set-Date, дальше идёт само).
#   prep                                          запомнить пояс и время (якорь для возврата), остановить и отключить w32time
#   scenario <зона> <дата-время местное> <ежедневно ЧЧ:ММ> <минут наблюдения> <метка>
#                                                 пояс (tzutil /s), время (Set-Date), две задачи (ежедневно в ЧЧ:ММ, каждые 30 минут от 00:00), планировщик включён;
#                                                 наблюдение за историей запусков (местное время каждого запуска), проверки: ежедневная задача ровно один раз, ни одного
#                                                 повтора с тем же местным ЧЧ:ММ, интервал между запусками «каждые 30 минут» не больше 70 минут, всего не больше 6 запусков (нет цикла)
#   restore                                       пояс и время обратно (якорь + прошедшее по секундомеру), w32time снова автоматический + повторная синхронизация
param([string]$Mode, [string]$Zone, [string]$Local, [string]$Daily, [int]$Minutes = 18, [string]$Label = 'tz')
. C:\vm2b\guest\lib.ps1
$anchorFile = 'C:\vm2b\tz-anchor.txt'
# Часы ставятся напрямую через SetSystemTime (UTC). Set-Date не годится: он трактует время в поясе, который .NET прочитал при запуске процесса (после tzutil /s он устарел).
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class Clk { [StructLayout(LayoutKind.Sequential)] public struct ST { public ushort y, mo, dow, d, h, mi, s, ms; } [DllImport("kernel32.dll", SetLastError = true)] public static extern bool SetSystemTime(ref ST t); }' -ErrorAction SilentlyContinue
function Set-UtcClock([DateTime]$utc) { $t = New-Object Clk+ST; $t.y = $utc.Year; $t.mo = $utc.Month; $t.d = $utc.Day; $t.h = $utc.Hour; $t.mi = $utc.Minute; $t.s = $utc.Second; $t.ms = $utc.Millisecond; return [Clk]::SetSystemTime([ref]$t) }
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Clear-TzTasks($w) { foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -like 'tz-*') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }; foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'tz src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } } }
if ($Mode -eq 'prep') {
    $tz = (& tzutil.exe /g).Trim(); $now = [DateTime]::UtcNow
    "$tz|$($now.Ticks)|$([Diagnostics.Stopwatch]::GetTimestamp())|$([Diagnostics.Stopwatch]::Frequency)" | Set-Content $anchorFile
    Write-Output "INFO original zone: $tz; UTC $($now.ToString('s'))"
    # Windows возвращает время с узла сам: без этого часы откатывались через 1-3 минуты (событие Kernel-General 1: процесс svchost.exe службы w32time). Поэтому: служба выключена и остановлена,
    # синхронизация NoSync, задачи «Time Synchronization» выключены.
    & sc.exe config w32time start= disabled | Out-Null; & sc.exe stop w32time | Out-Null
    & reg.exe add 'HKLM\SYSTEM\CurrentControlSet\Services\W32Time\Parameters' /v Type /t REG_SZ /d NoSync /f | Out-Null
    foreach ($t in 'SynchronizeTime', 'ForceSynchronizeTime', 'SynchronizeTimeZone') { & schtasks.exe /change /tn "\Microsoft\Windows\Time Synchronization\$t" /disable 2>&1 | Out-Null }
    # Раунд 9 (Windows 10 x64 ru на dockur, у ВМ есть интернет): через ~45 с после смены часов Windows САМА возвращала w32time «отключена -> Автоматически» (Service Control
    # Manager 7040), служба тут же брала время с time.windows.com, и сценарий видел «часы тронул кто-то другой». Поэтому дополнительно закрыт исходящий UDP 123 (правило
    # брандмауэра vm2b-block-ntp; снимается в restore): даже запущенная служба времени не получает источник.
    & netsh.exe advfirewall firewall delete rule name=vm2b-block-ntp 2>&1 | Out-Null
    & netsh.exe advfirewall firewall add rule name=vm2b-block-ntp dir=out action=block protocol=UDP remoteport=123 2>&1 | Out-Null
    Start-Sleep -Seconds 3; & taskkill.exe /f /fi "SERVICES eq w32time" 2>&1 | Out-Null
    $ws = Get-CimInstance Win32_Service -Filter "Name='w32time'"; Write-Output ("INFO w32time: " + $ws.State + " / " + $ws.StartMode)
    Check 'w32time stopped and disabled' ($ws.State -eq 'Stopped' -and $ws.StartMode -eq 'Disabled') "$($ws.State) / $($ws.StartMode)"
    $uptimeAnchor = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime; Write-Output "INFO boot time $uptimeAnchor"
}
elseif ($Mode -eq 'restore') {
    $a = (Get-Content $anchorFile).Split('|'); $elapsed = ([Diagnostics.Stopwatch]::GetTimestamp() - [int64]$a[2]) / [double]$a[3]
    & tzutil.exe /s $a[0] | Out-Null
    & netsh.exe advfirewall firewall delete rule name=vm2b-block-ntp 2>&1 | Out-Null
    $target = ([DateTime]::new([int64]$a[1], 'Utc')).AddSeconds($elapsed)
    $okc = Set-UtcClock $target
    & sc.exe config w32time start= delayed-auto | Out-Null; & reg.exe add 'HKLM\SYSTEM\CurrentControlSet\Services\W32Time\Parameters' /v Type /t REG_SZ /d NTP /f | Out-Null
    foreach ($t in 'SynchronizeTime', 'ForceSynchronizeTime', 'SynchronizeTimeZone') { & schtasks.exe /change /tn "\Microsoft\Windows\Time Synchronization\$t" /enable 2>&1 | Out-Null }
    Start-Service w32time -ErrorAction SilentlyContinue
    & w32tm.exe /resync /force 2>&1 | Out-Null
    Write-Output ("INFO restored zone " + (& tzutil.exe /g).Trim() + ", UTC now " + [DateTime]::UtcNow.ToString('s') + ", planned " + $target.ToString('s') + ", w32time " + (Get-Service w32time).Status)
    Check 'zone restored' ((& tzutil.exe /g).Trim() -eq $a[0]) ''
    Check 'clock within 2 minutes of the planned value' ([math]::Abs(([DateTime]::UtcNow - $target).TotalSeconds) -lt 120) ''
}
else {
    & sc.exe config w32time start= disabled | Out-Null; & sc.exe stop w32time 2>&1 | Out-Null
    $w = Web-Ready; Check 'web site up' ($null -ne $w) ''; if (-not $w) { exit 1 }
    Api $w 'scheduler.set' @{ enabled = $false } | Out-Null; Clear-TzTasks $w; Api $w 'history.clear' @{} | Out-Null
    & tzutil.exe /s $Zone | Out-Null; Check "zone set: $Zone" ((& tzutil.exe /g).Trim() -eq $Zone) (& tzutil.exe /g)
    [TimeZoneInfo]::ClearCachedData()
    $dt = [DateTime]::ParseExact($Local, 'yyyy-MM-dd HH:mm:ss', $null); $tzi = [TimeZoneInfo]::FindSystemTimeZoneById($Zone)
    if ($tzi.IsInvalidTime($dt)) { throw "local time $Local does not exist in $Zone" }
    if ($tzi.IsAmbiguousTime($dt)) { $off = @($tzi.GetAmbiguousTimeOffsets($dt) | Sort-Object -Descending)[0]; $utcSet = [DateTime]::SpecifyKind($dt - $off, 'Utc'); Write-Output "INFO the local time $Local happens twice in ${Zone}: the FIRST pass is taken (offset $off)" }      # повторяющееся время: берём первый проход (летнее время)
    else { $utcSet = [TimeZoneInfo]::ConvertTimeToUtc($dt, $tzi) }
    # Раунд 11: опорная точка «часы должны идти вровень с секундомером» берётся В МОМЕНТ SetSystemTime, а не при старте наблюдения. В раунде 10 (Windows 11, spring-eu) опорой служило
    # установленное время, а секундомер запускался после настройки (сон, вход в веб-сайт, сервер, задачи): настройка заняла 122 с (из них 56 с остановки сразу после смены часов),
    # и первая же проверка называла эти секунды «часы ушли на 115 с», хотя часы шли ровно (через 18 минут «now» отличался от секундомера на 1 с).
    $okc = Set-UtcClock $utcSet; $swSet = [Diagnostics.Stopwatch]::StartNew(); Check "system clock set (SetSystemTime to $($utcSet.ToString('s')) UTC)" $okc
    function Clock-Off { return ([DateTime]::UtcNow - $utcSet).TotalSeconds - $swSet.Elapsed.TotalSeconds }     # на сколько часы ушли от секундомера (секунды); 0 = идут вровень
    $off0 = Clock-Off; Start-Sleep -Seconds 10; $off1 = Clock-Off
    Check "${Label}: the clock keeps pace with the stopwatch right after it was set (10 s)" ([math]::Abs($off1 - $off0) -lt 1.5) ("{0:N2} s then {1:N2} s" -f $off0, $off1)
    Write-Output ("INFO {0}: zone {1}, local time set to {2}, now {3}, UTC {4}" -f $Label, $Zone, $Local, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), [DateTime]::UtcNow.ToString('s'))
    Start-Sleep -Seconds 4; $w = Web-Ready
    $src = 'C:\vm2b\tzsrc'; $store = 'C:\vm2b\tzstore'; Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmT" | Out-Null
    [IO.File]::WriteAllBytes("$src\vmT\disk-flat.vmdk", (New-Object byte[] 4000)); Set-Content "$src\vmT\vm.vmx" 'displayName = "tz"'
    $sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'tz src'; path = $src } }
    $el = @{ source = @{ server = $sv.Result.id; vms = @('vmT') }; target = @{ server = 'local'; directory = "$store\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $false; keep = 3; segment_mib = 16 }
    $day = $dt.ToString('yyyy-MM-dd')
    $t1 = Api $w 'tasks.save' @{ task = @{ name = 'tz-daily'; enabled = $true; elements = @($el); schedule = @{ enabled = $true; mode = 'weekly'; start_time = $Daily; weekdays = @(0, 1, 2, 3, 4, 5, 6); week_interval = 1 } } }
    $t2 = Api $w 'tasks.save' @{ task = @{ name = 'tz-every30'; enabled = $true; elements = @($el); schedule = @{ enabled = $true; mode = 'every'; every = @{ value = 30; unit = 'minutes' }; first_time = "${day}T00:00" } } }
    Check 'two scheduled tasks saved' ([bool]$t1.Result.id -and [bool]$t2.Result.id) "$($t1.Text) $($t2.Text)"
    foreach ($id in @($t1.Result.id, $t2.Result.id)) { $n = Api $w 'tasks.next' @{ id = $id }; Write-Output ("INFO tasks.next ($id): " + $n.Text) }
    Api $w 'scheduler.set' @{ enabled = $true } | Out-Null
    $epoch0 = [DateTimeOffset]::new($utcSet.ToUniversalTime()).ToUnixTimeSeconds() - 30; $seen = @{}; $runs = @(); $sw = [Diagnostics.Stopwatch]::StartNew(); $drift = 0; $maxDev = 0.0
    Write-Output ("INFO setup took {0:N0} s (clock set -> start of the observation); the clock is {1:N1} s off the stopwatch" -f $swSet.Elapsed.TotalSeconds, (Clock-Off))
    while ($sw.Elapsed.TotalMinutes -lt $Minutes) {
        # отклонение считается от опоры в момент установки часов и проверяется на КАЖДОМ обороте (раньше - один раз); допуск 20 с (w32time / узел сдвигали бы часы на минуты)
        $dev = [math]::Abs((Clock-Off) - $off0); if ($dev -gt $maxDev) { $maxDev = $dev }
        if ($dev -gt 20 -and -not $drift) { $drift = 1; Write-Output ("INFO the clock was moved by something else by {0:N0} s (UTC now {1}, since it was set {2:N0} s); w32time: {3}" -f $dev, [DateTime]::UtcNow.ToString('s'), $swSet.Elapsed.TotalSeconds, (Get-Service w32time).Status) }
        $h = Api $w 'history.list' @{ limit = 100 }
        foreach ($e in @($h.Result)) { if ($e.name -like 'tz-*' -and [int64]$e.started -ge $epoch0 -and -not $seen.ContainsKey($e.id)) { $seen[$e.id] = 1
            $st = [DateTimeOffset]::FromUnixTimeSeconds([int64]$e.started); $loc = [TimeZoneInfo]::ConvertTime($st, [TimeZoneInfo]::Local)
            $runs += [pscustomobject]@{ name = $e.name; utc = $st.UtcDateTime.ToString('HH:mm:ss'); local = $loc.ToString('yyyy-MM-dd HH:mm:ss'); hhmm = $loc.ToString('HH:mm'); unix = [int64]$e.started; status = $e.status }
            Write-Output ("INFO RUN {0}: started {1} local ({2} UTC), {3}" -f $e.name, $loc.ToString('yyyy-MM-dd HH:mm:ss'), $st.UtcDateTime.ToString('HH:mm:ss'), $e.status) } }
        Start-Sleep -Seconds 8
    }
    Write-Output ("INFO now {0} local, zone {1}, {2} runs in {3} min" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), (& tzutil.exe /g).Trim(), $runs.Count, $Minutes)
    Check "${Label}: the clock was not moved by anything else during the observation" ($drift -eq 0) ("largest deviation {0:N1} s, see the INFO line above" -f $maxDev)
    Write-Output ("INFO {0}: largest deviation of the clock from the stopwatch during the observation: {1:N2} s" -f $Label, $maxDev)
    Api $w 'scheduler.set' @{ enabled = $false } | Out-Null
    $d = @($runs | Where-Object name -eq 'tz-daily'); $e = @($runs | Where-Object name -eq 'tz-every30')
    Check "${Label}: the daily task ran exactly once ($($d.Count))" ($d.Count -eq 1) (($d | ForEach-Object { $_.local }) -join '; ')
    foreach ($grp in @($d, $e)) { $dup = @($grp | Group-Object hhmm | Where-Object Count -gt 1); Check "${Label}: no run repeated with the same local HH:MM ($($grp[0].name))" ($dup.Count -eq 0) (($dup | ForEach-Object { $_.Name }) -join ',') }
    $ok = $true; for ($i = 1; $i -lt $e.Count; $i++) { if (($e[$i].unix - $e[$i - 1].unix) -gt 70 * 60) { $ok = $false } }
    Check "${Label}: 'every 30 minutes' never waits more than 70 real minutes ($($e.Count) runs)" $ok ''
    Check "${Label}: 'every 30 minutes' ran at least once" ($e.Count -ge 1) ''
    Check "${Label}: at most 6 runs in all (no loop)" ($runs.Count -le 6) "runs=$($runs.Count)"
    $bad = @($runs | Where-Object { $_.status -ne 'Success' }); Check "${Label}: every run is Success" ($bad.Count -eq 0) (($bad | ForEach-Object { $_.name + ':' + $_.status }) -join ',')
    Clear-TzTasks $w; Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue
}
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
