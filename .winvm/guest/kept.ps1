# kept.ps1 add|list|state : data that must survive an update / a removal (a server saved through the web API)
param([string]$Mode = 'list')
. C:\vm2b\guest\lib.ps1
$w = New-Web 'https://127.0.0.1:8443'
$r = $null; for ($i = 0; $i -lt 90; $i++) { $r = Web-Login $w; if ($r.Status -eq 200) { break }; Start-Sleep -Seconds 1 }
if (-not $r -or $r.Status -ne 200) { Write-Output "login failed: $($r.Status) $($r.Text)"; exit 1 }
if ($Mode -eq 'add') { $s = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'kept server'; path = 'C:\vm2b\jobdata' } }; Write-Output ("saved: " + $s.Text) }
if ($Mode -eq 'state') { $s = Api $w 'state' @{}; Write-Output ("state: " + $s.Text) }
$l = Api $w 'servers.list' @{}; Write-Output ("servers: " + $l.Text)
Check 'the saved server is there' ($l.Text -match 'kept server') $l.Text
exit $global:Failed
