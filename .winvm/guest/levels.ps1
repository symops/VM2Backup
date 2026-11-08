# levels.ps1 (раунд 13): уровни сжатия xz 0-9 и потоки на 32-битной Windows: время, размер архива, ПИК рабочего набора и ПИК выделенной памяти (commit)
# процесса службы (PeakWorkingSet64 / PeakPagedMemorySize64 берутся у свежего процесса: перед каждым запуском служба перезапускается, поэтому пики не смешиваются),
# затем verify и restore этого архива с побайтовой (SHA-1) сверкой с источником.
#   levels.ps1 matrix [МБ_текста] [МБ_случайных]   - уровни 0-9 (потоки Auto), затем 7/8/9 с потоками 1, 2, 4, 8; данные: текст + реальные исполняемые файлы System32 + случайные
#   levels.ps1 big <ГБ>                              - одна большая ВМ-папка: уровень 7 по умолчанию, Auto и потоки 1..4
#   levels.ps1 foreign <папка tools/make_x64_archive.py>  - прочитать (verify + restore) архив, сделанный 64-битной службой на хосте, и сверить файлы
param([string]$Mode = 'matrix', [string]$A1 = '', [string]$A2 = '')
. C:\vm2b\guest\lib.ps1
$src = 'C:\vm2b\lvldata'; $store = 'C:\vm2b\lvlstore'; $rest = 'C:\vm2b\lvlrest'
$svcExe = (Get-WmiObject Win32_Service -Filter "Name='VM2BackupService'").PathName; if ($svcExe -match '^"([^"]+)"') { $svcExe = $Matches[1] } else { $svcExe = ($svcExe -split ' /')[0].Trim() }
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 1) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 1 }; return 'timeout' }
Add-Type -TypeDefinition @"
using System; using System.IO; using System.Text; using System.Security.Cryptography; using System.Collections.Generic;
public static class LvlGen {
    // журнал-подобный текст: словарь из 3000 слов, строки с метками времени (сжимается в 5-8 раз, как настоящие журналы)
    public static void Text(string path, long bytes, int seed) {
        Random r = new Random(seed); string[] v = new string[3000];
        for (int i = 0; i < v.Length; i++) { int n = 3 + r.Next(9); char[] c = new char[n]; for (int k = 0; k < n; k++) c[k] = (char)('a' + r.Next(26)); v[i] = new string(c); }
        using (StreamWriter w = new StreamWriter(path, false, new UTF8Encoding(false), 1 << 20)) {
            long done = 0; int t = 0;
            while (done < bytes) { StringBuilder sb = new StringBuilder(); sb.Append("2026-11-08 ").Append((t / 3600 % 24).ToString("00")).Append(':').Append((t / 60 % 60).ToString("00")).Append(':').Append((t % 60).ToString("00")).Append(" INFO ").Append(v[r.Next(300)]).Append('[').Append(r.Next(65536).ToString("x")).Append("] ");
                int words = 6 + r.Next(14); for (int k = 0; k < words; k++) sb.Append(v[r.Next(v.Length)]).Append(' '); sb.Append('\n'); string s = sb.ToString(); w.Write(s); done += s.Length; if (r.Next(3) == 0) t++; }
        }
    }
    public static void Random(string path, long bytes, int seed) {
        Random r = new Random(seed); byte[] b = new byte[1 << 20];
        using (FileStream f = File.Create(path, 1 << 20)) { long done = 0; while (done < bytes) { r.NextBytes(b); int n = (int)Math.Min(b.Length, bytes - done); f.Write(b, 0, n); done += n; } }
    }
    public static string TreeHash(string root) {
        List<string> files = new List<string>(Directory.GetFiles(root, "*", SearchOption.AllDirectories)); files.Sort(StringComparer.OrdinalIgnoreCase);
        SHA1 all = SHA1.Create(); StringBuilder sb = new StringBuilder(); byte[] buf = new byte[1 << 20];
        foreach (string p in files) {
            using (SHA1 h = SHA1.Create()) using (FileStream f = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 1 << 20)) {
                int n; while ((n = f.Read(buf, 0, buf.Length)) > 0) h.TransformBlock(buf, 0, n, null, 0); h.TransformFinalBlock(buf, 0, 0);
                sb.Append(p.Substring(root.Length).ToLowerInvariant()).Append('=').Append(BitConverter.ToString(h.Hash)).Append('\n'); }
        }
        return BitConverter.ToString(all.ComputeHash(Encoding.UTF8.GetBytes(sb.ToString()))).Replace("-", "") + " files=" + files.Count;
    }
}
"@
function Mk-Data([string]$dir, [int]$textMb, [int]$binMb, [int]$rndMb) {
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$dir\logs", "$dir\bin", "$dir\rnd" | Out-Null
    $n = 0; $left = [long]$textMb * 1MB; while ($left -gt 0) { $sz = [Math]::Min($left, 64MB); [LvlGen]::Text("$dir\logs\app$n.log", $sz, 100 + $n); $left -= $sz; $n++ }
    $left = [long]$binMb * 1MB; $n = 0
    foreach ($f in (Get-ChildItem "$env:SystemRoot\System32" -File -Include *.dll, *.exe -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.Length -gt 200KB } | Sort-Object Length -Descending)) {
        if ($left -le 0) { break }; try { Copy-Item $f.FullName "$dir\bin\$($f.Name)" -ErrorAction Stop; $left -= $f.Length } catch {} }
    # второй проход по тем же файлам под другими именами (повторы далеко друг от друга, как у ВМ-диска), пока не набрано
    $n = 0; foreach ($f in (Get-ChildItem "$dir\bin" -File | Sort-Object Name)) { if ($left -le 0) { break }; Copy-Item $f.FullName "$dir\bin\copy-$n-$($f.Name)"; $left -= $f.Length; $n++ }
    $n = 0; $left = [long]$rndMb * 1MB; while ($left -gt 0) { $sz = [Math]::Min($left, 64MB); [LvlGen]::Random("$dir\rnd\r$n.bin", $sz, 500 + $n); $left -= $sz; $n++ }
}
function Svc-Restart { Stop-Service VM2BackupService -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2; Start-Service VM2BackupService; return (Web-Ready) }
function Svc-Peaks { $p = Get-Process VM2BackupService -ErrorAction SilentlyContinue; if (-not $p) { return $null }; return [pscustomobject]@{ Pid = $p.Id; WS = [Math]::Round($p.PeakWorkingSet64 / 1MB); Commit = [Math]::Round($p.PeakPagedMemorySize64 / 1MB); Virt = [Math]::Round($p.PeakVirtualMemorySize64 / 1MB) } }
$free = [Math]::Round((Get-PSDrive C).Free / 1GB, 1); Write-Output "INFO free space C: $free GB; RAM: $([Math]::Round((Get-WmiObject Win32_ComputerSystem).TotalPhysicalMemory / 1MB)) MiB; CPUs: $((Get-WmiObject Win32_ComputerSystem).NumberOfLogicalProcessors); OS arch: $((Get-WmiObject Win32_OperatingSystem).OSArchitecture)"
# 32-битный образ, флаг LARGE_ADDRESS_AWARE: сколько адресного пространства получает процесс
$bytes = [IO.File]::ReadAllBytes($svcExe); $pe = [BitConverter]::ToInt32($bytes, 0x3C); $mach = [BitConverter]::ToUInt16($bytes, $pe + 4); $chr = [BitConverter]::ToUInt16($bytes, $pe + 22)
Write-Output ("INFO {0}: machine 0x{1:x} (0x14c = x86), characteristics 0x{2:x}, LARGE_ADDRESS_AWARE={3}" -f $svcExe, $mach, $chr, (($chr -band 0x20) -ne 0))
try { Write-Output ("INFO bcdedit increaseuserva: " + ((bcdedit /enum '{current}') | Select-String 'increaseuserva' | Out-String).Trim()) } catch {}
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if (-not $w) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'lvl src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }

function Run-Level($vm, $level, $threads, $srcRoot, $srcHash, $label, [switch]$BackupOnly) {
    $dir = "$store\$vm-L$level-T$threads"; Remove-Item -Recurse -Force $dir, $rest -ErrorAction SilentlyContinue
    $w = Svc-Restart; if (-not $w) { Check "$label : service restarted" $false ''; return }
    Api $w 'settings.set' @{ level = $level; threads = $threads; max_parallel = 1 } | Out-Null
    foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'lvl src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -like 'lvl-*') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'lvl src'; path = $srcRoot } }; $sid = $sv.Result.id
    $el = @{ source = @{ server = $sid; vms = @($vm) }; target = @{ server = 'local'; directory = "$dir\{VM}" }; compress = @{ codec = 'xz' }; encrypt = @{ enabled = $false }; verify = $false; keep = 0; segment_mib = 0 }
    $tk = Api $w 'tasks.save' @{ task = @{ name = "lvl-$level-$threads"; enabled = $true; elements = @($el) } }
    $svcPid = (Get-Process VM2BackupService).Id
    $sw = [Diagnostics.Stopwatch]::StartNew(); $r = Api $w 'tasks.run' @{ id = $tk.Result.id }
    if (-not $r.Result.job) { Write-Output "INFO tasks.run answer: $($r.Status) $($r.Text)" }
    $st = Job-Wait $w $r.Result.job 5400; $sec = $sw.Elapsed.TotalSeconds
    $pk = Svc-Peaks; $alive = ($pk -and $pk.Pid -eq $svcPid)
    $h = Api $w 'history.list' @{ limit = 20 }; $he = @($h.Result) | Where-Object { $_.name -eq "lvl-$level-$threads" } | Select-Object -First 1
    $arch = (Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '\.(json|manifest)$' } | Measure-Object Length -Sum).Sum
    $hs = if ($he) { $he.status } else { $st }
    $row = "level=$level threads=$threads backup_s={0:N1} archive_MiB={1:N0} peakWS_MiB={2} peakCommit_MiB={3} peakVirt_MiB={4} status=$hs service_same_process=$alive" -f $sec, ($arch / 1MB), $pk.WS, $pk.Commit, $pk.Virt
    Write-Output "INFO LEVELROW $label $row"
    Check "$label : backup succeeds, the service process did not die" ($hs -eq 'Success' -and $alive) $row
    Api $w 'tasks.delete' @{ id = $tk.Result.id } | Out-Null
    if ($hs -ne 'Success') { return }
    if ($BackupOnly) { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; return }
    $bl = Api $w 'backups.list' @{ server = 'local'; directory = "$dir\$vm" }; $b = @($bl.Result.backups)[0]
    if (-not $b) { Check "$label : the archive is listed" $false $bl.Text; return }
    # verify: ещё раз с перезапуском службы (пик чтения)
    $w = Svc-Restart; $sw.Restart(); $vr = Api $w 'backups.verify' @{ server = 'local'; directory = $b.directory; archive = $b.archive }; $vs = Job-Wait $w $vr.Result.job 3600; $vsec = $sw.Elapsed.TotalSeconds; $pv = Svc-Peaks
    Write-Output ("INFO LEVELROW {0} verify_s={1:N1} verify_peakWS_MiB={2} verify_peakCommit_MiB={3} status={4}" -f $label, $vsec, $pv.WS, $pv.Commit, $vs)
    Check "$label : backups.verify" ($vs -eq 'Success' -or $vs -eq 'Gone') $vs
    $w = Svc-Restart; $sw.Restart(); $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b.directory; archive = $b.archive; dest = $rest }; $rs = Job-Wait $w $rr.Result.job 3600; $rsec = $sw.Elapsed.TotalSeconds; $pr = Svc-Peaks
    Write-Output ("INFO LEVELROW {0} restore_s={1:N1} restore_peakWS_MiB={2} restore_peakCommit_MiB={3} status={4}" -f $label, $rsec, $pr.WS, $pr.Commit, $rs)
    Check "$label : backups.restore" ($rs -eq 'Success' -or $rs -eq 'Gone') $rs
    $rh = ''; if (Test-Path "$rest\$vm") { $rh = [LvlGen]::TreeHash("$rest\$vm") }
    Check "$label : the restored tree is byte-identical to the source" ($rh -eq $srcHash) "restored=$rh source=$srcHash"
    Remove-Item -Recurse -Force $dir, $rest -ErrorAction SilentlyContinue
}

if ($Mode -eq 'matrix') {
    $textMb = if ($A1) { [int]$A1 } else { 500 }; $rndMb = if ($A2) { [int]$A2 } else { 300 }
    Mk-Data "$src\vmM" $textMb 500 $rndMb
    $tot = (Get-ChildItem "$src\vmM" -Recurse -File | Measure-Object Length -Sum).Sum; $hash = [LvlGen]::TreeHash("$src\vmM")
    Write-Output ("INFO source: {0:N0} MiB, {1}" -f ($tot / 1MB), $hash)
    foreach ($lv in 0..9) { Run-Level 'vmM' $lv 0 $src $hash "matrix L$lv Auto" }
    foreach ($lv in 7, 8, 9) { foreach ($t in 1, 2, 4, 8) { Run-Level 'vmM' $lv $t $src $hash "matrix L$lv T$t" } }
}
elseif ($Mode -eq 'list') {
    # отдельные пары «уровень:потоки» (A1 = '7:8 7:4 9:8'), те же данные, что в matrix; сначала самые опасные для памяти
    $textMb = 500; $rndMb = 300
    Mk-Data "$src\vmM" $textMb 500 $rndMb
    $hash = [LvlGen]::TreeHash("$src\vmM"); Write-Output ("INFO source hash: $hash")
    foreach ($pair in ($A1 -split ' ')) { $lv, $t = $pair -split ':'; Run-Level 'vmM' ([int]$lv) ([int]$t) $src $hash "list L$lv T$t" }
}
elseif ($Mode -eq 'big') {
    $gb = if ($A1) { [int]$A1 } else { 10 }
    # смесь: 40 % текст, 25 % исполняемые (повторяются), 35 % случайные; число копий набирает объём
    Remove-Item -Recurse -Force $src -ErrorAction SilentlyContinue; Mk-Data "$src\vmB" ([int]($gb * 1024 * 0.40)) ([int]($gb * 1024 * 0.25)) ([int]($gb * 1024 * 0.35))
    $tot = (Get-ChildItem "$src\vmB" -Recurse -File | Measure-Object Length -Sum).Sum; $hash = [LvlGen]::TreeHash("$src\vmB")
    Write-Output ("INFO source: {0:N0} MiB, {1}" -f ($tot / 1MB), $hash)
    Run-Level 'vmB' 7 0 $src $hash 'big L7 Auto (default)'
    foreach ($t in 4, 2, 1) { Run-Level 'vmB' 7 $t $src $hash "big L7 T$t" -BackupOnly }       # остальные потоки: только копия (время, размер, пики); verify / restore сделаны на Auto
}
elseif ($Mode -eq 'foreign') {
    # архив 64-битной службы (tools/make_x64_archive.py, папка $A1 на общей папке хоста: store\vmX + files.sha1); копируем на локальный диск и читаем 32-битной службой
    $loc = 'C:\vm2b\x64arch'; Remove-Item -Recurse -Force $loc, $rest -ErrorAction SilentlyContinue; Copy-Item -Recurse $A1 $loc
    $w = Svc-Restart
    $bl = Api $w 'backups.list' @{ server = 'local'; directory = "$loc\store\vmX" }; $b = @($bl.Result.backups)[0]
    Check 'foreign (x64) archive is listed by the 32-bit service' ($null -ne $b) $bl.Text
    if ($b) {
        Write-Output ("INFO foreign archive entry: " + ($b | ConvertTo-Json -Compress -Depth 4))
        $vr = Api $w 'backups.verify' @{ server = 'local'; directory = $b.directory; archive = $b.archive }; $vs = Job-Wait $w $vr.Result.job 3600
        Check 'foreign (x64) archive: backups.verify on 32 bits' ($vs -eq 'Success' -or $vs -eq 'Gone') $vs
        $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b.directory; archive = $b.archive; dest = $rest }; $rs = Job-Wait $w $rr.Result.job 3600
        Check 'foreign (x64) archive: backups.restore on 32 bits' ($rs -eq 'Success' -or $rs -eq 'Gone') $rs
        $pk = Svc-Peaks; Write-Output ("INFO foreign archive peaks: WS {0} MiB, commit {1} MiB" -f $pk.WS, $pk.Commit)
        $vmdir = Get-ChildItem $rest -Directory | Select-Object -First 1; $bad = 0; $n = 0
        foreach ($line in (Get-Content "$loc\files.sha1")) { $f = $line -split ' '; $p = Join-Path $vmdir.FullName $f[0]; $n++
            $h = if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA1).Hash.ToLower() } else { 'missing' }; if ($h -ne $f[1]) { $bad++; Write-Output "INFO differs: $($f[0]) $h vs $($f[1])" } }
        Check "foreign archive: all $n restored files equal the sources made on the host ($bad differ)" ($n -gt 0 -and $bad -eq 0) ''
    }
    Remove-Item -Recurse -Force $loc, $rest -ErrorAction SilentlyContinue
}
$w = Web-Ready; if ($w) { Api $w 'settings.set' @{ level = 7; threads = 0 } | Out-Null; foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'lvl src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } } }
Remove-Item -Recurse -Force $src, $store, $rest -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
