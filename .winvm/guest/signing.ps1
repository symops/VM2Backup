# Проверка ПОДПИСАННОГО установщика на Windows 11: Authenticode, SmartScreen (Mark of the Web), UAC (издатель), Smart App Control.
#   signing.ps1 info                  политика UAC, хранилища сертификатов, подписи всех вариантов установщика (Get-AuthenticodeSignature)
#   signing.ps1 trust                 тестовый корень -> LocalMachine\Root, сертификат подписи -> LocalMachine\TrustedPublisher
#   signing.ps1 untrust               убрать оба
#   signing.ps1 smartscreen on|off    включить SmartScreen для приложений и файлов (на тестовых ВМ его выключает bootstrap.cmd политикой EnableSmartScreen=0: тогда окна «Windows protected your PC» нет вовсе) / вернуть как было
#   signing.ps1 uac on|off|reset      включить запрос UAC на безопасном рабочем столе для теста (ConsentPromptBehaviorAdmin=2) / вернуть исходные значения
#   signing.ps1 launch <вариант> [nomotw]   вариант = signed | unsigned | tampered: копия в Загрузки\<вариант>\VM2Backup-Setup.exe с Mark of the Web (ZoneId=3),
#                                     запуск «двойным щелчком» (explorer.exe); возвращается сразу, снимки делает хост
#   signing.ps1 click more|run|runfile|dont [секунд]   нажать в окне SmartScreen «Подробнее» / «Выполнить в любом случае» / в окне «Открыть файл - предупреждение
#                                     системы безопасности» «Запустить» / «Не выполнять» (UI Automation по имени; в cmd кириллицу не передать, поэтому ключевые слова)
#   signing.ps1 state                 процессы Setup / smartscreen / consent, подпись запущенного файла
#   signing.ps1 kill                  завершить Setup / smartscreen / consent, удалить копии из Загрузок
#   signing.ps1 events                журнал CodeIntegrity (Operational) и Windows Defender SmartScreen / AppLocker за последние минуты
. C:\vm2b\guest\lib.ps1
$Mode = $args[0]; $Arg1 = $args[1]
$pki = "$env:SRC\pki"; $work = 'C:\vm2b\sign'; New-Item -ItemType Directory -Force $work | Out-Null
$rootCer = "$pki\test-root.cer"; $signCer = "$pki\test-sign.cer"
$loc = "$work\variants"; if (-not (Test-Path "$loc\signed.exe")) { New-Item -ItemType Directory -Force $loc | Out-Null; Copy-Item "$pki\variants\*.exe" $loc -Force }
function Get-Thumb($f) { (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $f).Thumbprint }
function Sig($f) { $s = Get-AuthenticodeSignature $f; "{0}: Status={1}; Signer={2}; Msg={3}" -f (Split-Path $f -Leaf), $s.Status, $(if ($s.SignerCertificate) { $s.SignerCertificate.Subject } else { '-' }), $s.StatusMessage }
function Win-Ru($t) { $t }
switch ($Mode) {
 'info' {
    $p = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'; $k = Get-ItemProperty $p
    "INFO  UAC: EnableLUA=$($k.EnableLUA) ConsentPromptBehaviorAdmin=$($k.ConsentPromptBehaviorAdmin) PromptOnSecureDesktop=$($k.PromptOnSecureDesktop) FilterAdministratorToken=$($k.FilterAdministratorToken)"
    foreach ($t in @((Get-Thumb $rootCer), (Get-Thumb $signCer))) { foreach ($st in 'Root', 'TrustedPublisher', 'CA') { $f = Get-ChildItem "Cert:\LocalMachine\$st" | Where-Object Thumbprint -eq $t; "INFO  thumbprint $t in LocalMachine\${st}: $([bool]$f)" } }
    foreach ($v in 'unsigned', 'signed', 'tampered') { "INFO  " + (Sig "$loc\$v.exe") }
    try { "INFO  SAC VerifiedAndReputablePolicyState=" + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState } catch { 'INFO  SAC: not present' }
    "INFO  SmartScreen Explorer=" + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer' -ErrorAction SilentlyContinue).SmartScreenEnabled
 }
 'prep' {    # снимки хоста посылают клавишу Shift (пробуждение экрана): пять нажатий включали окно «Залипание клавиш»; сочетание отключаем на время теста
    Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class StickyOff { [StructLayout(LayoutKind.Sequential)] public struct STICKYKEYS { public uint cbSize; public uint dwFlags; } [DllImport("user32.dll", SetLastError = true)] public static extern bool SystemParametersInfo(uint a, uint b, ref STICKYKEYS c, uint d); }' -ErrorAction SilentlyContinue
    $sk = New-Object StickyOff+STICKYKEYS; $sk.cbSize = 8; $sk.dwFlags = 2    # SKF_AVAILABLE, без горячей клавиши
    [StickyOff]::SystemParametersInfo(0x3B, 8, [ref]$sk, 0) | Out-Null
    Set-ItemProperty 'HKCU:\Control Panel\Accessibility\StickyKeys' Flags '506' -ErrorAction SilentlyContinue
    Get-Process -Name sethc -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    "INFO  sticky keys hotkey off"
 }
 'trust' {
    Import-Certificate -FilePath $rootCer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $signCer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    "INFO  trusted: root -> LocalMachine\Root, signer -> LocalMachine\TrustedPublisher"
 }
 'untrust' {
    foreach ($st in 'Root', 'TrustedPublisher') { foreach ($f in @($rootCer, $signCer)) { $t = Get-Thumb $f; Get-ChildItem "Cert:\LocalMachine\$st" | Where-Object Thumbprint -eq $t | Remove-Item -Force } }
    "INFO  trust removed"
 }
 'smartscreen' {
    $pol = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System'; $ex = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer'; $bak = "$work\smartscreen-backup.txt"
    if ($Arg1 -eq 'on') {
        $old = (Get-ItemProperty $pol -ErrorAction SilentlyContinue).EnableSmartScreen; $oldEx = (Get-ItemProperty $ex -ErrorAction SilentlyContinue).SmartScreenEnabled
        if (-not (Test-Path $bak)) { "policy=$old;explorer=$oldEx" | Set-Content $bak }
        Remove-ItemProperty $pol -Name EnableSmartScreen -ErrorAction SilentlyContinue; Set-ItemProperty $ex -Name SmartScreenEnabled -Value 'Warn' -Type String
        New-Item -Path 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppHost' -Force | Out-Null; Set-ItemProperty 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppHost' -Name EnableWebContentEvaluation -Value 1 -Type DWord
        "INFO  SmartScreen switched on (policy EnableSmartScreen removed, Explorer SmartScreenEnabled=Warn); was: $(Get-Content $bak)"
    } else {
        $t = (Get-Content $bak -ErrorAction SilentlyContinue); if ($t -match 'policy=(\d+)') { Set-ItemProperty $pol -Name EnableSmartScreen -Value ([int]$Matches[1]) -Type DWord } else { Set-ItemProperty $pol -Name EnableSmartScreen -Value 0 -Type DWord }
        if ($t -match 'explorer=(\w+)') { Set-ItemProperty $ex -Name SmartScreenEnabled -Value $Matches[1] -Type String } else { Remove-ItemProperty $ex -Name SmartScreenEnabled -ErrorAction SilentlyContinue }
        "INFO  SmartScreen restored: $t"
    }
 }
 'uac' {
    $p = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'; $bak = "$work\uac-backup.txt"
    if ($Arg1 -eq 'on') {
        $k = Get-ItemProperty $p; if (-not (Test-Path $bak)) { "$($k.ConsentPromptBehaviorAdmin),$($k.PromptOnSecureDesktop),$($k.EnableLUA)" | Set-Content $bak }
        Set-ItemProperty $p EnableLUA 1 -Type DWord; Set-ItemProperty $p ConsentPromptBehaviorAdmin 2 -Type DWord; Set-ItemProperty $p PromptOnSecureDesktop 1 -Type DWord
        "INFO  UAC switched on (EnableLUA=1, ConsentPromptBehaviorAdmin=2: consent on the secure desktop; needs a restart); was (ConsentPromptBehaviorAdmin,PromptOnSecureDesktop,EnableLUA): $(Get-Content $bak)"
    } else {
        $v = (Get-Content $bak).Split(','); Set-ItemProperty $p ConsentPromptBehaviorAdmin ([int]$v[0]) -Type DWord; Set-ItemProperty $p PromptOnSecureDesktop ([int]$v[1]) -Type DWord; Set-ItemProperty $p EnableLUA ([int]$v[2]) -Type DWord
        "INFO  UAC restored: ConsentPromptBehaviorAdmin=$($v[0]) PromptOnSecureDesktop=$($v[1]) EnableLUA=$($v[2]) (needs a restart)"
    }
 }
 'launch' {
    $v = $Arg1; $d = "$env:USERPROFILE\Downloads\$v"; New-Item -ItemType Directory -Force $d | Out-Null; $f = "$d\VM2Backup-Setup.exe"
    Copy-Item "$loc\$v.exe" $f -Force
    if ($args[2] -ne 'nomotw') {
        Set-Content -Path $f -Stream Zone.Identifier -Value "[ZoneTransfer]`r`nZoneId=3`r`nReferrerUrl=https://example.test/`r`nHostUrl=https://example.test/VM2Backup-Setup.exe"
        "INFO  variant ${v}: MOTW = " + ((Get-Content $f -Stream Zone.Identifier) -join ' ')
    } else { "INFO  variant ${v}: WITHOUT Mark of the Web (as after a copy from a share / after 'Run anyway': SmartScreen does not take part, UAC does)" }
    "INFO  " + (Sig $f)
    Start-Process explorer.exe -ArgumentList "`"$f`""
    "INFO  started like a double click at $(Get-Date -Format HH:mm:ss)"
 }
 'click' {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    # ключевые слова (в cmd нельзя передать кириллицу): more = «Подробнее», run = «Выполнить в любом случае», dont = «Не выполнять»
    $dict = @{ more = @('Подробнее', 'More info'); run = @('Выполнить в любом случае', 'Все равно выполнить', 'Всё равно выполнить', 'Run anyway'); dont = @('Не выполнять', "Don't run"); runfile = @('Запустить', 'Run') }
    $names = $dict[$Arg1]; $limit = if ($args[2]) { [int]$args[2] } else { 20 }; $t0 = Get-Date; $done = $false
    while (-not $done -and ((Get-Date) - $t0).TotalSeconds -lt $limit) {
        $root = [Windows.Automation.AutomationElement]::RootElement
        $all = $root.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)
        foreach ($e in $all) { try { $n = $e.Current.Name } catch { continue }
            if ($n -and ($names -contains $n.Trim()) -and $e.Current.ControlType.ProgrammaticName -match 'Button|Hyperlink|Text') {
                try { $ip = $e.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern); $ip.Invoke(); "INFO  clicked '$n' ($($e.Current.ControlType.ProgrammaticName))"; $done = $true; break } catch { "INFO  found '$n' but cannot invoke: $($_.Exception.Message)" }
            } }
        if (-not $done) { Start-Sleep -Milliseconds 700 }
    }
    if (-not $done) { "INFO  no control '$Arg1' found" }
 }
 'dump' {    # имена элементов окон SmartScreen / установщика (отладка и протокол: что именно видит пользователь)
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $root = [Windows.Automation.AutomationElement]::RootElement
    foreach ($w in $root.FindAll([Windows.Automation.TreeScope]::Children, [Windows.Automation.Condition]::TrueCondition)) {
        try { $pn = (Get-Process -Id $w.Current.ProcessId -ErrorAction Stop).ProcessName } catch { $pn = '?' }
        $wn = $w.Current.Name
        if ($pn -notmatch 'smartscreen|consent|VM2Backup|Setup' -and $wn -notmatch 'SmartScreen|защит|VM2Backup') { continue }
        "WIN '$wn' process=$pn [$($w.Current.ProcessId)]"
        foreach ($e in $w.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition)) { try { "     $($e.Current.ControlType.ProgrammaticName) '$($e.Current.Name)' class=$($e.Current.ClassName) focus=$($e.Current.HasKeyboardFocus)" } catch {} }
    }
 }
 'state' {
    "INFO  processes: " + ((Get-Process | Where-Object { $_.ProcessName -match 'Setup|smartscreen|consent|VM2' } | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', ')
    foreach ($p in Get-Process | Where-Object { $_.ProcessName -match 'VM2Backup' }) { try { "INFO  running file: $($p.Path) :: " + (Sig $p.Path) } catch {} }
 }
 'kill' {
    foreach ($n in 'VM2Backup-Setup', 'VM2Backup', 'smartscreen', 'CHXSmartScreen', 'consent') { Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }
    Remove-Item "$env:USERPROFILE\Downloads\signed", "$env:USERPROFILE\Downloads\unsigned", "$env:USERPROFILE\Downloads\tampered" -Recurse -Force -ErrorAction SilentlyContinue
    Get-ChildItem "$env:TEMP" -Filter 'VM2Backup-Setup-*' -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    "INFO  cleaned"
 }
 'events' {
    $since = (Get-Date).AddMinutes([int]$(if ($Arg1) { -[int]$Arg1 } else { -30 }))
    foreach ($log in 'Microsoft-Windows-CodeIntegrity/Operational', 'Microsoft-Windows-AppLocker/EXE and DLL', 'Microsoft-Windows-Windows Defender/Operational') {
        try { Get-WinEvent -FilterHashtable @{ LogName = $log; StartTime = $since } -MaxEvents 40 -ErrorAction Stop | ForEach-Object { "INFO  [$log] id=$($_.Id) $('{0:HH:mm:ss}' -f $_.TimeCreated): $((($_.Message -split "`r?`n")[0]))" } } catch { "INFO  [$log] no events ($($_.Exception.Message.Split("`n")[0]))" }
    }
 }
}
exit 0
