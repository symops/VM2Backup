# signinst.ps1: ПОДПИСАННЫЙ установщик на Windows 10 (в раунде 7 проверена только Windows 11 x64): Authenticode, тихая установка (32-битный загрузчик на x86), подписи внутренних exe,
# обновление поверх старой версии, файл с Mark of the Web, изменённый после подписи файл (подпись не проходит, а установщик должен вести себя понятно).
# Варианты делает хост (tools/winvm-variants.sh): unsigned / signed / tampered (байт в PE) / tampered-cab (байт внутри пакета).
. C:\vm2b\guest\lib.ps1
$pki = "$env:SRC\pki"; $work = 'C:\vm2b\sign2'; $loc = "$work\variants"; New-Item -ItemType Directory -Force $loc | Out-Null; Copy-Item "$pki\variants\*.exe" $loc -Force
$rootCer = "$pki\test-root.cer"; $signCer = "$pki\test-sign.cer"; $B = "$env:ProgramFiles\VM2Backup"
function Thumb($f) { (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $f).Thumbprint }
function Sig($f) { Get-AuthenticodeSignature $f }
function Sline($f) { $s = Sig $f; "{0}: Status={1}; Signer={2}; {3}" -f (Split-Path $f -Leaf), $s.Status, $(if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '-' }), $s.StatusMessage }
function Machine($f) { $b = [IO.File]::ReadAllBytes($f); $pe = [BitConverter]::ToInt32($b, 0x3C); '0x{0:X}' -f [BitConverter]::ToUInt16($b, $pe + 4) }
function Untrust { foreach ($st in 'Root', 'TrustedPublisher') { foreach ($f in @($rootCer, $signCer)) { $t = Thumb $f; Get-ChildItem "Cert:\LocalMachine\$st" | Where-Object Thumbprint -eq $t | Remove-Item -Force } } }
function Motw($f) { Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3`r`nReferrerUrl=https://example.test/`r`nHostUrl=https://example.test/VM2Backup-Setup.exe" }
function Click-Dialog($names, $sec) {      # UI Automation: кнопка окна «Открыть файл - предупреждение системы безопасности» (Windows 10 показывает его для файла с Mark of the Web)
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $t = Get-Date
    while (((Get-Date) - $t).TotalSeconds -lt $sec) {
        $all = [Windows.Automation.AutomationElement]::RootElement.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
        foreach ($e in $all) { try { $n = $e.Current.Name } catch { continue }
            if ($n -and ($names -contains $n.Trim()) -and $e.Current.ControlType.ProgrammaticName -match 'Button') { try { $e.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); return $n } catch {} } }
        Start-Sleep -Milliseconds 500
    }
    return $null
}
function RunSetup($exe, $args2, $sec, [switch]$motw) {    # CreateProcess (как запускают средства развёртывания и скрипты: без запроса «Открыть файл» даже для файла с Mark of the Web); возвращает @{rc; hung; ms}
    $t = Get-Date; $psi = New-Object Diagnostics.ProcessStartInfo $exe, $args2; $psi.UseShellExecute = $false; $p = [Diagnostics.Process]::Start($psi)
    if (-not $p.WaitForExit($sec * 1000)) { try { Take-Screenshot "$Share\results\shots\signinst-hang-$(Split-Path $exe -Leaf).png" } catch {}; $p.Kill(); return @{ rc = -1; hung = $true; ms = $sec * 1000; dialog = $false } }
    return @{ rc = $p.ExitCode; hung = $false; ms = [int]((Get-Date) - $t).TotalMilliseconds; dialog = $false }
}
function RunSetupShell($exe, $args2, $sec, $tag) {    # как «двойной щелчок» (ShellExecute): для файла с Mark of the Web Windows 10 спрашивает «Открыть файл - предупреждение системы безопасности». Запуск идёт в фоновом задании (Start-Process блокируется на окне), потом снимок и «Запустить»
    $t = Get-Date
    $job = Start-Job -ScriptBlock { param($f, $a) $p = Start-Process -FilePath $f -ArgumentList $a -PassThru; $p.WaitForExit(); $p.ExitCode } -ArgumentList $exe, $args2
    Start-Sleep -Seconds 7; try { Take-Screenshot "$Share\results\shots\signinst-securitywarning-$tag.png" } catch {}
    $c = Click-Dialog @('Run', 'Запустить') 25; if ($c) { Write-Output "INFO the 'Open File - Security Warning' window was shown for the ShellExecute start of the Mark-of-the-Web file ($tag) and answered with '$c'" } else { Write-Output "INFO no 'Open File - Security Warning' window for the ShellExecute start ($tag)" }
    $done = Wait-Job $job -Timeout $sec; $rc = $null; if ($done) { $rc = Receive-Job $job } else { Stop-Job $job }; Remove-Job $job -Force
    return @{ hung = (-not $done); rc = $rc; ms = [int]((Get-Date) - $t).TotalMilliseconds; dialog = [bool]$c }
}
function SvcPid { $p = Get-Process VM2BackupService -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p) { $p.Id } else { 0 } }
function Svc-Up($sec) { for ($i = 0; $i -lt $sec; $i++) { if ((Get-Service VM2BackupService -ErrorAction SilentlyContinue).Status -eq 'Running') { return $true }; Start-Sleep -Seconds 1 }; $false }
Untrust
Write-Output "INFO guest: $((Get-CimInstance Win32_OperatingSystem).Caption) $([Environment]::OSVersion.Version), PROCESSOR_ARCHITECTURE=$env:PROCESSOR_ARCHITECTURE"
# 0. чистое начало
if (Test-Path "$B\VM2Backup-Setup.exe") { $r = RunSetup "$B\VM2Backup-Setup.exe" '/remove /silent' 180; Write-Output "INFO removal of the previous product: rc=$($r.rc)" }
# 1. подписи до доверия
foreach ($v in 'unsigned', 'signed', 'tampered', 'tampered-cab') { Write-Output ("INFO [no trust] " + (Sline "$loc\$v.exe")) }
$s0 = Sig "$loc\signed.exe"; Check 'signed: signature present, chain stops at the untrusted test root (UnknownError / NotTrusted)' ($s0.Status -ne 'NotSigned' -and $s0.Status -ne 'Valid' -and $s0.SignerCertificate.Subject -match 'Symo') "$($s0.Status)"
Check 'unsigned: NotSigned' ((Sig "$loc\unsigned.exe").Status -eq 'NotSigned') ''
# 2. после доверия корню и издателю
foreach ($pair in @(@($rootCer, 'Root'), @($signCer, 'TrustedPublisher'))) { try { Import-Certificate -FilePath $pair[0] -CertStoreLocation "Cert:\LocalMachine\$($pair[1])" -ErrorAction Stop | Out-Null } catch { Write-Output "INFO Import-Certificate ($($pair[1])): $($_.Exception.Message); trying certutil"; & certutil.exe -f -addstore $pair[1] $pair[0] | Out-Null } }
Write-Output ('INFO trust stores: root=' + [bool](Get-ChildItem Cert:\LocalMachine\Root | Where-Object Thumbprint -eq (Thumb $rootCer)) + ' publisher=' + [bool](Get-ChildItem Cert:\LocalMachine\TrustedPublisher | Where-Object Thumbprint -eq (Thumb $signCer)))
foreach ($v in 'unsigned', 'signed', 'tampered', 'tampered-cab') { Write-Output ("INFO [trusted] " + (Sline "$loc\$v.exe")) }
Check 'signed + trusted root: Valid' ((Sig "$loc\signed.exe").Status -eq 'Valid') (Sline "$loc\signed.exe")
Check 'tampered (a byte of the launcher changed after signing): HashMismatch' ((Sig "$loc\tampered.exe").Status -eq 'HashMismatch') (Sline "$loc\tampered.exe")
Check 'tampered-cab (a byte of the package changed after signing): HashMismatch' ((Sig "$loc\tampered-cab.exe").Status -eq 'HashMismatch') (Sline "$loc\tampered-cab.exe")
Write-Output "INFO signed file: $((Get-Item "$loc\signed.exe").Length) bytes, launcher machine type $(Machine "$loc\signed.exe") (0x14C = 32-bit)"
Check 'the single-file installer is a 32-bit PE (the 32-bit launcher)' ((Machine "$loc\signed.exe") -eq '0x14C') (Machine "$loc\signed.exe")
# 3. старая версия и данные
Copy-Item "$env:SRC\pkg\old.exe" "$work\old.exe" -Force -ErrorAction SilentlyContinue
if (Test-Path "$work\old.exe") {
    $r = RunSetup "$work\old.exe" '/install /silent' 300; Check "old package installs silently (rc=$($r.rc), $($r.ms) ms)" ($r.rc -eq 0 -and -not $r.hung) ''
    Check 'service running after the old install' (Svc-Up 90) ''
    & powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\kept.ps1 add | Out-Null
    $oldVer = (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion; $oldPid = SvcPid; Write-Output "INFO old: version $oldVer, service pid $oldPid"
}
# 4. ОБНОВЛЕНИЕ подписанным пакетом (файл с Mark of the Web, как после скачивания) поверх работающей службы
$f = "$work\dl\VM2Backup-Setup.exe"; New-Item -ItemType Directory -Force "$work\dl" | Out-Null; Copy-Item "$loc\signed.exe" $f -Force; Motw $f
Check 'the downloaded copy carries the Mark of the Web' ((Get-Content $f -Stream Zone.Identifier -ErrorAction SilentlyContinue) -match 'ZoneId=3') ''
$r = RunSetup $f '/install /silent' 300 -motw; Write-Output "INFO signed update over the old version (file with the Mark of the Web, started by CreateProcess like a deployment tool does): rc=$($r.rc), $($r.ms) ms"
Check "signed update over the old version: rc 0, no window left open (rc=$($r.rc))" ($r.rc -eq 0 -and -not $r.hung) ''
Check 'service running after the signed update' (Svc-Up 90) ''
$newVer = (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion; $newPid = SvcPid; Write-Output "INFO new: version $newVer, service pid $newPid"
if ($oldVer) { Check 'the update changed the version and restarted the service' ($newVer -ne $oldVer -and $newPid -ne $oldPid -and $newPid -ne 0) "old $oldVer/$oldPid new $newVer/$newPid" }
$st = & powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\kept.ps1 state; Check 'data kept over the update (server record)' ($st -match 'kept server') ($st -join ' ')
# 5. подписи внутренних файлов в Program Files
foreach ($n in 'VM2Backup.exe', 'VM2BackupService.exe', 'VM2Backup-Setup.exe') {
    $p = "$B\$n"; Write-Output ("INFO [installed] " + (Sline $p) + "; machine " + (Machine $p))
    Check "installed $n is signed and Valid (root trusted)" ((Sig $p).Status -eq 'Valid') (Sline $p)
}
Check 'the installed service is 32-bit on this x86 Windows (0x14C), or 0x8664 on x64' ((Machine "$B\VM2BackupService.exe") -in '0x14C', '0x8664') (Machine "$B\VM2BackupService.exe")
Write-Output "INFO machine type of the installed service: $(Machine "$B\VM2BackupService.exe") on PROCESSOR_ARCHITECTURE=$env:PROCESSOR_ARCHITECTURE"
$svcPath = (Get-CimInstance Win32_Service -Filter "Name='VM2BackupService'").PathName; Write-Output "INFO service path: $svcPath"
# 6. изменённый ПОСЛЕ подписи файл (байт внутри PE загрузчика): подпись не проходит, пакет цел -> установщик работает как обычно
$f2 = "$work\dl\tampered.exe"; Copy-Item "$loc\tampered.exe" $f2 -Force; Motw $f2
$r = RunSetup $f2 '/install /silent' 300 -motw; Write-Output "INFO tampered (launcher byte): rc=$($r.rc), hung=$($r.hung), $($r.ms) ms"
Check "tampered launcher byte: the installer itself does not hang and returns 0 (rc=$($r.rc)); the signature check is done by Windows, not by the installer" ($r.rc -eq 0 -and -not $r.hung) ''
Check 'service running after it' (Svc-Up 90) ''
Check 'installed files still Valid after the install of the tampered launcher (the payload inside is intact)' ((Sig "$B\VM2BackupService.exe").Status -eq 'Valid') (Sline "$B\VM2BackupService.exe")
# 7. изменённый байт ВНУТРИ пакета: понятная ошибка, не зависание, установленное не ломается
$pidBefore = SvcPid; $verBefore = (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion
$f3 = "$work\dl\tampered-cab.exe"; Copy-Item "$loc\tampered-cab.exe" $f3 -Force; Motw $f3
Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
$r = RunSetup $f3 '/install /silent' 180 -motw; Write-Output "INFO tampered-cab (package byte): rc=$($r.rc), hung=$($r.hung), $($r.ms) ms"
$logf = "$env:TEMP\VM2Backup-Setup.log"; $logt = if (Test-Path $logf) { (Get-Content $logf -Raw) } else { '' }; Write-Output "INFO VM2Backup-Setup.log after the damaged package: $($logt.Trim())"
Check "tampered package: the reason is written to %TEMP%\VM2Backup-Setup.log (damaged or incomplete package)" ($logt -match 'damaged or incomplete') $logt
Check "tampered package: the installer does not hang (it ends with an exit code in $($r.ms) ms)" (-not $r.hung) 'hung: see signinst-hang-*.png'
Check "tampered package: the exit code is NOT 0 (rc=$($r.rc))" ($r.rc -ne 0) ''
Check 'tampered package: the installed product is not damaged (service running, same version)' ((Svc-Up 30) -and (Get-Item "$B\VM2BackupService.exe").VersionInfo.FileVersion -eq $verBefore) ''
Check 'tampered package: the same service process went on (not restarted by a failed install)' ((SvcPid) -eq $pidBefore) "pid before $pidBefore, after $(SvcPid)"
$left = @(Get-ChildItem $env:TEMP -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue).Count; Check "tampered package: the temporary extraction folder is removed ($left left)" ($left -eq 0) ''
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-4) } -MaxEvents 20 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'VM2Backup' }
foreach ($e in @($ev)) { Write-Output "INFO [Application id=$($e.Id)] $(($e.Message -split "`r?`n")[0])" }
# 8. удаление и повторная установка подписанного
$r = RunSetup "$B\VM2Backup-Setup.exe" '/remove /silent' 180; Check "silent removal of the signed install (rc=$($r.rc))" ($r.rc -eq 0 -and -not $r.hung) ''
Check 'files gone after removal' (-not (Test-Path "$B\VM2BackupService.exe")) ''
$r = RunSetup $f '/install /silent' 300 -motw; Check "silent reinstall of the signed package (rc=$($r.rc))" ($r.rc -eq 0) ''
Check 'service running after the reinstall' (Svc-Up 90) ''
Check 'reinstalled files Valid' ((Sig "$B\VM2Backup.exe").Status -eq 'Valid' -and (Sig "$B\VM2BackupService.exe").Status -eq 'Valid') ''
# справка (без проверок): тот же файл с Mark of the Web, запущенный через ShellExecute (как двойной щелчок в проводнике) - Windows 10 показывает «Открыть файл - предупреждение системы безопасности»
$r = RunSetupShell $f '/install /silent' 40 'doubleclick'; Write-Output "INFO ShellExecute start of the Mark-of-the-Web file: window answered=$($r.dialog), hung=$($r.hung), $($r.ms) ms"
Get-Process -Name 'VM2Backup-Setup' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process -Name 'SearchApp','OpenWith' -ErrorAction SilentlyContinue | Out-Null
Untrust; Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
