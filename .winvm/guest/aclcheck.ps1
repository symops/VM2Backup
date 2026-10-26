# aclcheck.ps1 old|new : права на папку данных C:\ProgramData\VM2Backup в виде SDDL (не зависит от языка Windows: BU = «Пользователи» / Users, WD = все, AU = прошедшие проверку, IU = интерактивные).
#   old : версия 1.0.298 (до исправления): только запись в вывод (INFO), есть ли там открытые для обычных пользователей записи
#   new : после обновления / установки текущей версии: у папки, config.json, web.json, state.json, history.json защищённый DACL без записей для BU / WD / AU / IU / NU / AC;
#         папка logs остаётся доступной обычным пользователям (панель пишет туда журнал)
param([string]$Mode = 'new')
. C:\vm2b\guest\lib.ps1
$data = "$env:ProgramData\VM2Backup"
$wide = 'BU', 'WD', 'AU', 'IU', 'NU', 'AC', 'S-1-5-32-545', 'S-1-1-0', 'S-1-5-11', 'S-1-5-4'
function Open-Aces($p) {
    if (-not (Test-Path $p)) { return @('MISSING') }
    $sd = (Get-Acl -LiteralPath $p).GetSecurityDescriptorSddlForm('Access'); $r = @()
    foreach ($m in [regex]::Matches($sd, '\(([^)]*)\)')) { $f = $m.Groups[1].Value -split ';'; if ($f[0] -eq 'A' -and ($wide -contains $f[5])) { $r += $m.Groups[1].Value } }
    return $r
}
function Sddl($p) { if (Test-Path $p) { (Get-Acl -LiteralPath $p).GetSecurityDescriptorSddlForm('Access') } else { 'MISSING' } }
foreach ($n in '', 'config.json', 'web.json', 'state.json', 'history.json', 'logs') {
    $p = if ($n) { "$data\$n" } else { $data }; $open = @(Open-Aces $p)
    Write-Output ("INFO [{0}] {1}: {2}" -f $Mode, $(if ($n) { $n } else { '<data folder>' }), (Sddl $p))
    if ($Mode -eq 'new') {
        if ($n -eq 'logs') { Check 'the logs folder is writable for ordinary users (panel logs)' ($open.Count -ge 1 -and $open[0] -ne 'MISSING') ($open -join ' ') }
        elseif ($n -eq 'web.json' -or $n -eq 'state.json' -or $n -eq 'history.json') { if (Test-Path $p) { Check "$n has no ACE for Users / Everyone / Authenticated Users" ($open.Count -eq 0) ($open -join ' ') } }
        else { Check ("{0}: no ACE for Users / Everyone / Authenticated Users" -f $(if ($n) { $n } else { 'the data folder' })) ($open.Count -eq 0) ($open -join ' ') }
    } else { Write-Output ("INFO [old] {0}: ACEs open to ordinary users: {1}" -f $(if ($n) { $n } else { 'data folder' }), $(if ($open.Count) { $open -join ' ' } else { 'none' })) }
}
if ($Mode -eq 'new') { $d = (Get-Acl -LiteralPath $data).GetSecurityDescriptorSddlForm('Access'); Check 'the data folder DACL is protected (no inheritance from ProgramData)' ($d -match '^D:P') $d }
exit $global:Failed
