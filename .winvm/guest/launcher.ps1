# launcher.ps1 (раунд 13, F7): лаунчер установщика на НАСТОЯЩЕЙ Windows.
#   * во время тихой установки в %TEMP% есть папка VM2Backup-Setup-<32 шестнадцатеричные цифры> (непредсказуемое имя), её DACL закрыт (только SYSTEM, Administrators и текущий пользователь);
#   * файлы в ней держатся открытыми: запись, удаление и переименование чужим процессом отвергаются (sharing violation), а повышенный установщик при этом запущен ИЗ неё и ЗАКАНЧИВАЕТ с кодом 0;
#   * после установки папка удалена;
#   * тихое обновление, тихое удаление и повторная установка работают с этим лаунчером;
#   * пакет с испорченным хешем (cab цел) отвергается: код не 0, в %TEMP%\VM2Backup-Setup.log понятная причина, ничего не установлено заново (служба прежняя), папка убрана.
# Варианты пакета готовит хост (tools/winvm-variants.sh): signed.exe, unsigned.exe, tampered-hash.exe.
. C:\vm2b\guest\lib.ps1
$pki = "$env:SRC\pki"; $work = 'C:\vm2b\f7'; New-Item -ItemType Directory -Force $work | Out-Null; Copy-Item "$pki\variants\*.exe" $work -Force
$B = "$env:ProgramFiles\VM2Backup"
function SvcPid { $p = Get-Process VM2BackupService -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p) { $p.Id } else { 0 } }
function Svc-Up($sec) { for ($i = 0; $i -lt $sec; $i++) { if ((Get-Service VM2BackupService -ErrorAction SilentlyContinue).Status -eq 'Running') { return $true }; Start-Sleep -Seconds 1 }; $false }
function TmpDirs { @(Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue) }
Write-Output "INFO TEMP = $env:TEMP, user = $env:USERNAME, elevated = $(([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:TEMP\VM2Backup-Setup.log" -ErrorAction SilentlyContinue

# ---- 1. тихая установка (обновление поверх) подписанного пакета с наблюдением за папкой распаковки
$exe = "$work\signed.exe"
$psi = New-Object Diagnostics.ProcessStartInfo $exe, '/install /silent'; $psi.UseShellExecute = $false
$p = [Diagnostics.Process]::Start($psi); $sw = [Diagnostics.Stopwatch]::StartNew()
$seen = $null; $obs = @{}; $names = @{}
$childRunsFromFolder = $false; $writeDenied = 0; $writeOk = 0; $delDenied = 0; $delOk = 0; $renDenied = 0; $renOk = 0; $readOk = 0; $sddl = ''; $nfiles = 0
$attackedAt = $null
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt 240) {
    $d = TmpDirs | Select-Object -First 1
    if ($d) {
        if (-not $seen) { $seen = $d.FullName; $sddl = (Get-Acl -LiteralPath $d.FullName).GetSecurityDescriptorSddlForm('Access,Owner'); Write-Output "INFO extraction folder seen after $([int]$sw.Elapsed.TotalMilliseconds) ms: $($d.FullName)"; Write-Output "INFO its SDDL: $sddl" }
        # нападаем только когда повышенный установщик уже запущен ИЗ папки: до этого файлы ещё распаковываются и не сверены (подмена до сверки правильно приводит к отказу «пакет повреждён»)
        $running = $false
        foreach ($q in @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'VM2Backup-Setup*' -and $_.Id -ne $p.Id })) { try { if ($q.Path -like "$($d.FullName)*") { $running = $true; $childRunsFromFolder = $true } } catch {} }
        if ($running) {
            if (-not $attackedAt) { $attackedAt = [int]$sw.Elapsed.TotalMilliseconds }
            foreach ($f in @(Get-ChildItem -LiteralPath $d.FullName -File -Recurse -ErrorAction SilentlyContinue)) {
                $nfiles++
                try { $fs = [IO.File]::Open($f.FullName, 'Open', 'Write', 'ReadWrite'); $fs.Close(); $writeOk++ } catch { $writeDenied++ }
                try { Rename-Item -LiteralPath $f.FullName -NewName ($f.Name + '.x') -ErrorAction Stop; $renOk++; Rename-Item -LiteralPath ($f.FullName + '.x') -NewName $f.Name -ErrorAction SilentlyContinue } catch { $renDenied++ }
                try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction Stop; $delOk++ } catch { $delDenied++ }
                try { $fs = [IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite'); $fs.Close(); $readOk++ } catch {}
            }
        }
    }
    Start-Sleep -Milliseconds 40
}
Write-Output "INFO the attack on the held files started $attackedAt ms after the start"
$hung = -not $p.HasExited; if ($hung) { $p.Kill() }; $rc = if ($hung) { -1 } else { $p.ExitCode }
Write-Output "INFO signed update (silent): rc=$rc, $([int]$sw.Elapsed.TotalSeconds) s; polled files: $nfiles; write denied/ok $writeDenied/$writeOk; rename denied/ok $renDenied/$renOk; delete denied/ok $delDenied/$delOk; read ok $readOk; installer process seen running from the folder: $childRunsFromFolder"
Check "silent install through the launcher ends with 0 (no sharing violation) (rc=$rc)" ($rc -eq 0 -and -not $hung) ''
Check 'an extraction folder with an unpredictable name was seen in %TEMP% during the install' ($seen -and ((Split-Path $seen -Leaf) -match '^VM2Backup-Setup-[0-9a-fA-F]{32}$')) "$seen"
Check 'the folder DACL is protected and does not grant Users / Everyone / Authenticated Users anything' ($sddl -match 'D:P' -and $sddl -notmatch ';;;(BU|WD|AU)\)') $sddl
Check 'the elevated installer was started FROM the held folder' $childRunsFromFolder 'no VM2Backup-Setup process with a path inside the folder was seen'
Check 'held files: writing was refused every time (and never succeeded)' ($writeDenied -gt 0 -and $writeOk -eq 0) "denied $writeDenied ok $writeOk"
Check 'held files: renaming was refused every time' ($renDenied -gt 0 -and $renOk -eq 0) "denied $renDenied ok $renOk"
Check 'held files: deleting was refused every time' ($delDenied -gt 0 -and $delOk -eq 0) "denied $delDenied ok $delOk"
Start-Sleep -Seconds 2
$left = TmpDirs; Check "the extraction folder is removed afterwards ($($left.Count) left)" ($left.Count -eq 0) (($left | ForEach-Object { $_.FullName }) -join ',')
Check 'service running after the update' (Svc-Up 90) ''
$logf = "$env:TEMP\VM2Backup-Setup.log"; Write-Output ("INFO VM2Backup-Setup.log after a successful install: " + $(if (Test-Path $logf) { (Get-Content $logf -Raw).Trim() } else { '(no file)' }))

# ---- 2. испорченный хеш в списке пакета
$pidBefore = SvcPid; $verBefore = (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion; $t0 = (Get-Item "$B\VM2BackupService.exe").LastWriteTimeUtc
Remove-Item $logf -ErrorAction SilentlyContinue; Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
$p2 = Start-Process -FilePath "$work\tampered-hash.exe" -ArgumentList '/install', '/silent' -PassThru; $ok2 = $p2.WaitForExit(120000); if (-not $ok2) { $p2.Kill() }
$logt = if (Test-Path $logf) { (Get-Content $logf -Raw) } else { '' }
Write-Output "INFO tampered-hash: rc=$($p2.ExitCode), finished=$ok2; VM2Backup-Setup.log: $($logt.Trim())"
Check 'tampered hash list: the installer ends (does not hang) with a non-zero code' ($ok2 -and $p2.ExitCode -ne 0) "rc=$($p2.ExitCode)"
Check 'tampered hash list: a clear reason is written to %TEMP%\VM2Backup-Setup.log' ($logt -match 'damaged or incomplete') $logt
Check 'tampered hash list: the installed product and the running service are untouched' ((Svc-Up 30) -and (SvcPid) -eq $pidBefore -and (Get-Item "$B\VM2BackupService.exe").LastWriteTimeUtc -eq $t0 -and (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion -eq $verBefore) "pid $pidBefore -> $(SvcPid)"
Check 'tampered hash list: the extraction folder is removed' ((TmpDirs).Count -eq 0) ''

# ---- 3. тихое удаление и повторная установка через лаунчер (установщик в Program Files сам не лаунчер, поэтому ещё и пакет)
$p3 = Start-Process -FilePath "$B\VM2Backup-Setup.exe" -ArgumentList '/remove', '/silent' -PassThru -Wait
Check "silent removal (rc=$($p3.ExitCode))" ($p3.ExitCode -eq 0 -and -not (Test-Path "$B\VM2BackupService.exe")) ''
Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
$p4 = Start-Process -FilePath "$work\unsigned.exe" -ArgumentList '/install', '/silent' -PassThru -Wait
Check "silent install of the unsigned package through the launcher (rc=$($p4.ExitCode))" ($p4.ExitCode -eq 0 -and (Svc-Up 90)) ''
Check 'no extraction folders left after the install and the removal' ((TmpDirs).Count -eq 0) ''
# повторить 15 раз подряд: ни разу sharing violation / оставленной папки
$bad = 0; for ($i = 0; $i -lt 15; $i++) { $q = Start-Process -FilePath "$work\unsigned.exe" -ArgumentList '/install', '/silent' -PassThru -Wait; if ($q.ExitCode -ne 0 -or (TmpDirs).Count -ne 0) { $bad++ } }
Check "15 silent updates in a row: all rc=0, no folder left ($bad bad)" ($bad -eq 0) ''
Check 'service running at the end' (Svc-Up 90) ''
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
