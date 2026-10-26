# unicode-user.ps1: a local account with a CYRILLIC name (its profile path C:\Users\<name> is non-ASCII): the native autotests (core) and the panel's
# /selftest run as that user with a loaded profile.  (Russian Windows: the Administrators group is 'Администраторы'; the pipe ACL uses SIDs, checked in pipeacl.ps1.)
. C:\vm2b\guest\lib.ps1
$name = -join ([char[]](0x42E, 0x437, 0x435, 0x440, 0x2D, 0x422, 0x435, 0x441, 0x442))      # Юзер-Тест
$pw = 'Cyr-Pass-123!x'
& net.exe user $name /delete 2>$null | Out-Null
& net.exe user $name $pw /add /y | Out-Null
# Проверка через .NET, а не по тексту net user: консоль английской Windows (OEM 437) кириллицу не выводит ("????-????"), и проверка по тексту давала ложное FAIL
Check 'Cyrillic user created' ([bool](Get-LocalUser -Name $name -ErrorAction SilentlyContinue)) ''
& net.exe localgroup (-join ([char[]](0x410,0x434,0x43C,0x438,0x43D,0x438,0x441,0x442,0x440,0x430,0x442,0x43E,0x440,0x44B))) $name /add 2>&1 | Out-Null     # Администраторы
$sec = ConvertTo-SecureString $pw -AsPlainText -Force; $cred = New-Object Management.Automation.PSCredential("$env:COMPUTERNAME\$name", $sec)
New-Item -ItemType Directory -Force C:\vm2b\ulogs | Out-Null; icacls C:\vm2b\ulogs /grant '*S-1-1-0:(OI)(CI)F' | Out-Null
function RunAs($exe, $args2, $tag) {
    $o = "C:\vm2b\ulogs\$tag.txt"; Remove-Item $o -ErrorAction SilentlyContinue
    try { $p = Start-Process $exe -ArgumentList $args2 -Credential $cred -LoadUserProfile -WorkingDirectory C:\vm2b\t -RedirectStandardOutput $o -RedirectStandardError "$o.err" -Wait -PassThru -WindowStyle Hidden -ErrorAction Stop; return $p.ExitCode }
    catch { Write-Output ("START FAILED " + $_.Exception.Message); return -1 }
}
$rc = RunAs 'C:\vm2b\t\vm2bautotest.exe' '--seed 4242 --only core' 'unicode-core'
Write-Output ("autotest as the Cyrillic user: rc=$rc"); Get-Content C:\vm2b\ulogs\unicode-core.txt -ErrorAction SilentlyContinue | Select-String '^FAIL|passed,' | ForEach-Object { Write-Output $_.Line }
Check 'native autotests (core) pass under a Cyrillic profile' ($rc -eq 0) "rc=$rc"
Write-Output ("profile dir: " + ((Get-ChildItem C:\Users -Directory | Where-Object { $_.Name -match '[^\x00-\x7F]' } | ForEach-Object { $_.FullName }) -join ', '))
Copy-Item C:\vm2b\ulogs\unicode-core.txt "$Share\results\autotest-unicode.txt" -ErrorAction SilentlyContinue
& net.exe user $name /delete 2>&1 | Out-Null
exit $global:Failed
