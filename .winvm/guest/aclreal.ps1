# aclreal.ps1 : права папки данных и канала службы от ОБЫЧНОГО пользователя с КИРИЛЛИЧЕСКИМ именем (русская Windows 10 x64).
#   - SDDL папки и файлов (aclcheck.ps1 new): ничего для BU / WD / AU;
#   - пользователь «Юзер-Обычный» состоит в группе «Пользователи» (по SID S-1-5-32-545), НЕ в администраторах: acltest.ps1 от его имени (config.json / web.json не читаются,
#     DPAPI не снять, запись запрещена), pipeclient.ps1 (канал службы закрыт: DENIED);
#   - «Пользователи» по SID могут писать в logs; icacls по SID показывает то же.
. C:\vm2b\guest\lib.ps1
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\aclcheck.ps1 new
$name = -join ([char[]](0x42E, 0x437, 0x435, 0x440, 0x2D, 0x41E, 0x431, 0x44B, 0x447, 0x43D, 0x44B, 0x439)); $pw = 'Cyr-Std-123!x'         # Юзер-Обычный
& net.exe user $name /delete 2>&1 | Out-Null; & net.exe user $name $pw /add /y /expires:never | Out-Null
Check 'a standard user with a Cyrillic name is created' ([bool](Get-LocalUser -Name $name -ErrorAction SilentlyContinue)) ''
Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $name -ErrorAction SilentlyContinue
$inUsers = @(Get-LocalGroupMember -SID 'S-1-5-32-545' -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*\$name" }).Count -gt 0
$inAdm = @(Get-LocalGroupMember -SID 'S-1-5-32-544' -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*\$name" }).Count -gt 0
Check 'the user is in the group with SID S-1-5-32-545 (Users / Пользователи) and not in Administrators' ($inUsers -and -not $inAdm) "users=$inUsers admins=$inAdm"
Write-Output ("INFO groups: " + ((Get-LocalGroup | Where-Object { $_.SID.Value -in 'S-1-5-32-544', 'S-1-5-32-545' } | ForEach-Object { $_.Name }) -join ', '))
$cred = New-Object Management.Automation.PSCredential("$env:COMPUTERNAME\$name", (ConvertTo-SecureString $pw -AsPlainText -Force))
function As-User($script, $out) {
    Remove-Item $out -ErrorAction SilentlyContinue
    try { Start-Process powershell.exe -Credential $cred -LoadUserProfile -WorkingDirectory C:\vm2b -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script, $out -Wait -WindowStyle Hidden -ErrorAction Stop } catch { Write-Output ("START FAILED " + $_.Exception.Message) }
    return $(if (Test-Path $out) { (Get-Content $out) -join ' | ' } else { 'NO RESULT' })
}
$acl = As-User 'C:\vm2b\guest\acltest.ps1' 'C:\vm2b\acl-cyr.txt'; Write-Output "INFO the Cyrillic standard user vs the data folder: $acl"
Check 'the user cannot read config.json' ($acl -match 'READ_DENIED config.json') $acl
Check 'the user cannot read web.json' ($acl -match 'READ_DENIED web.json') $acl
Check 'the user cannot read history.json / state.json' ($acl -notmatch 'READ_OK (history|state).json') $acl
Check 'the user cannot decrypt a machine-scope secret' ($acl -notmatch 'UNPROTECT_OK') $acl
Check 'the user cannot write config.json' ($acl -match 'WRITE_DENIED config.json') $acl
Check 'the user can still use the logs folder (panel)' ($acl -match 'LOGS_LIST_OK') $acl
$pipe = As-User 'C:\vm2b\guest\pipeclient.ps1' 'C:\vm2b\pipe-cyr.txt'; Write-Output "INFO the Cyrillic standard user vs the service pipe: $pipe"
Check 'the user cannot connect to the service pipe (DENIED)' ($pipe -match 'DENIED') $pipe
Write-Output ("INFO icacls by SID: " + ((& icacls.exe "$env:ProgramData\VM2Backup" /findsid '*S-1-5-32-545' 2>&1) -join ' | '))
& net.exe user $name /delete 2>&1 | Out-Null
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
