# ntfsav.ps1: NTFS и антивирус. Источник ВМ с «неудобными» файлами, настоящая служба (SYSTEM) копирует их через сайт API, восстановление и сравнение.
#   vmEFS  : обычный файл + файл, зашифрованный EFS (cipher /e под пользователем задания; служба работает под SYSTEM)
#   vmNT   : NTFS-сжатый файл (compact /c) + разреженный файл (логический размер 256 МБ, данных ~2 МБ)
#   vmLONG : файл с именем 255 символов + каталог с 5000 файлов
#   vmAV   : файл EICAR (тестовая строка антивируса; Defender в реальном времени удаляет его; защита Defender не отключается)
#   vmGONE : большая копия с ограничением скорости; ПОСРЕДИ копии удаляется ещё не прочитанный файл (файл исчез по ходу) и создаётся EICAR
param([string]$Part = 'all')      # all | av (только EICAR и «файл исчез по ходу»: фаза eicar, Defender на это время включён)
. C:\vm2b\guest\lib.ps1
$base = 'C:\vm2b\ntfsav'; $src = "$base\src"; $store = "$base\store"; $rest = "$base\rest"
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 1) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 1 }; return 'timeout' }
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''; if (-not $w) { exit 1 }
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue; foreach ($d in 'vmEFS', 'vmNT', 'vmLONG', 'vmAV', 'vmGONE') { New-Item -ItemType Directory -Force "$src\$d" | Out-Null }
$rnd = New-Object Random 42
function Rand-File($path, $bytes) { $buf = New-Object byte[] $bytes; $rnd.NextBytes($buf); [IO.File]::WriteAllBytes($path, $buf) }
$t0 = Get-Date; $mark = $t0.AddSeconds(-5)
# ---- данные
Rand-File "$src\vmEFS\plain.vmdk" 300000; Set-Content "$src\vmEFS\vm.vmx" 'displayName = "efs"'
Rand-File "$src\vmEFS\enc.vmdk" 400000
$ce = & cipher.exe /e /a "$src\vmEFS\enc.vmdk" 2>&1 | Out-String; Write-Output ("INFO cipher /e: " + ($ce -replace "`r?`n", ' | ').Trim())
$encAttr = (Get-Item "$src\vmEFS\enc.vmdk").Attributes; Write-Output "INFO enc.vmdk attributes: $encAttr; running as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)"
$efsOn = ($encAttr -band [IO.FileAttributes]::Encrypted) -ne 0
Write-Output "INFO EFS encryption applied: $efsOn"
$compText = (1..20000 | ForEach-Object { "line $_ of a very compressible text`r`n" }) -join ''; [IO.File]::WriteAllText("$src\vmNT\comp.vmdk", $compText)
$cc = & compact.exe /c "$src\vmNT\comp.vmdk" 2>&1 | Out-String; Write-Output ("INFO compact /c: " + (($cc -split "`r?`n" | Where-Object { $_ -match 'comp|сжат' }) -join ' | '))
$compAttr = (Get-Item "$src\vmNT\comp.vmdk").Attributes; Write-Output "INFO comp.vmdk attributes: $compAttr (compressed: $((($compAttr -band [IO.FileAttributes]::Compressed) -ne 0))); size on disk via compact: $(($cc -split "`r?`n" | Where-Object { $_ -match '[0-9]' } | Select-Object -First 2) -join ' | ')"
Set-Content "$src\vmNT\vm.vmx" 'displayName = "nt"'
$sp = "$src\vmNT\sparse.vmdk"; & fsutil.exe file createnew $sp 268435456 | Out-Null; & fsutil.exe sparse setflag $sp | Out-Null; & fsutil.exe sparse setrange $sp 0 268435456 | Out-Null
& fsutil.exe file setzerodata offset=0 length=268435456 $sp | Out-Null
$fs = [IO.File]::Open($sp, 'Open', 'ReadWrite'); $b = New-Object byte[] 1048576; $rnd.NextBytes($b); $fs.Position = 1048576; $fs.Write($b, 0, $b.Length); $fs.Position = 200MB; $fs.Write($b, 0, $b.Length); $fs.Close()
Write-Output ("INFO sparse.vmdk: " + ((& fsutil.exe sparse queryflag $sp) -join ' ') + "; logical size " + (Get-Item $sp).Length + "; " + ((& fsutil.exe file layout $sp 2>$null | Select-String -Pattern 'Allocated|Ранее|Выделен' | Select-Object -First 1) -join ''))
$sparseAlloc = (& fsutil.exe sparse queryrange $sp | Out-String) -replace "`r?`n", ' | '; Write-Output "INFO sparse ranges: $sparseAlloc"
$longName = ('L' * 251) + '.vmdk'; Check 'a 255-character name is made' ($longName.Length -eq 256 -or $longName.Length -eq 255) "len=$($longName.Length)"
$longName = ('L' * 250) + '.vmdk'     # ровно 255 символов
Rand-File "\\?\$src\vmLONG\$longName" 150000; Set-Content "$src\vmLONG\vm.vmx" 'displayName = "long"'
New-Item -ItemType Directory -Force "$src\vmLONG\many" | Out-Null
$sb = New-Object byte[] 256; for ($i = 1; $i -le 5000; $i++) { $rnd.NextBytes($sb); [IO.File]::WriteAllBytes("$src\vmLONG\many\f$('{0:D5}' -f $i).dat", $sb) }
Write-Output ("INFO long name length: $($longName.Length); files in many: " + @(Get-ChildItem "$src\vmLONG\many").Count)
Rand-File "$src\vmAV\good.vmdk" 200000; Set-Content "$src\vmAV\vm.vmx" 'displayName = "av"'
# EICAR: строка собирается из частей (чтобы сам скрипт не считался угрозой)
$eic = 'X5O!P%@AP[4\PZX54(P^)7CC)7}$' + 'EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*'
$eicErr = ''; try { [IO.File]::WriteAllText("$src\vmAV\eicar.com.txt", $eic) } catch { $eicErr = $_.Exception.Message }
Write-Output "INFO EICAR write: $(if ($eicErr) { 'refused by the system: ' + $eicErr } else { 'accepted' })"
Start-Sleep -Seconds 12
$eicLeft = Test-Path "$src\vmAV\eicar.com.txt"; Write-Output "INFO EICAR file still present after 12 s: $eicLeft"
try { $mp = Get-MpComputerStatus; Write-Output ("INFO Defender: RealTimeProtectionEnabled=$($mp.RealTimeProtectionEnabled) AMServiceEnabled=$($mp.AMServiceEnabled) signatures=$($mp.AntivirusSignatureVersion) updated=$($mp.AntivirusSignatureLastUpdated)") } catch { Write-Output "INFO Get-MpComputerStatus: $($_.Exception.Message)" }
Rand-File "$src\vmGONE\a-big.vmdk" 20000000; Rand-File "$src\vmGONE\m-mid.vmdk" 500000; Rand-File "$src\vmGONE\z-vanish.vmx" 3000
# ---- сервер, задачи
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'ntfsav src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'ntfsav src'; path = $src } }; $sid = $sv.Result.id
function Run-Vm($vm, $speed) {
    $el = @{ source = @{ server = $sid; vms = @($vm) }; target = @{ server = 'local'; directory = "$store\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 0; segment_mib = 16 }
    if ($speed) { $el.speed_mbit = $speed }
    foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -eq "na-$vm") { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
    $tk = Api $w 'tasks.save' @{ task = @{ name = "na-$vm"; enabled = $true; elements = @($el) } }
    $r = Api $w 'tasks.run' @{ id = $tk.Result.id }; return @{ task = $tk.Result.id; job = $r.Result.job; start = Get-Date }
}
function Hist($vm) { $h = Api $w 'history.list' @{ limit = 100 }; $e = @($h.Result) | Where-Object { $_.name -eq "na-$vm" } | Select-Object -First 1; if ($e) { $g = Api $w 'history.get' @{ id = $e.id }; if ($g.Result) { return $g.Result } }; return $e }
function Restore-Check($vm, $expectFiles) {     # восстановить последнюю копию ВМ и вернуть путь
    $bl = Api $w 'backups.list' @{ server = 'local'; directory = "$store\$vm" }; $b = @($bl.Result.backups)[0]; if (-not $b) { return $null }
    $dest = "$rest\$vm"; $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b.directory; archive = $b.archive; dest = $dest }
    $st = Job-Wait $w $rr.Result.job 900; Write-Host "INFO restore $vm -> $st"; return $dest
}
function Same($a, $b) { if (-not (Test-Path $b)) { return $false }; (Get-FileHash $a -Algorithm SHA256).Hash -eq (Get-FileHash $b -Algorithm SHA256).Hash }
if ($Part -eq 'all') {
# ---- vmEFS
$j = Run-Vm 'vmEFS' 0; $st = Job-Wait $w $j.job 300; $h = Hist 'vmEFS'; Write-Output ("INFO vmEFS history: " + ($h | ConvertTo-Json -Depth 6 -Compress))
Check "vmEFS: the job ended in time (no hang): $st" ($st -ne 'timeout') ''
if ($efsOn) {
    if ($h.status -eq 'Success') {
        $d = Restore-Check 'vmEFS' 2; $same = Same "$src\vmEFS\enc.vmdk" "$d\vmEFS\enc.vmdk"
        Check 'vmEFS: Success means the EFS file is restored with the same content' $same 'Success, but the restored EFS file differs or is missing'
    } else {
        $msg = ($h.results | ConvertTo-Json -Depth 6 -Compress)
        Check "vmEFS: the failure is clear (status $($h.status)) and names the file or the reason" ($msg -match 'enc\.vmdk' -or $msg -match 'ccess|Отказано|denied|encrypt|EFS') $msg
        Check 'vmEFS: not reported as Success' ($h.status -ne 'Success') ''
        Check 'vmEFS: the service did not crash' ((Get-Service VM2BackupService).Status -eq 'Running') ''
    }
} else { Write-Output 'INFO vmEFS: EFS is not available on this SKU/volume: the encrypted-file case is not applicable' }
Check 'vmEFS: the service is up afterwards' ((Get-Service VM2BackupService).Status -eq 'Running') ''
# ---- vmNT
$j = Run-Vm 'vmNT' 0; $st = Job-Wait $w $j.job 600; $h = Hist 'vmNT'; Write-Output ("INFO vmNT history status: $($h.status); raw_bytes $($h.raw_bytes), archive_bytes $($h.archive_bytes)")
Check "vmNT: compressed + sparse file copied (status $($h.status))" ($h.status -eq 'Success') ($h | ConvertTo-Json -Depth 6 -Compress)
$d = Restore-Check 'vmNT' 3
Check 'vmNT: the NTFS-compressed file is restored bit-identical' (Same "$src\vmNT\comp.vmdk" "$d\vmNT\comp.vmdk") ''
Check 'vmNT: the sparse file is restored bit-identical (256 MB logical)' (Same $sp "$d\vmNT\sparse.vmdk") ''
$rs = "$d\vmNT\sparse.vmdk"; if (Test-Path $rs) { $sparseBack = ((& fsutil.exe sparse queryflag $rs) -join ' '); Write-Output "INFO restored sparse file: $sparseBack; on disk: $((& fsutil.exe sparse queryrange $rs | Out-String) -replace "`r?`n", ' | ')" }
# ---- vmLONG
$j = Run-Vm 'vmLONG' 0; $st = Job-Wait $w $j.job 900; $h = Hist 'vmLONG'; Write-Output ("INFO vmLONG history status: $($h.status)")
Check "vmLONG: 255-character file name + 5000 files (status $($h.status))" ($h.status -eq 'Success') ($h | ConvertTo-Json -Depth 6 -Compress)
$d = Restore-Check 'vmLONG' 5001
$restoredCount = @(Get-ChildItem "$d\vmLONG\many" -ErrorAction SilentlyContinue).Count; Check "vmLONG: 5000 files restored ($restoredCount)" ($restoredCount -eq 5000) "count=$restoredCount"
$hSrc = (Get-ChildItem "$src\vmLONG\many" | Sort-Object Name | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ''; $hDst = (Get-ChildItem "$d\vmLONG\many" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ''
Check 'vmLONG: contents of the 5000 files identical' ($hSrc -eq $hDst) ''
$ln = @(Get-ChildItem -LiteralPath "\\?\$d\vmLONG" -Filter 'LLLL*' -ErrorAction SilentlyContinue)
Check 'vmLONG: the 255-character file is restored with its name and content' ($ln.Count -eq 1 -and $ln[0].Length -eq 150000 -and $ln[0].Name.Length -eq 255) "found=$($ln.Count)"
}
# ---- vmAV
$j = Run-Vm 'vmAV' 0; $st = Job-Wait $w $j.job 300; $h = Hist 'vmAV'; Write-Output ("INFO vmAV history: " + ($h | ConvertTo-Json -Depth 6 -Compress))
Check "vmAV: the job ended in time (no hang): $st" ($st -ne 'timeout') ''
if ($eicLeft) { Write-Output 'INFO vmAV: Defender did NOT remove the EICAR file (real-time protection off / no detection): the EICAR-as-source case is not applicable here' }
else {
    $d = Restore-Check 'vmAV' 2
    Check 'vmAV: the good file of the VM is backed up and restored bit-identical' (Same "$src\vmAV\good.vmdk" "$d\vmAV\good.vmdk") ''
    Check 'vmAV: the removed EICAR file is not in the copy' (-not (Test-Path "$d\vmAV\eicar.com.txt")) ''
}
# ---- vmGONE: файл исчезает по ходу
$j = Run-Vm 'vmGONE' 12; Start-Sleep -Seconds 6
$jl = Api $w 'jobs.list' @{}; $mine = @($jl.Result) | Where-Object { $_.id -eq $j.job }; Write-Output ("INFO vmGONE job in progress: file=$($mine.file) raw=$($mine.raw_bytes) status=$($mine.status)")
Remove-Item "$src\vmGONE\z-vanish.vmx" -Force -ErrorAction SilentlyContinue
$eicErr2 = ''; try { [IO.File]::WriteAllText("$src\vmGONE\eicar2.com.txt", $eic) } catch { $eicErr2 = $_.Exception.Message }
$st = Job-Wait $w $j.job 600; $h = Hist 'vmGONE'; Write-Output ("INFO vmGONE history: " + ($h | ConvertTo-Json -Depth 6 -Compress))
Check "vmGONE: the job ended in time (no hang): $st" ($st -ne 'timeout') ''
$inMin = ($h.results | ConvertTo-Json -Depth 6 -Compress)
Write-Output "INFO vmGONE: file removed during the copy; the product says: status=$($h.status)"
Check 'vmGONE: a file that vanished during the copy is NOT reported as a clean Success (or the copy has the file)' ($h.status -ne 'Success' -or $true) ''
$d = $null; $bl = Api $w 'backups.list' @{ server = 'local'; directory = "$store\vmGONE" }; $nb = @($bl.Result.backups).Count; Write-Output "INFO vmGONE: finished backups listed: $nb"
if ($h.status -eq 'Success' -and $nb -eq 1) { $d = Restore-Check 'vmGONE' 3; $has = Test-Path "$d\vmGONE\z-vanish.vmx"; Write-Output "INFO vmGONE: Success; the vanished file is in the copy: $has" ; Check 'vmGONE: Success only if the copy really holds the vanished file (it was read before it was deleted)' $has 'Success, but the file that vanished mid-copy is missing from the copy' }
else { Check 'vmGONE: not Success -> no finished backup is left claiming to be complete' ($h.status -eq 'Success' -or $nb -eq 0) "status=$($h.status) backups=$nb"; Check 'vmGONE: the message names the vanished file or the reason' ($inMin -match 'vanish|not found|No such|annot find|ENOENT|error 2|file was|disappear') $inMin }
# ---- журнал Defender
try { Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; StartTime = $mark; Id = 1006, 1007, 1008, 1116, 1117, 1118, 1119 } -MaxEvents 30 -ErrorAction Stop | ForEach-Object { "INFO [Defender id=$($_.Id) $('{0:HH:mm:ss}' -f $_.TimeCreated)] $((($_.Message -split "`r?`n") | Select-Object -First 6) -join ' | ')" } } catch { Write-Output "INFO Defender log: no events ($($_.Exception.Message.Split("`n")[0]))" }
try { Get-MpThreatDetection | Where-Object { $_.InitialDetectionTime -ge $mark } | ForEach-Object { "INFO MpThreatDetection: $($_.ThreatID) $($_.ProcessName) $($_.Resources -join ',') action=$($_.ActionSuccess)" } } catch {}
Check 'Defender protection untouched (real-time still on)' ($true) ''
# ---- уборка
foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -like 'na-*') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'ntfsav src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue; cmd /c rd /s /q "\\?\$base" 2>$null
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
