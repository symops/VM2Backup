# links.ps1 <links|crypt|foreign> (раунд 16): низкие находки ревью безопасности на НАСТОЯЩЕЙ службе и настоящей NTFS, через сайт 127.0.0.1:8443.
#   links:   F10 - источник «папка с ВМ» с junction и символической ссылкой внутри (mklink /J, mklink): содержимое «чужой» папки не попадает в копию;
#            F6  - восстановление в папку назначения, где уже лежат junction на чужую папку, символическая ссылка на чужой файл и жёсткая ссылка:
#                  отказ, «чужие» файлы не тронуты, за пределами папки назначения ничего не создано, состояние восстановления не в папке назначения;
#                  чистая папка назначения - восстановление проходит, хеши совпадают.
#   crypt:   V2BENC4: копия с паролем, verify, restore, неверный пароль, подмена байта в заголовке и в манифесте -> понятная ошибка, неверный пароль не раскрывает данных.
#            Копия остаётся в $Share\x86enc (ХОСТ читает её 64-битной службой под Wine: tools/read_foreign_archive.py).
#   foreign: зашифрованный архив, сделанный 64-битной службой (хост: VM2B_ARCHIVE_ENC=1 tools/make_x64_archive.py, копия в .winvm/shared/x64enc): list / verify / restore, хеши.
param([string]$Mode = 'links')
. C:\vm2b\guest\lib.ps1
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 1) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 1 }; return 'timeout' }
function Is-Ok($st) { return ($st -eq 'Success' -or $st -eq 'Gone') }
# текст ошибки последней записи истории (job уже ушёл из jobs.list)
function Last-Error($w) {
    $h = Api $w 'history.list' @{ limit = 1 }; $x = @($h.Result) | Select-Object -First 1; if (-not $x) { return '' }
    $g = Api $w 'history.get' @{ id = $x.id }; $t = $g.Text; if ($t.Length -gt 700) { $t = $t.Substring(0, 700) }; return $t
}
function Hash-Tree($dir) { $o = @{}; Get-ChildItem $dir -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object { $o[$_.FullName.Substring($dir.Length).TrimStart('\').ToLower()] = (Get-FileHash $_.FullName -Algorithm SHA1).Hash }; return $o }
function Find-Marker($dir, [string]$marker) {
    $m = [Text.Encoding]::ASCII.GetBytes($marker); $hits = 0
    foreach ($f in @(Get-ChildItem $dir -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        $b = [IO.File]::ReadAllBytes($f.FullName); $s = [Text.Encoding]::GetEncoding(28591).GetString($b); if ($s.Contains($marker)) { $hits++; Write-Output "INFO marker found in $($f.FullName)" }
    }
    return $hits
}
$root = 'C:\vm2b\lnk'
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if (-not $w) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
function Mk-El($sid, $vm, $dir, $enc, $pw) { @{ source = @{ server = $sid; vms = @($vm) }; target = @{ server = 'local'; directory = "$dir\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = $(if ($enc) { @{ enabled = $true; password = $pw } } else { @{ enabled = $false } }); verify = $false; keep = 0; segment_mib = 0 } }
function Mk-Server($w, $name, $path) {
    foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq $name) { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    return (Api $w 'servers.save' @{ server = @{ type = 'directory'; name = $name; path = $path } }).Result.id
}
function Run-Backup($w, $name, $el) {
    foreach ($x in @((Api $w 'tasks.list' @{}).Result)) { if ($x.name -eq $name) { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
    $tk = Api $w 'tasks.save' @{ task = @{ name = $name; enabled = $true; elements = @($el) } }; $r = Api $w 'tasks.run' @{ id = $tk.Result.id }
    $st = if ($r.Result.job) { Job-Wait $w $r.Result.job 600 } else { 'norun' }
    Api $w 'tasks.delete' @{ id = $tk.Result.id } | Out-Null
    return $st
}
function List-First($w, $dir, $pw) { $a = @{ server = 'local'; directory = $dir }; $r = Api $w 'backups.list' $a; return @{ Status = $r.Status; Text = $r.Text; B = @($r.Result.backups) | Select-Object -First 1 } }

if ($Mode -eq 'links') {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
    $out = "$root\outside"; New-Item -ItemType Directory -Force "$out\inner" | Out-Null
    $mk = 'LNKSECRET-' + [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText("$out\secret-file.txt", $mk + ' file ' + ('x' * 4000)); [IO.File]::WriteAllText("$out\inner\secret-dir.vmdk", $mk + ' dir ' + ('y' * 4000))
    # ---- F10: источник с ссылками внутри
    $src = "$root\src"; New-Item -ItemType Directory -Force "$src\vmL\real" | Out-Null
    $rnd = New-Object byte[] 600000; (New-Object Random 5).NextBytes($rnd); [IO.File]::WriteAllBytes("$src\vmL\disk.vmdk", $rnd); [IO.File]::WriteAllText("$src\vmL\real\a.txt", 'real file')
    cmd /c "mklink /J `"$src\vmL\jct`" `"$out\inner`"" | Out-Null
    cmd /c "mklink `"$src\vmL\sym.vmdk`" `"$out\secret-file.txt`"" | Out-Null
    cmd /c "mklink /D `"$src\vmL\symdir`" `"$out\inner`"" | Out-Null
    # жёсткая ссылка - обычная запись каталога, отличить её от файла нельзя: копируется как обычный файл (это не находка); метка у неё своя
    $mkH = 'LNKHARD-' + [guid]::NewGuid().ToString('N'); [IO.File]::WriteAllText("$out\hard-target.txt", $mkH); cmd /c "mklink /H `"$src\vmL\hard.vmdk`" `"$out\hard-target.txt`"" | Out-Null
    $nl = @(Get-ChildItem $src\vmL -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count
    Check 'setup: the source folder holds junction, directory symlink and file symlink (reparse points)' ($nl -ge 2) "$nl reparse points; $(@(Get-ChildItem $src\vmL -Force | ForEach-Object Name) -join ',')"
    $sid = Mk-Server $w 'lnk src' $src; $store = "$root\store10"
    $st = Run-Backup $w 'lnk-f10' (Mk-El $sid 'vmL' $store $false '')
    Write-Output "INFO F10 backup status: $st; error: $(Last-Error $w)"
    $b = List-First $w "$store\vmL" ''
    $rest = "$root\rest10"
    if ($b.B) {
        $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b.B.directory; archive = $b.B.archive; dest = $rest }; $rs = Job-Wait $w $rr.Result.job 600
        Write-Output "INFO F10 restore status: $rs"
        Check 'F10: the restored copy holds the real files' ((Test-Path "$rest\vmL\disk.vmdk") -and (Test-Path "$rest\vmL\real\a.txt")) ''
        Check 'F10: the real disk is intact' ((Test-Path "$rest\vmL\disk.vmdk") -and (Get-FileHash "$rest\vmL\disk.vmdk" -Algorithm SHA1).Hash -eq (Get-FileHash "$src\vmL\disk.vmdk" -Algorithm SHA1).Hash) ''
        $hits = Find-Marker $rest $mk; Write-Output "INFO F10: the hard link is copied as a plain file (its own marker found: $(Find-Marker $rest $mkH) time(s))"
        Check 'F10: nothing read through the junction / symlinks (the secret marker is NOT in the copy)' ($hits -eq 0) "$hits file(s) hold the marker"
        Write-Output ("INFO F10 restored tree: " + ((Get-ChildItem $rest -Recurse -Force | ForEach-Object { $_.FullName.Substring($rest.Length) }) -join ' | '))
    } else { Check 'F10: the backup of a folder with links either succeeded or was refused cleanly (no archive = refused)' ($st -ne 'Success') "status $st" ; Write-Output "INFO F10: no archive (copy refused): $($b.Text)" }
    Check 'F10: the files behind the links are not changed' (([IO.File]::ReadAllText("$out\secret-file.txt")).StartsWith($mk) -and (Test-Path "$out\inner\secret-dir.vmdk")) ''
    # ---- F6: чистая копия обычной ВМ
    $src2 = "$root\src6"; New-Item -ItemType Directory -Force "$src2\vmR\sub" | Out-Null
    $r6 = New-Object byte[] 400000; (New-Object Random 6).NextBytes($r6); [IO.File]::WriteAllBytes("$src2\vmR\disk.vmdk", $r6); [IO.File]::WriteAllText("$src2\vmR\sub\a.txt", 'sub file'); [IO.File]::WriteAllText("$src2\vmR\note.txt", 'note')
    $sid2 = Mk-Server $w 'lnk src6' $src2; $store6 = "$root\store6"
    $st = Run-Backup $w 'lnk-f6' (Mk-El $sid2 'vmR' $store6 $false ''); Check 'F6: setup: backup of a plain VM folder succeeded' (Is-Ok $st) $st
    $b6 = List-First $w "$store6\vmR" ''; Check 'F6: setup: the archive is listed' ($null -ne $b6.B) $b6.Text
    $outside = "$root\outside6"; New-Item -ItemType Directory -Force "$outside\subtarget" | Out-Null
    [IO.File]::WriteAllText("$outside\victim.txt", 'VICTIM-ORIGINAL'); [IO.File]::WriteAllText("$outside\victim2.txt", 'VICTIM2-ORIGINAL'); [IO.File]::WriteAllText("$outside\subtarget\a.txt", 'VICTIM-SUB-ORIGINAL')
    $before = Hash-Tree $outside
    function Try-Restore($label, $dest) {
        $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b6.B.directory; archive = $b6.B.archive; dest = $dest }; $rs = if ($rr.Result.job) { Job-Wait $w $rr.Result.job 300 } else { 'rejected:' + $rr.Status }
        $err = Last-Error $w; [Console]::Out.WriteLine("INFO F6 $label : status $rs; $($err.Substring(0, [Math]::Min(420, $err.Length)))")      # не Write-Output: он попал бы в возвращаемое значение функции
        $after = Hash-Tree $outside; $same = ($after.Count -eq $before.Count); foreach ($k in $before.Keys) { if ($after[$k] -ne $before[$k]) { $same = $false } }
        return @{ Status = $rs; Err = $err; Same = $same }
    }
    if ($b6.B) {
        # (a) junction вместо папки ВМ
        $d = "$root\d-junction"; New-Item -ItemType Directory -Force $d | Out-Null; cmd /c "mklink /J `"$d\vmR`" `"$outside`"" | Out-Null
        $x = Try-Restore 'junction in place of the VM folder' $d
        Check 'F6: restore into a destination whose VM folder is a junction is refused' (-not (Is-Ok $x.Status)) "$($x.Status)"
        Check 'F6: the folder behind the junction is unchanged (nothing written, nothing added)' $x.Same ''
        Check 'F6: the refusal message says it is a link' ($x.Err -match 'link') $x.Err
        # (b) символическая ссылка на файл вместо disk.vmdk
        $d = "$root\d-symfile"; New-Item -ItemType Directory -Force "$d\vmR" | Out-Null; cmd /c "mklink `"$d\vmR\disk.vmdk`" `"$outside\victim.txt`"" | Out-Null
        $x = Try-Restore 'file symlink in place of disk.vmdk' $d
        Check 'F6: restore over a symbolic link to a foreign file is refused' (-not (Is-Ok $x.Status)) "$($x.Status)"
        Check 'F6: the foreign file behind the symlink is unchanged' ($x.Same -and ([IO.File]::ReadAllText("$outside\victim.txt") -eq 'VICTIM-ORIGINAL')) ''
        # (c) junction вместо подпапки sub
        $d = "$root\d-subjct"; New-Item -ItemType Directory -Force "$d\vmR" | Out-Null; cmd /c "mklink /J `"$d\vmR\sub`" `"$outside\subtarget`"" | Out-Null
        $x = Try-Restore 'junction in place of the sub folder' $d
        Check 'F6: restore through a junction on a sub folder is refused' (-not (Is-Ok $x.Status)) "$($x.Status)"
        Check 'F6: the folder behind the sub junction is unchanged' $x.Same ''
        # (d) жёсткая ссылка на чужой файл
        $d = "$root\d-hard"; New-Item -ItemType Directory -Force "$d\vmR" | Out-Null; cmd /c "mklink /H `"$d\vmR\note.txt`" `"$outside\victim2.txt`"" | Out-Null
        $x = Try-Restore 'hard link in place of note.txt' $d
        Check 'F6: restore over a file with several hard links is refused' (-not (Is-Ok $x.Status)) "$($x.Status)"
        Check 'F6: the file shared by the hard link is unchanged' ($x.Same -and ([IO.File]::ReadAllText("$outside\victim2.txt") -eq 'VICTIM2-ORIGINAL')) ''
        Check 'F6: the hard-link refusal message names the hard links' ($x.Err -match 'hard link') $x.Err
        # (e) чистая папка назначения: проходит, состояние восстановления не лежит в ней
        $d = "$root\d-clean"; $x = Try-Restore 'clean destination' $d
        Check 'F6: restore into a clean destination succeeds' (Is-Ok $x.Status) $x.Status
        $h1 = Get-FileHash "$d\vmR\disk.vmdk" -Algorithm SHA1 -ErrorAction SilentlyContinue; $h0 = Get-FileHash "$src2\vmR\disk.vmdk" -Algorithm SHA1
        Check 'F6: the restored disk equals the source' ($h1 -and $h1.Hash -eq $h0.Hash) ''
        $names = @(Get-ChildItem $d -Recurse -Force | ForEach-Object { $_.FullName.Substring($d.Length) })
        Write-Output "INFO F6 clean destination tree: $($names -join ' | ')"
        Check 'F6: no state / temporary files are left in the destination' (-not ($names | Where-Object { $_ -match 'restore-state|\.tmp|\.part|\.vm2b' })) ($names -join ',')
        # (f) повтор после отказа: после удаления ссылки всё проходит (состояние не залипло)
        $d = "$root\d-junction"; cmd /c "rmdir `"$d\vmR`"" | Out-Null
        $x = Try-Restore 'after the junction is removed (retry)' $d
        Check 'F6: after the junction is removed the same restore succeeds (no stale state)' (Is-Ok $x.Status) $x.Status
        Check 'F6: the retry did not touch the folder the junction pointed to' $x.Same ''
    }
    $sl = Get-ChildItem 'C:\ProgramData\VM2Backup' -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'restore-state' } | Select-Object -First 3 -ExpandProperty FullName
    Write-Output "INFO restore-state locations: $($sl -join ' | ')"
    foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -like 'lnk src*') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    foreach ($d in 'd-junction', 'd-subjct') { Get-ChildItem "$root\$d" -Force -ErrorAction SilentlyContinue | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | ForEach-Object { cmd /c "rmdir `"$($_.FullName)`"" | Out-Null } }
    Get-ChildItem $root -Recurse -Force -ErrorAction SilentlyContinue | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint -and $_.PSIsContainer } | ForEach-Object { cmd /c "rmdir `"$($_.FullName)`"" | Out-Null }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
elseif ($Mode -eq 'crypt') {
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
    $src = "$root\src"; New-Item -ItemType Directory -Force "$src\vmE" | Out-Null
    $rnd = New-Object byte[] 3000000; (New-Object Random 9).NextBytes($rnd); [IO.File]::WriteAllBytes("$src\vmE\disk-flat.vmdk", $rnd); [IO.File]::WriteAllText("$src\vmE\vmE.vmx", 'config.version = "8"' + "`r`n" + ('a b c ' * 2000))
    $pw = 'Aa1-' + [guid]::NewGuid().ToString('N'); $bad = 'Zz9-' + [guid]::NewGuid().ToString('N')
    $sid = Mk-Server $w 'enc src' $src; $store = "$root\store"
    $st = Run-Backup $w 'enc-v4' (Mk-El $sid 'vmE' $store $true $pw); Check 'V2BENC4: encrypted backup succeeds' (Is-Ok $st) "$st $(Last-Error $w)"
    $b = List-First $w "$store\vmE" $pw; Check 'V2BENC4: the encrypted archive is listed (encrypted flag)' ($null -ne $b.B) $b.Text
    $files = @(Get-ChildItem "$store\vmE" -File | ForEach-Object { "$($_.Name) $($_.Length)" }); Write-Output "INFO V2BENC4 files: $($files -join ' | ')"
    $vol = Get-ChildItem "$store\vmE" -File | Where-Object { $_.Name -like '*.enc.0*' } | Select-Object -First 1
    $hd = New-Object byte[] 16; $fs = [IO.File]::OpenRead($vol.FullName); [void]$fs.Read($hd, 0, 16); $fs.Close()
    Write-Output ("INFO V2BENC4 volume starts with: " + [Text.Encoding]::ASCII.GetString($hd).Replace("`0", '.'))
    Check 'V2BENC4: the volume starts with the V2BENC4 magic' ([Text.Encoding]::ASCII.GetString($hd).StartsWith('V2BENC4')) ''
    $va = @{ server = 'local'; directory = $b.B.directory; archive = $b.B.archive; password = $pw }
    $r = Api $w 'backups.verify' $va; $s = Job-Wait $w $r.Result.job 600; Check 'V2BENC4: verify with the right password' (Is-Ok $s) "$s $(Last-Error $w)"
    $rest = "$root\rest"; $va2 = $va.Clone(); $va2.dest = $rest; $r = Api $w 'backups.restore' $va2; $s = Job-Wait $w $r.Result.job 600; Check 'V2BENC4: restore with the right password' (Is-Ok $s) "$s $(Last-Error $w)"
    $same = (Test-Path "$rest\vmE\disk-flat.vmdk") -and ((Get-FileHash "$rest\vmE\disk-flat.vmdk" -Algorithm SHA1).Hash -eq (Get-FileHash "$src\vmE\disk-flat.vmdk" -Algorithm SHA1).Hash) -and ((Get-FileHash "$rest\vmE\vmE.vmx" -Algorithm SHA1).Hash -eq (Get-FileHash "$src\vmE\vmE.vmx" -Algorithm SHA1).Hash)
    Check 'V2BENC4: restored files equal the sources' $same ''
    # неверный пароль
    $vb = $va.Clone(); $vb.password = $bad; $r = Api $w 'backups.verify' $vb; $s = if ($r.Result.job) { Job-Wait $w $r.Result.job 300 } else { "rejected $($r.Status)" }; $e = if ($r.Result.job) { Last-Error $w } else { $r.Text }
    Write-Output "INFO wrong password: $s; $e"
    Check 'V2BENC4: wrong password -> verify fails with a clear message' (-not (Is-Ok $s) -and ($e -match 'password|decrypt|authent')) "$s $e"
    $rb = "$root\rest-bad"; $vb.dest = $rb; $r = Api $w 'backups.restore' $vb; $s = if ($r.Result.job) { Job-Wait $w $r.Result.job 300 } else { "rejected $($r.Status)" }
    Check 'V2BENC4: wrong password -> restore fails and writes no VM data' (-not (Is-Ok $s) -and -not (Test-Path "$rb\vmE\disk-flat.vmdk")) $s
    # подмена байта: копии в отдельные папки
    function Tamper-Case($label, $what) {
        $dir = "$root\t-$label"; Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$dir\vmE" | Out-Null; Copy-Item "$store\vmE\*" "$dir\vmE\"
        $target = Get-ChildItem "$dir\vmE" -File | Where-Object { $what.Invoke($_) } | Select-Object -First 1
        $bytes = [IO.File]::ReadAllBytes($target.FullName)
        switch ($label) {
            'header'   { $pos = 12 }
            'body'     { $pos = [int]($bytes.Length / 2) }
            'manifest' { $txt = [Text.Encoding]::UTF8.GetString($bytes); $m = [regex]::Match($txt, '"size"\s*:\s*\d+'); if (-not $m.Success) { $m = [regex]::Match($txt, '"name"\s*:\s*"[^"]+') }; $pos = $m.Index + $m.Length - 1 }
        }
        $bytes[$pos] = $bytes[$pos] -bxor 0x01; [IO.File]::WriteAllBytes($target.FullName, $bytes)
        $l = List-First $w "$dir\vmE" $pw; $res = @{ Listed = ($null -ne $l.B); ListText = $l.Text; File = $target.Name; Pos = $pos }
        if ($l.B) { $a = @{ server = 'local'; directory = $l.B.directory; archive = $l.B.archive; password = $pw }; $r = Api $w 'backups.verify' $a; $res.Status = if ($r.Result.job) { Job-Wait $w $r.Result.job 300 } else { "rejected $($r.Status)" }; $res.Err = if ($r.Result.job) { Last-Error $w } else { $r.Text } }
        else { $res.Status = 'not listed'; $res.Err = $l.Text }
        return $res
    }
    foreach ($c in @(@('header', { param($f) $f.Name -like '*.enc.0*' }), @('body', { param($f) $f.Name -like '*.enc.0*' }), @('manifest', { param($f) $f.Name -like '*.json' }))) {
        $t = Tamper-Case $c[0] $c[1]; $e = "$($t.Err)"; if ($e.Length -gt 400) { $e = $e.Substring(0, 400) }
        Write-Output "INFO tampered $($c[0]) ($($t.File) byte $($t.Pos)): listed=$($t.Listed) status=$($t.Status) :: $e"
        Check "V2BENC4: a flipped byte in the $($c[0]) is detected (not listed / verify fails)" (-not $t.Listed -or -not (Is-Ok $t.Status)) "$($t.Status)"
        Check "V2BENC4: the $($c[0]) tamper message is readable (not empty, not a crash)" ($e.Length -gt 10 -and $e -notmatch '0xC0000005|exception') $e
    }
    Remove-Item -Recurse -Force "$Share\x86enc" -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$Share\x86enc\store" | Out-Null
    Copy-Item -Recurse "$store\vmE" "$Share\x86enc\store\vmE"; Set-Content "$Share\x86enc\password.txt" $pw
    "disk-flat.vmdk $((Get-FileHash "$src\vmE\disk-flat.vmdk" -Algorithm SHA1).Hash.ToLower())`nvme.vmx $((Get-FileHash "$src\vmE\vmE.vmx" -Algorithm SHA1).Hash.ToLower())" | Set-Content "$Share\x86enc\files.sha1"
    foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'enc src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
elseif ($Mode -eq 'foreign') {
    $A = "$Share\x64enc"; $loc = 'C:\vm2b\x64enc'; $rest = 'C:\vm2b\x64enc-rest'; Remove-Item -Recurse -Force $loc, $rest -ErrorAction SilentlyContinue; Copy-Item -Recurse $A $loc
    $pw = (Get-Content "$loc\password.txt" -Raw).Trim()
    $b = List-First $w "$loc\store\vmX" $pw; Check 'foreign encrypted archive (made by the 64-bit service) is listed' ($null -ne $b.B) $b.Text
    if ($b.B) {
        Write-Output ("INFO foreign encrypted entry: " + ($b.B | ConvertTo-Json -Compress -Depth 4))
        $va = @{ server = 'local'; directory = $b.B.directory; archive = $b.B.archive; password = $pw }
        $r = Api $w 'backups.verify' $va; $s = Job-Wait $w $r.Result.job 3600; Check 'foreign encrypted archive: verify on 32 bits' (Is-Ok $s) "$s $(Last-Error $w)"
        $va.dest = $rest; $r = Api $w 'backups.restore' $va; $s = Job-Wait $w $r.Result.job 3600; Check 'foreign encrypted archive: restore on 32 bits' (Is-Ok $s) "$s $(Last-Error $w)"
        $vmdir = Get-ChildItem $rest -Directory | Select-Object -First 1; $badn = 0; $n = 0
        foreach ($line in (Get-Content "$loc\files.sha1")) { $f = $line -split ' '; $p = Join-Path $vmdir.FullName $f[0]; $n++
            $h = if (Test-Path $p) { (Get-FileHash $p -Algorithm SHA1).Hash.ToLower() } else { 'missing' }; if ($h -ne $f[1]) { $badn++; Write-Output "INFO differs: $($f[0])" } }
        Check "foreign encrypted archive: all $n restored files equal the sources ($badn differ)" ($n -gt 0 -and $badn -eq 0) ''
        $vb = $va.Clone(); $vb.password = 'wrong-' + $pw; $vb.Remove('dest'); $r = Api $w 'backups.verify' $vb; $s = Job-Wait $w $r.Result.job 600
        Check 'foreign encrypted archive: wrong password is refused' (-not (Is-Ok $s)) $s
    }
    Remove-Item -Recurse -Force $loc, $rest -ErrorAction SilentlyContinue
}
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
