# web-bind.ps1 <set|reset>: lets the site listen on the guest's real address too (the host curls it through the qemu port forward), or back to localhost only.
param([string]$Mode = 'set')
. C:\vm2b\guest\lib.ps1
$w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; Check 'login' ($r.Status -eq 200) $r.Text
$ifs = Api $w 'web.interfaces.list' @{}
$ip = (Get-WmiObject Win32_NetworkAdapterConfiguration | Where-Object { $_.IPEnabled } | ForEach-Object { $_.IPAddress | Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } } | Select-Object -First 1)
Write-Output "guest address: $ip"
if ($Mode -eq 'set') {
    $s = Api $w 'web.settings.set' @{ bind = @('127.0.0.1', $ip); port = 8443 }
    Write-Output ("web.settings.set: " + $s.Text); Check 'bind to the real address accepted' ($s.Status -eq 200) $s.Text
    Start-Sleep -Seconds 3
    $ws = Api $w 'web.status' @{}; Write-Output ("web.status: " + $ws.Text)
    $w2 = New-Web "https://${ip}:8443"; $me = Web-Call $w2 'GET' '/api/me' $null; Check "the guest reaches its own address $ip" ($me.Status -eq 200) $me.Text
    Set-Content -Path "$Share\results\guest-ip.txt" -Value $ip
    $o = & netstat.exe -ano | Select-String ':8443'; Write-Output ($o -join "`n")
} else {
    $s = Api $w 'web.settings.set' @{ bind = @('127.0.0.1'); port = 8443 }
    Write-Output ("web.settings.set: " + $s.Text); Check 'back to localhost' ($s.Status -eq 200) $s.Text
}
exit $global:Failed
