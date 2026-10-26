# smbsign.ps1 on|off <unc> <user> <pw> <control host:port> : подпись SMB ОБЯЗАТЕЛЬНА с обеих сторон (Samba: server signing = mandatory; Windows: RequireSecuritySignature).
# Копия / проверка / восстановление через неё делаются отдельным заданием (autotest smbreal); здесь - записать, что показывает Windows, и проверить службу.
param([string]$Mode, [string]$Unc, [string]$User, [string]$Pw, [string]$Ctl = '10.0.2.2:4456')   # Mode: on (включить и проверить) | off (негативный случай и возврат)
. C:\vm2b\guest\lib.ps1
function Control($cmd) { $h, $p = $Ctl.Split(':'); $c = New-Object Net.Sockets.TcpClient; $c.Connect($h, [int]$p); $s = $c.GetStream(); $b = [Text.Encoding]::ASCII.GetBytes("$cmd`n"); $s.Write($b, 0, $b.Length); $r = New-Object IO.StreamReader($s); $t = $r.ReadLine(); $c.Close(); return $t }
function Drop-Connections { & cmd.exe /c 'net use * /delete /y' 2>&1 | Out-Null; Start-Sleep -Seconds 4 }
if ($Mode -eq 'on') {
$cl = Get-SmbClientConfiguration; Write-Output ("INFO client before: RequireSecuritySignature=" + $cl.RequireSecuritySignature + " EnableSecuritySignature=" + $cl.EnableSecuritySignature)
Write-Output ("INFO samba: " + (Control 'signing on'))
Set-SmbClientConfiguration -RequireSecuritySignature $true -Force; Drop-Connections
Check 'the client now requires signing' ((Get-SmbClientConfiguration).RequireSecuritySignature) ''
$o = & cmd.exe /c "net use Y: $Unc `"$Pw`" /user:$User /persistent:no 2>&1"; Write-Output "net use: $o"
Set-Content -Path 'Y:\signing-test.txt' -Value 'signed write' -ErrorAction Continue
Check 'a file can be written over the signed session' ((Get-Content 'Y:\signing-test.txt' -ErrorAction SilentlyContinue) -eq 'signed write') ''
$conn = Get-SmbConnection | Where-Object { $_.ShareName -eq 'vm2b' } | Select-Object -First 1
Write-Output ((Get-SmbConnection | Select-Object ServerName, ShareName, UserName, Dialect, Signed, Encrypted | Format-Table -AutoSize | Out-String -Width 200))
Check 'Get-SmbConnection: the session is Signed' ($conn -and $conn.Signed) ($conn | Out-String)
Write-Output ("INFO dialect " + $conn.Dialect)
Remove-Item 'Y:\signing-test.txt' -ErrorAction SilentlyContinue; & cmd.exe /c 'net use Y: /delete /y' 2>&1 | Out-Null
# служба (если установлена): сервер с паролем проходит servers.test по подписанной сессии
$w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w
if ($r.Status -eq 200) {
    $old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq 'signing share') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'signing share'; path = $Unc; user = $User; password = $Pw } }
    $t = Api $w 'servers.test' @{ id = $sv.Result.id }; Check 'service: servers.test over the signed session' ($t.Status -eq 200) $t.Text; Write-Output ("servers.test: " + $t.Text)
    Api $w 'servers.delete' @{ id = $sv.Result.id } | Out-Null
} else { Write-Output 'INFO service web site not reachable: servers.test skipped' }
} else {
$w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w
# сервер, который подпись не поддерживает (Samba: disabled), при требовании подписи у клиента: что видит пользователь
Write-Output ("INFO samba: " + (Control 'signing disabled')); Drop-Connections
$o = & cmd.exe /c "net use Y: $Unc `"$Pw`" /user:$User /persistent:no 2>&1"; Write-Output "INFO net use against a server that does not sign (client requires): $o"
& cmd.exe /c 'net use Y: /delete /y' 2>&1 | Out-Null
if ($r.Status -eq 200) {
    $sv = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'signing share 2'; path = $Unc; user = $User; password = $Pw } }
    $t = Api $w 'servers.test' @{ id = $sv.Result.id }; Write-Output ("INFO servers.test against a server that cannot sign: status " + $t.Status + " " + $t.Text)
    Check 'no secret in that message' (-not $t.Text.Contains($Pw)) $t.Text
    Api $w 'servers.delete' @{ id = $sv.Result.id } | Out-Null
}
# обратно
Write-Output ("INFO samba: " + (Control 'signing off')); Set-SmbClientConfiguration -RequireSecuritySignature $false -Force; Drop-Connections
Check 'signing requirement removed again' (-not (Get-SmbClientConfiguration).RequireSecuritySignature) ''
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
}
