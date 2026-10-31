# leaks.ps1 [минуты=36]: утечки ресурсов службы и панели на НАСТОЯЩЕЙ Windows (под Wine GetGuiResources возвращает 0, там это не проверялось).
# Нагрузка по кругу (виток ~2 мин): две копии малой ВМ (задача), входы / выходы через сайт, чередование порта сайта 8443 <-> 8444, открытие и закрытие панели (12 раз за виток,
# всего не меньше 200), перезапуск службы на 1/3 и 2/3 срока. Каждые ~2 минуты замер: HandleCount, Threads, PrivateMemorySize64, WorkingSet64, GDI / USER (GetGuiResources) процесса службы
# и ДОЛГОЖИВУЩЕЙ панели (одно окно открыто всё время). Вывод: прирост после прогрева (4 минуты) в последнем отрезке жизни процесса. Пороги: дескрипторы службы < 50, память службы < 30 МБ;
# для долгоживущей панели: дескрипторы < 50, GDI < 20, USER < 20, память < 30 МБ. Таблица замеров: C:\vm2b\leaks.csv (копия в results\leaks.csv).
param([int]$Minutes = 36)
. C:\vm2b\guest\lib.ps1
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class Gui { [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr h, uint f); }' -ErrorAction SilentlyContinue
$csv = 'C:\vm2b\leaks.csv'; 'min,proc,pid,handles,threads,private_mb,ws_mb,gdi,user' | Set-Content $csv
$src = 'C:\vm2b\leaksrc'; $store = 'C:\vm2b\leakstore'
$B = "$env:ProgramFiles\VM2Backup"; if (-not (Test-Path "$B\VM2Backup.exe")) { $B = 'C:\Program Files\VM2Backup' }
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web "https://127.0.0.1:$script:Port"; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 1) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 1 }; return 'timeout' }
$script:Port = 8443
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if (-not $w) { exit 1 }
Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmL" | Out-Null
$rnd = New-Object Random 7; $buf = New-Object byte[] 200000; $rnd.NextBytes($buf); [IO.File]::WriteAllBytes("$src\vmL\disk-flat.vmdk", $buf); Set-Content "$src\vmL\vm.vmx" 'displayName = "leak"'
# Раунд 9: сначала задача, потом сервер: сервер, на который ссылается задача прошлого прогона, не удаляется («a server named "leak src" already exists» при сохранении, и tasks.save падал с «choose a server with virtual machines»)
foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -eq 'leak-task') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'leak src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'leak src'; path = $src } }
# Раунд 9: однажды tasks.save ответил «choose a server with virtual machines as the source» (список ВМ источника ещё не видел только что записанный файл), и нагрузка шла вхолостую:
# ждём, пока источник покажет ВМ, и при отказе сохранения не продолжаем.
for ($i = 0; $i -lt 60; $i++) { $vl = Api $w 'servers.vms' @{ id = $sv.Result.id }; if ($vl.Status -eq 200 -and @($vl.Result.vms).Count -ge 1) { break }; Start-Sleep -Milliseconds 500 }
$el = @{ source = @{ server = $sv.Result.id; vms = @('vmL') }; target = @{ server = 'local'; directory = "$store\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 2; segment_mib = 16 }
$tk = Api $w 'tasks.save' @{ task = @{ name = 'leak-task'; enabled = $true; elements = @($el) } }; Check 'tasks.save' ([bool]$tk.Result.id) ($tk.Text + ' | servers.save: ' + $sv.Text + ' | source: ' + $sv.Result.id)
if (-not $tk.Result.id) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
$taskId = $tk.Result.id
$t0 = Get-Date
function Sample($tag) {
    $min = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    foreach ($p in @(Get-Process -Name VM2BackupService -ErrorAction SilentlyContinue) + @(Get-Process -Id $script:panelPid -ErrorAction SilentlyContinue)) {
        if (-not $p) { continue }
        $p.Refresh(); $gdi = 0; $usr = 0; try { $gdi = [Gui]::GetGuiResources($p.Handle, 0); $usr = [Gui]::GetGuiResources($p.Handle, 1) } catch {}
        # InvariantCulture: на русской Windows -f и N1 дают десятичную ЗАПЯТУЮ, строки CSV разваливались на лишние столбцы (раунд 9: «VM2BackupService measured :: no rows»)
        $line = [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0},{1},{2},{3},{4},{5:F1},{6:F1},{7},{8}', $min, $p.ProcessName, $p.Id, $p.HandleCount, $p.Threads.Count, ($p.PrivateMemorySize64 / 1MB), ($p.WorkingSet64 / 1MB), $gdi, $usr)
        Add-Content $csv $line
    }
}
$script:panelPid = 0
$pp = Start-Process "$B\VM2Backup.exe" -PassThru; $script:panelPid = $pp.Id; Start-Sleep -Seconds 8
Check 'the long-lived panel window is open' ((Get-Process -Id $script:panelPid -ErrorAction SilentlyContinue) -ne $null) ''
$cycles = 0; $backups = 0; $restarts = 0; $iter = 0
function PanelCycle {
    $q = Start-Process "$B\VM2Backup.exe" -PassThru; $d = Get-Date
    while (-not $q.HasExited -and ((Get-Date) - $d).TotalSeconds -lt 4) { $q.Refresh(); if ($q.MainWindowHandle -ne 0 -and ((Get-Date) - $d).TotalSeconds -gt 1.5) { break }; Start-Sleep -Milliseconds 300 }
    if (-not $q.HasExited) { $null = $q.CloseMainWindow(); if (-not $q.WaitForExit(5000)) { $q.Kill(); $q.WaitForExit(3000) | Out-Null } }
    $script:cycles++
}
Sample 'start'
$next = (Get-Date).AddMinutes(2); $r1 = $false; $r2 = $false
while (((Get-Date) - $t0).TotalMinutes -lt $Minutes -or $cycles -lt 200) {
    $iter++
    for ($k = 0; $k -lt 2; $k++) { $r = Api $w 'tasks.run' @{ id = $taskId }; if ($r.Status -eq 200) { $st = Job-Wait $w $r.Result.job 120; $backups++ } }
    for ($k = 0; $k -lt 3; $k++) { $w2 = New-Web "https://127.0.0.1:$script:Port"; Web-Login $w2 | Out-Null; Api $w2 'servers.list' @{} | Out-Null; Api $w2 'history.list' @{ limit = 20 } | Out-Null; Api $w2 'state' @{} | Out-Null; Web-Call $w2 'POST' '/api/logout' @{} | Out-Null }
    $np = if ($script:Port -eq 8443) { 8444 } else { 8443 }
    $s = Api $w 'web.settings.set' @{ bind = @('127.0.0.1'); port = $np }; Start-Sleep -Seconds 3; $script:Port = $np; $w = Web-Ready
    for ($k = 0; $k -lt 12; $k++) { PanelCycle }
    $el = ((Get-Date) - $t0).TotalMinutes
    if (-not $r1 -and $el -gt $Minutes / 3) { $r1 = $true; if ($script:Port -ne 8443) { Api $w 'web.settings.set' @{ bind = @('127.0.0.1'); port = 8443 } | Out-Null; Start-Sleep 3; $script:Port = 8443 }; & net.exe stop VM2BackupService | Out-Null; & net.exe start VM2BackupService | Out-Null; $restarts++; $w = Web-Ready; Add-Content $csv "$([math]::Round($el,1)),RESTART,0,0,0,0,0,0,0" }
    if (-not $r2 -and $el -gt 2 * $Minutes / 3) { $r2 = $true; if ($script:Port -ne 8443) { Api $w 'web.settings.set' @{ bind = @('127.0.0.1'); port = 8443 } | Out-Null; Start-Sleep 3; $script:Port = 8443 }; & net.exe stop VM2BackupService | Out-Null; & net.exe start VM2BackupService | Out-Null; $restarts++; $w = Web-Ready; Add-Content $csv "$([math]::Round($el,1)),RESTART,0,0,0,0,0,0,0" }
    if ((Get-Date) -ge $next) { Sample 'loop'; $next = (Get-Date).AddMinutes(2) }
    if ($iter % 3 -eq 0) { Write-Output ("INFO iter {0} min {1:N1}: backups {2}, panel opens {3}, restarts {4}" -f $iter, $el, $backups, $cycles, $restarts) }
}
Sample 'end'
if ($script:Port -ne 8443) { $w = Web-Ready; Api $w 'web.settings.set' @{ bind = @('127.0.0.1'); port = 8443 } | Out-Null; Start-Sleep 3; $script:Port = 8443 }
Check "panel opened and closed at least 200 times ($cycles)" ($cycles -ge 200) "cycles=$cycles"
Check "at least 20 backups ran ($backups)" ($backups -ge 20) "backups=$backups"
Check 'the service restarted twice' ($restarts -ge 2) "restarts=$restarts"
# ---- разбор таблицы
Stop-Process -Id $script:panelPid -Force -ErrorAction SilentlyContinue
Copy-Item $csv "$Share\results\leaks.csv" -Force -ErrorAction SilentlyContinue
# защита от локали: каждая строка таблицы - ровно 9 столбцов (десятичная запятая в русской Windows ломала разбор: раунд 9)
$badRows = @(Get-Content $csv | Select-Object -Skip 1 | Where-Object { ($_ -split ',').Count -ne 9 }).Count
Check 'every row of the table has 9 columns (no locale decimal comma)' ($badRows -eq 0) "bad rows: $badRows"
$rows = Import-Csv $csv
function Seg($name) {    # последний отрезок жизни процесса (после последнего RESTART для службы), только замеры старше 4 минут от начала отрезка
    $all = @($rows); $start = 0; if ($name -eq 'VM2BackupService') { $idx = 0; for ($i = 0; $i -lt $all.Count; $i++) { if ($all[$i].proc -eq 'RESTART') { $idx = $i } }; $start = $idx }
    $r = @($all[$start..($all.Count - 1)] | Where-Object { $_.proc -eq $name }); if ($r.Count -eq 0) { return $null }
    $t0m = [double]$r[0].min; $warm = @($r | Where-Object { [double]$_.min -ge $t0m + 4 }); if ($warm.Count -lt 3) { $warm = $r }
    return @{ all = $r; warm = $warm }
}
function Judge($name, $limits) {
    $s = Seg $name; if (-not $s) { Check "$name measured" $false 'no rows'; return }
    $a = $s.warm[0]; $z = $s.warm[$s.warm.Count - 1]; $n = $s.warm.Count
    $dh = [int]$z.handles - [int]$a.handles; $dm = [double]$z.private_mb - [double]$a.private_mb; $dws = [double]$z.ws_mb - [double]$a.ws_mb; $dg = [int]$z.gdi - [int]$a.gdi; $du = [int]$z.user - [int]$a.user; $dt = [int]$z.threads - [int]$a.threads
    $inc = 0; for ($i = 1; $i -lt $n; $i++) { if ([int]$s.warm[$i].handles -gt [int]$s.warm[$i - 1].handles) { $inc++ } }
    $mono = ($n -ge 6 -and $inc -ge $n - 2)
    Write-Output ("INFO {0}: {1} samples after warm-up ({2}..{3} min): handles {4} -> {5} ({6:+0;-0;0}), threads {7:+0;-0;0}, private {8:N1} -> {9:N1} MB ({10:+0.0;-0.0;0.0}), working set {11:+0.0;-0.0;0.0} MB, GDI {12:+0;-0;0}, USER {13:+0;-0;0}; handle samples that grew: {14} of {15}" -f $name, $n, $a.min, $z.min, $a.handles, $z.handles, $dh, $dt, [double]$a.private_mb, [double]$z.private_mb, $dm, $dws, $dg, $du, $inc, ($n - 1))
    Check "$name handle growth < $($limits.h)" ($dh -lt $limits.h) "grew by $dh"
    Check "$name private memory growth < $($limits.m) MB" ($dm -lt $limits.m) ("grew by {0:N1} MB" -f $dm)
    if ($limits.g) { Check "$name GDI objects growth < $($limits.g)" ($dg -lt $limits.g) "grew by $dg"; Check "$name USER objects growth < $($limits.g)" ($du -lt $limits.g) "grew by $du" }
    Check "$name handles are not growing monotonically past half the limit" (-not ($mono -and $dh -ge $limits.h / 2)) "monotonic growth by $dh"
}
Judge 'VM2BackupService' @{ h = 50; m = 30 }
Judge 'VM2Backup' @{ h = 50; m = 30; g = 20 }
Remove-Item -Recurse -Force $src, $store -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
