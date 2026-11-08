# instacl.ps1 : (F1 ревью безопасности) установка в каталог ВНЕ Program Files получает защищённый DACL.
#   * C:\VM2B-acltest создаётся заранее и наследует права корня тома (Прошедшие проверку: изменение) - это и есть опасное исходное состояние;
#   * после установки (/dir C:\VM2B-acltest): DACL каталога и exe защищённый, у «Пользователи» только чтение и выполнение, у «Прошедшие проверку» / «Все» ничего;
#     обычный пользователь не может записать, переименовать, заменить или удалить программы и положить в каталог файл; чтение и запуск разрешены;
#   * служба зарегистрирована из этого каталога и работает;
#   * обновление поверх (после того как права намеренно испорчены icacls) снова закрывает каталог; смена каталога (C:\VM2B-acltest2) переносит установку,
#     старый каталог убирается; удаление (/remove) работает; в конце ставится пакет в обычное место для следующих фаз.
# Запускается фазой installer (tools/run-winvm.sh), после uninstall-update.cmd. ПОКА НЕ ПРОГОНЯЛСЯ на настоящей Windows (docs/WINDOWS-TESTS.md).
. C:\vm2b\guest\lib.ps1
$setup = 'C:\vm2b\setup\VM2Backup-Setup.exe'
$d1 = 'C:\VM2B-acltest'; $d2 = 'C:\VM2B-acltest2'
$name = 'vm2bacl'; $pw = 'Acl-Std-123!x'
$wide = 'BU', 'WD', 'AU', 'IU', 'NU', 'AC', 'S-1-5-32-545', 'S-1-1-0', 'S-1-5-11'
$writeMask = 0x2 -bor 0x4 -bor 0x10 -bor 0x100 -bor 0x40 -bor 0x10000 -bor 0x40000 -bor 0x80000 -bor 0x40000000 -bor 0x10000000 -bor 0x20000000   # write, append, EA, attrs, delete child, delete, DACL, owner, generic write / all / execute-all

function Sddl($p) { if (Test-Path -LiteralPath $p) { (Get-Acl -LiteralPath $p).GetSecurityDescriptorSddlForm('Access, Owner') } else { 'MISSING' } }
# the ACEs for ordinary users that can change things: any allow ACE for a wide SID with a write bit; the SDDL mask may be a hex number or letters
function Risky-Aces($p) {
    $sd = (Get-Acl -LiteralPath $p).GetSecurityDescriptorSddlForm('Access'); $r = @()
    foreach ($m in [regex]::Matches($sd, '\(([^)]*)\)')) {
        $f = $m.Groups[1].Value -split ';'
        if ($f[0] -ne 'A' -or -not ($wide -contains $f[5])) { continue }
        $rights = $f[2]; $bad = $false
        if ($rights -match '^0x[0-9a-fA-F]+$') { $bad = (([Convert]::ToInt64($rights, 16)) -band $writeMask) -ne 0 }
        else { foreach ($t in 'GA', 'GW', 'FA', 'WD', 'AD', 'WA', 'WE', 'DC', 'SD', 'WO') { if ($rights -match $t) { $bad = $true } } }
        if ($bad) { $r += $m.Groups[1].Value }
    }
    return $r
}
function Install-To($dir) { Start-Process -FilePath $setup -ArgumentList '/install', '/silent', '/dir', "`"$dir`"" -Wait -PassThru | ForEach-Object { $_.ExitCode } }
function Svc-Path() { (& sc.exe qc VM2BackupService 2>&1) -join ' ' }
function As-User($script, $out, $dir) {
    Remove-Item $out -ErrorAction SilentlyContinue
    try { Start-Process powershell.exe -Credential $cred -LoadUserProfile -WorkingDirectory C:\vm2b -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, $out, $dir -Wait -WindowStyle Hidden -ErrorAction Stop } catch { Write-Output ("START FAILED " + $_.Exception.Message) }
    return $(if (Test-Path $out) { (Get-Content $out) -join ' | ' } else { 'NO RESULT' })
}

& net.exe user $name /delete 2>&1 | Out-Null; & net.exe user $name $pw /add /y /expires:never | Out-Null
Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $name -ErrorAction SilentlyContinue
$cred = New-Object Management.Automation.PSCredential("$env:COMPUTERNAME\$name", (ConvertTo-SecureString $pw -AsPlainText -Force))
Check 'a standard user is created for the installer ACL test' ([bool](Get-LocalUser -Name $name -ErrorAction SilentlyContinue)) ''

# ---- 1. the dangerous starting point: a folder under the volume root that ordinary users may change
Remove-Item -LiteralPath $d1, $d2 -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $d1 | Out-Null
$pre = Risky-Aces $d1
Write-Output ("INFO before the install: " + (Sddl $d1))
Check 'precondition: the new folder under C:\ is open for writing to ordinary users (inherited from the volume root)' ($pre.Count -ge 1) (Sddl $d1)

# ---- 2. install into it
$rc = Install-To $d1; Write-Output "INFO install rc=$rc"
Check 'the installer ends with 0 for a folder outside Program Files' ($rc -eq 0) "rc=$rc"
Start-Sleep -Seconds 5
$files = 'VM2Backup.exe', 'VM2BackupService.exe', 'VM2Backup-Setup.exe', 'uninstall.cmd'
Check 'all files are in the folder' (@($files | Where-Object { Test-Path -LiteralPath "$d1\$_" }).Count -eq 4) ((Get-ChildItem $d1 -Name) -join ',')
Write-Output ("INFO folder: " + (Sddl $d1)); foreach ($f in $files) { Write-Output ("INFO ${f}: " + (Sddl "$d1\$f")) }
$sd1 = Sddl $d1
Check 'the folder DACL is protected (no inheritance from C:\)' ($sd1 -match 'D:P') $sd1
Check 'the folder: no write for Users / Everyone / Authenticated Users' ((Risky-Aces $d1).Count -eq 0) ((Risky-Aces $d1) -join ' ')
Check 'the folder: Users have read and execute (the panel runs for them)' ($sd1 -match '\(A;[A-Z]*;(0x1200a9|GRGX|FRFX);;;BU\)') $sd1
Check 'the folder: SYSTEM and Administrators have full access' (($sd1 -match '\(A;[A-Z]*;FA;;;SY\)') -and ($sd1 -match '\(A;[A-Z]*;FA;;;BA\)')) $sd1
Check 'the folder owner is Administrators or SYSTEM' ($sd1 -match '^O:(BA|SY)') $sd1
foreach ($f in $files) { $p = "$d1\$f"; Check "$f: protected DACL without write for ordinary users" (((Sddl $p) -match 'D:P') -and ((Risky-Aces $p).Count -eq 0)) (Sddl $p) }
Check 'the service is registered from this folder' ((Svc-Path) -match [regex]::Escape($d1)) (Svc-Path)
Check 'the service is running' ((& sc.exe query VM2BackupService 2>&1) -join ' ' -match 'RUNNING') ''

# ---- 3. an ordinary user cannot replace the program that SYSTEM runs
$u = As-User 'C:\vm2b\guest\instacl-user.ps1' 'C:\vm2b\instacl-user.txt' $d1
Write-Output "INFO the standard user vs the install folder: $u"
foreach ($t in 'WRITE_DENIED VM2BackupService.exe', 'WRITE_DENIED VM2Backup.exe', 'CREATE_DENIED planted.dll', 'RENAME_DENIED VM2BackupService.exe', 'DELETE_DENIED VM2Backup-Setup.exe', 'REPLACE_DENIED VM2BackupService.exe') { Check "user: $t" ($u -match [regex]::Escape($t)) $u }
Check 'user: can still read and list the folder' (($u -match 'READ_OK VM2BackupService.exe') -and ($u -match 'LIST_OK')) $u
Check 'nothing was planted' (-not (Test-Path -LiteralPath "$d1\planted.dll")) ''

# ---- 4. update over the existing installation after the rights were spoiled on purpose (the old behaviour: inherited Modify)
& icacls.exe $d1 /inheritance:e /grant '*S-1-5-11:(OI)(CI)M' | Out-Null
& icacls.exe "$d1\VM2BackupService.exe" /inheritance:e /grant '*S-1-5-11:M' | Out-Null
Check 'precondition of the update: the rights are open again' ((Risky-Aces $d1).Count -ge 1 -and (Risky-Aces "$d1\VM2BackupService.exe").Count -ge 1) (Sddl $d1)
$rc = Install-To $d1; Start-Sleep -Seconds 5
Check 'the update over the existing folder ends with 0' ($rc -eq 0) "rc=$rc"
Check 'after the update: the folder is closed again' (((Risky-Aces $d1).Count -eq 0) -and ((Sddl $d1) -match 'D:P')) (Sddl $d1)
Check 'after the update: the service exe is closed again' (((Risky-Aces "$d1\VM2BackupService.exe").Count -eq 0) -and ((Sddl "$d1\VM2BackupService.exe") -match 'D:P')) (Sddl "$d1\VM2BackupService.exe")
Check 'after the update: the service runs' ((& sc.exe query VM2BackupService 2>&1) -join ' ' -match 'RUNNING') ''

# ---- 5. a changed folder: the installation moves, the new folder is closed, the old one is gone
$rc = Install-To $d2; Start-Sleep -Seconds 5
Check 'the install into another folder ends with 0' ($rc -eq 0) "rc=$rc"
Check 'the new folder is closed (protected, no write for ordinary users)' (((Risky-Aces $d2).Count -eq 0) -and ((Sddl $d2) -match 'D:P')) (Sddl $d2)
Check 'the service now starts from the new folder' ((Svc-Path) -match [regex]::Escape($d2)) (Svc-Path)
Check 'the old folder lost the program files' (-not (Test-Path -LiteralPath "$d1\VM2BackupService.exe")) ((Get-ChildItem $d1 -Name -ErrorAction SilentlyContinue) -join ',')

# ---- 6. removal works with the protected folder
$p = Start-Process -FilePath "$d2\VM2Backup-Setup.exe" -ArgumentList '/remove', '/silent' -Wait -PassThru
Start-Sleep -Seconds 6
Check 'the removal (uninstall) from the protected folder ends with 0' ($p.ExitCode -eq 0) "rc=$($p.ExitCode)"
Check 'the service is gone' (-not (((& sc.exe query VM2BackupService 2>&1) -join ' ') -match 'SERVICE_NAME')) ''
Check 'the program files are gone' (-not (Test-Path -LiteralPath "$d2\VM2BackupService.exe")) ((Get-ChildItem $d2 -Name -ErrorAction SilentlyContinue) -join ',')

# ---- the state for the following phases: the package in the usual place
Remove-Item -LiteralPath $d1, $d2 -Recurse -Force -ErrorAction SilentlyContinue
& net.exe user $name /delete 2>&1 | Out-Null
$rc = (Start-Process -FilePath $setup -ArgumentList '/install', '/silent' -Wait -PassThru).ExitCode
Check 'the package is installed again in the usual place for the next phases' ($rc -eq 0) "rc=$rc"
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
