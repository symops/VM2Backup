# Smart App Control (Windows 11): включение в режим «On» и поведение подписанного / неподписанного установщика и продукта.
#   sac.ps1 info       состояние (VerifiedAndReputablePolicyState: 0 выключен, 1 включён, 2 оценка) и журнал CodeIntegrity
#   sac.ps1 enable     включает SAC через настоящий интерфейс «Безопасность Windows -> Управление приложениями и браузером -> Smart App Control -> On»
#                      (UI Automation: переключатель «On»); если не вышло - запись значения реестра (как в раунде 4, реального включения не даёт). Нужна перезагрузка.
#   sac.ps1 check      после перезагрузки: состояние; запуск вариантов установщика (неподписанный / подписанный / изменённый; с меткой Mark of the Web и без),
#                      службы и программ продукта; события CodeIntegrity 3076 / 3077 / 3033 / 3034 / 3099; снимки экрана
#   sac.ps1 trust      то же, но после добавления тестового корня в LocalMachine\Root и сертификата в TrustedPublisher
# ПОСЛЕ включения SAC без сброса Windows выключить нельзя: запускать ПОСЛЕДНЕЙ фазой перед удалением ВМ. Ничего, кроме этого значения, не меняется.
. C:\vm2b\guest\lib.ps1
$Mode = if ($args[0]) { $args[0] } else { 'info' }
$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy'
$pki = "$env:SRC\pki"; $work = 'C:\vm2b\sign'; $loc = "$work\variants"
if (-not (Test-Path "$loc\signed.exe")) { New-Item -ItemType Directory -Force $loc | Out-Null; Copy-Item "$pki\variants\*.exe" $loc -Force }
function Get-Sac { try { return [int](Get-ItemProperty $key -ErrorAction Stop).VerifiedAndReputablePolicyState } catch { return -1 } }
function Show-Sac($tag) { $s = Get-Sac; $t = switch ($s) { 0 { 'Off' } 1 { 'On (enforced)' } 2 { 'Evaluation' } default { 'not present' } }; Write-Host ("INFO  Smart App Control ${tag}: VerifiedAndReputablePolicyState=$s ($t)"); return $s }
$ciIds = 3076, 3077, 3033, 3034, 3099, 3023, 3035, 3036, 3089, 3090
function Show-CI($since, $max = 40) {
    foreach ($log in 'Microsoft-Windows-CodeIntegrity/Operational', 'Microsoft-Windows-Windows Defender/Operational', 'Microsoft-Windows-AppLocker/MSI and Script') {
        try {
            $f = @{ LogName = $log }; if ($since) { $f.StartTime = $since }
            Get-WinEvent -FilterHashtable $f -MaxEvents $max -ErrorAction Stop | Where-Object { $log -notmatch 'Defender' -or $_.Id -in 1116, 1117, 5007, 1015 } | ForEach-Object { Write-Host ("INFO  [{0}] id={1} {2:HH:mm:ss}: {3}" -f ($log -replace 'Microsoft-Windows-', ''), $_.Id, $_.TimeCreated, (($_.Message -split "`r?`n")[0..1] -join ' | ')) }
        } catch { Write-Host "INFO  [$log] no events" }
    }
}
function Sig($f) { $s = Get-AuthenticodeSignature $f; "Status={0}; Signer={1}" -f $s.Status, $(if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '-' }) }
function Shot($name) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\shot.ps1 $name | Out-Null }
function Ui-Find($names, $types, $seconds) {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds) {
        $root = [Windows.Automation.AutomationElement]::RootElement
        foreach ($e in $root.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) {
            try { if (($names -contains $e.Current.Name) -and ($e.Current.ControlType.ProgrammaticName -match $types)) { return $e } } catch {}
        }
        Start-Sleep -Milliseconds 700
    }
    return $null
}
if ($Mode -eq 'info') { Show-Sac 'now' | Out-Null; Show-CI $null 15; exit 0 }
if ($Mode -eq 'enable') {
    $before = Show-Sac 'before'
    if ($before -eq 1) { Write-Host 'INFO  already enforced'; exit 0 }
    # 1) настоящий путь: окно «Безопасность Windows» -> Smart App Control -> On
    Start-Process 'windowsdefender://smartapp'
    $on = Ui-Find @('On', 'Вкл.', 'Включено') 'RadioButton' 40
    if ($on) {
        Shot 'sac-ui-before'
        try { $on.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select(); Write-Host "INFO  UI: the radio button 'On' selected" } catch { Write-Host ("INFO  UI: cannot select: " + $_.Exception.Message) }
        Start-Sleep -Seconds 6; Shot 'sac-ui-after'
        # возможное окно подтверждения
        $c = Ui-Find @('Turn on', 'Включить', 'Yes', 'Да', 'OK', 'Continue', 'Продолжить') 'Button' 6
        if ($c) { try { $c.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke(); Write-Host "INFO  UI: confirmation '$($c.Current.Name)' pressed"; Start-Sleep -Seconds 4; Shot 'sac-ui-confirmed' } catch {} }
    } else { Write-Host 'INFO  UI: the Smart App Control page / the radio button On was not found'; Shot 'sac-ui-notfound' }
    Show-Sac 'after the UI change' | Out-Null
    Get-Process -Name SecHealthUI -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    # 2) запасной путь: запись значения напрямую (в раунде 4 не включила принудительное применение)
    if ((Get-Sac) -ne 1) {
        try { Set-ItemProperty -Path $key -Name VerifiedAndReputablePolicyState -Value 1 -Type DWord -ErrorAction Stop; Write-Host 'INFO  direct write as administrator: accepted' } catch { Write-Host ('INFO  direct write as administrator: refused: ' + $_.Exception.Message) }
        Show-Sac 'after the direct write' | Out-Null
    }
    if ((Get-Sac) -eq 1) { Write-Host 'INFO  the value is 1: a restart will show whether Windows honours it' } else { Write-Host 'INFO  the value stays as it was: Smart App Control could not be switched to On' }
    exit 0
}
if ($Mode -eq 'probe') {    # дополнительные пробы при включённом SAC: сеть, служба, события блокировки, свежесобранные неподписанные exe, повтор «изменённого» файла
    $s = Show-Sac 'probe'
    foreach ($h in 'www.microsoft.com', 'smartscreen.microsoft.com') { try { $r = [Net.Dns]::GetHostAddresses($h); Write-Host "INFO  DNS $h -> $($r[0])" } catch { Write-Host "INFO  DNS $h failed" } }
    try { $r = Invoke-WebRequest -UseBasicParsing -Uri 'https://www.microsoft.com' -TimeoutSec 15; Write-Host "INFO  HTTPS www.microsoft.com: $($r.StatusCode)" } catch { Write-Host ("INFO  HTTPS www.microsoft.com failed: " + $_.Exception.Message) }
    Get-Service VM2BackupService -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "INFO  service VM2BackupService: $($_.Status) (start type $($_.StartType))" }
    Get-Process -Name VM2BackupService -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "INFO  service process pid=$($_.Id) started $($_.StartTime)" }
    $d = "$env:USERPROFILE\Downloads\sac"; New-Item -ItemType Directory -Force $d | Out-Null
    for ($i = 1; $i -le 2; $i++) {
        foreach ($motw in $true, $false) {
            $f = "$d\fresh$i-$(if ($motw) { 'motw' } else { 'nomotw' }).exe"; Remove-Item $f -Force -ErrorAction SilentlyContinue
            Add-Type -TypeDefinition "public class P$i$(if ($motw) { 'm' } else { 'n' }) { public static int Main() { System.Console.WriteLine(`"fresh $i`"); return 7; } }" -OutputAssembly $f -OutputType ConsoleApplication -ErrorAction Stop
            if ($motw) { Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3" }
            $tc = Get-Date; $err = ''; $out = ''
            try { $out = & $f 2>&1 | Out-String; $code = $LASTEXITCODE } catch { $err = $_.Exception.Message; $code = 'n/a' }
            Write-Host ("INFO  fresh unsigned exe $(Split-Path $f -Leaf) (never seen by the cloud): exit=$code out='$($out.Trim())' $err")
            Show-CI $tc 6
        }
    }
    foreach ($r in 1..3) {
        $f = "$d\tampered-again$r.exe"; Copy-Item "$loc\tampered.exe" $f -Force; Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3"
        $tc = Get-Date; $err = ''
        try { $p = Start-Process -FilePath $f -PassThru -ErrorAction Stop; Start-Sleep -Seconds 8; $st = -not $p.HasExited; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } catch { $st = $false; $err = $_.Exception.Message }
        Write-Host "INFO  tampered with MOTW, try ${r}: started=$st $err"; Show-CI $tc 6
    }
    Get-Process | Where-Object { $_.ProcessName -match '^(VM2Backup-Setup|tampered-again\d)$' } | Stop-Process -Force -ErrorAction SilentlyContinue
    $n = (Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3076, 3077, 3033, 3034 } -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host "INFO  CodeIntegrity events 3076/3077/3033/3034 in the whole log: $n"
    Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3099 } -MaxEvents 6 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("INFO  CI 3099 {0:HH:mm:ss}: {1}" -f $_.TimeCreated, (($_.Message -split "`r?`n")[0])) }
    Shot 'sac-probe-end'
    exit 0
}
if ($Mode -eq 'probe2') {    # служба продукта и неподписанные exe сборки при включённом SAC
    $s = Show-Sac 'probe2'
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Service Control Manager' } -MaxEvents 400 -ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'VM2Backup' } | Select-Object -First 14 | ForEach-Object { Write-Host ("INFO  SCM id={0} {1:MM-dd HH:mm:ss}: {2}" -f $_.Id, $_.TimeCreated, (($_.Message -split "`r?`n")[0])) }
    Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-CodeIntegrity/Operational'; Id = 3077, 3033 } -ErrorAction SilentlyContinue | ForEach-Object { Write-Host ("INFO  CI block id={0} {1:MM-dd HH:mm:ss}: {2}" -f $_.Id, $_.TimeCreated, ($_.Message -replace '\\Device\\HarddiskVolume\d+', '' -replace 'Code Integrity determined that a process \(.*?\) attempted to load', 'blocked:' )) }
    $d = "$env:USERPROFILE\Downloads\sac"; New-Item -ItemType Directory -Force $d | Out-Null
    foreach ($n in 'VM2Backup.exe', 'VM2BackupService.exe', 'VM2Backup-Setup.exe') {
        $src = "$env:SRC\pkg\bin\$n"; if (-not (Test-Path $src)) { continue }
        $f = "$d\unsigned-$n"; Copy-Item $src $f -Force
        $tc = Get-Date; $err = ''; $st = $false; $code = ''
        try { $p = Start-Process -FilePath $f -PassThru -ErrorAction Stop; Start-Sleep -Seconds 8; $st = -not $p.HasExited; if ($p.HasExited) { $code = "exit $($p.ExitCode)" } else { Stop-Process -Id $p.Id -Force } } catch { $err = $_.Exception.Message }
        Write-Host "INFO  unsigned build file $n ($(Sig $f)): started=$st $code $err"; Show-CI $tc 6
    }
    exit 0
}
if ($Mode -eq 'trust') {
    Import-Certificate -FilePath "$pki\test-root.cer" -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath "$pki\test-sign.cer" -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Host 'INFO  test root -> LocalMachine\Root, signer -> LocalMachine\TrustedPublisher'; exit 0
}
if ($Mode -eq 'check' -or $Mode -eq 'check-trusted') {
    $s = Show-Sac "($Mode)"
    $svc = ((Get-Service VM2BackupService -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Status)" }) -join ' ')
    Write-Host ("INFO  product service: $svc")
    $pf = "$env:ProgramFiles\VM2Backup"
    foreach ($x in 'VM2Backup.exe', 'VM2BackupService.exe', 'VM2Backup-Setup.exe') { if (Test-Path "$pf\$x") { Write-Host ("INFO  installed $x :: " + (Sig "$pf\$x")) } }
    $t0 = Get-Date
    $d = "$env:USERPROFILE\Downloads\sac"; New-Item -ItemType Directory -Force $d | Out-Null
    # (имя, файл, метка Mark of the Web)
    $cases = @(
        @('unsigned-motw', "$loc\unsigned.exe", $true), @('unsigned-nomotw', "$loc\unsigned.exe", $false),
        @('signed-motw', "$loc\signed.exe", $true), @('signed-nomotw', "$loc\signed.exe", $false),
        @('tampered-motw', "$loc\tampered.exe", $true), @('tampered-nomotw', "$loc\tampered.exe", $false))
    foreach ($c in $cases) {
        $name = $c[0]; $f = "$d\$name.exe"; Copy-Item $c[1] $f -Force
        if ($c[2]) { Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3`r`nReferrerUrl=https://example.test/" }
        $tc = Get-Date; $err = ''; $started = $false; $code = ''
        try { $p = Start-Process -FilePath $f -PassThru -ErrorAction Stop; Start-Sleep -Seconds 14; $started = -not $p.HasExited; if ($p.HasExited) { $code = "exited with code $($p.ExitCode)" } } catch { $err = $_.Exception.Message }
        Shot "sac-$Mode-$name"
        Write-Host ("INFO  SAC state ${s}, ${name} ($(Sig $f)): started=$started $code $err")
        Show-CI $tc 6
        Get-Process | Where-Object { $_.ProcessName -match "^(VM2Backup-Setup|VM2Backup|$name)$" } | Stop-Process -Force -ErrorAction SilentlyContinue
        Get-Process -Name 'smartscreen', 'CHXSmartScreen' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    # тихая установка подписанного пакета без метки: ставится ли продукт, стартует ли служба
    $silent = "$d\signed-silent.exe"; Copy-Item "$loc\signed.exe" $silent -Force
    $tc = Get-Date; $rc = 'n/a'
    try { $p = Start-Process -FilePath $silent -ArgumentList '/install', '/silent' -PassThru -Wait -ErrorAction Stop; $rc = $p.ExitCode } catch { $rc = 'cannot start: ' + $_.Exception.Message }
    Start-Sleep -Seconds 10
    Write-Host ("INFO  SAC state ${s}: silent install of the signed package: rc=$rc; service: " + (((Get-Service VM2BackupService -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Status)" }) -join ' ')))
    foreach ($x in 'VM2Backup.exe', 'VM2BackupService.exe') { if (Test-Path "$pf\$x") { Write-Host ("INFO  installed after that $x :: " + (Sig "$pf\$x")) } }
    Show-CI $tc 20
    # неподписанная программа продукта из тестовой сборки: vm2bautotest.exe (в пакете её нет), служба из bin\ (не подписана)
    $at = "$env:SRC\pkg\bin\vm2bautotest.exe"
    if (Test-Path $at) {
        $tc = Get-Date; $o = & $at --only core --seed 1 2>&1 | Select-Object -Last 2; Write-Host ("INFO  vm2bautotest.exe (unsigned): rc=$LASTEXITCODE; " + ($o -join ' | ')); Show-CI $tc 6
    }
    Shot "sac-$Mode-end"
    Show-CI $t0 40
    if ($s -eq 1) { Write-Host 'PASS  Smart App Control is ENFORCED (state 1): the behaviour is recorded above (INFO lines, screenshots sac-*.png)' }
    else { Write-Host "INFO  Smart App Control is not enforced (state $s): enforcement NOT tested on this install" }
    exit 0
}
