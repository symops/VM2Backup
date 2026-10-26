# dpapi.ps1 create <unc> <user> <password> | check   : a server with a password is saved through the web API; config.json must not hold it in clear;
# after a service restart / a reboot the stored secret must still work (servers.test against the real share).
param([string]$Mode, [string]$Unc, [string]$User, [string]$Pw)
. C:\vm2b\guest\lib.ps1
$cfgPath = "$env:ProgramData\VM2Backup\config.json"
$w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; Check 'login' ($r.Status -eq 200) $r.Text
if ($Mode -eq 'create') {
    $old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq 'dpapi share') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'smb'; name = 'dpapi share'; path = $Unc; user = $User; password = $Pw } }
    Check 'servers.save smb with a password' ($sv.Status -eq 200 -and $sv.Result.id) $sv.Text
    Set-Content -Path C:\vm2b\dpapi.id -Value $sv.Result.id
    Set-Content -Path C:\vm2b\dpapi.pw -Value $Pw
    $t = Api $w 'servers.test' @{ id = $sv.Result.id }; Write-Output ("servers.test: " + $t.Text); Check 'the server works right after saving' ($t.Status -eq 200) $t.Text
}
else {
    $id = (Get-Content C:\vm2b\dpapi.id).Trim()
    $t = Api $w 'servers.test' @{ id = $id }; Write-Output ("servers.test: " + $t.Text); Check 'the stored secret still works (DPAPI unprotect after the restart)' ($t.Status -eq 200) $t.Text
}
$pwSaved = (Get-Content C:\vm2b\dpapi.pw).Trim()
$cfg = Get-Content -Raw $cfgPath
Check 'config.json does not contain the password in clear' (-not $cfg.Contains($pwSaved)) 'password found in clear text'
Check 'config.json does not contain it as UTF-16 / base64 either' (-not $cfg.Contains([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pwSaved)))) 'base64 of the password found'
$sl = Api $w 'servers.list' @{}; Check 'servers.list does not return the password' (-not $sl.Text.Contains($pwSaved)) $sl.Text
$log = (Get-ChildItem "$env:ProgramData\VM2Backup" -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -lt 20MB -and $_.Name -ne 'config.json' } | ForEach-Object { Get-Content -Raw $_.FullName -ErrorAction SilentlyContinue }) -join "`n"
Check 'no other file of the data folder contains the password' (-not $log.Contains($pwSaved)) 'found in a log / history file'
Write-Output ("secret field in config.json: " + ([regex]::Match($cfg, '"password"\s*:\s*"[^"]{0,60}')).Value)
exit $global:Failed
