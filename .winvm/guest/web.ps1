# The web site on the real Windows stack: TLS from Windows' own clients, login, session, RPC, adapters.
. C:\vm2b\guest\lib.ps1
$base = 'https://127.0.0.1:8443'
Write-Output "==== SslStream (.NET / Schannel) per protocol"
foreach ($p in @(@('Tls', 192), @('Tls11', 768), @('Tls12', 3072), @('Tls13', 12288))) {
    $name = $p[0]; $val = [Security.Authentication.SslProtocols]$p[1]
    try {
        $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 8443); $ssl = New-Object Net.Security.SslStream($c.GetStream(), $false, ([Net.Security.RemoteCertificateValidationCallback]{ $true }))
        $ssl.AuthenticateAsClient('localhost', $null, $val, $false)
        Write-Output ("{0,-6} negotiated {1}  cipher {2} {3}  hash {4}  kx {5}" -f $name, $ssl.SslProtocol, $ssl.CipherAlgorithm, $ssl.CipherStrength, $ssl.HashAlgorithm, $ssl.KeyExchangeAlgorithm)
        if ($name -eq 'Tls12') { Check 'TLS 1.2 handshake from .NET / Schannel' ($ssl.SslProtocol.ToString() -match 'Tls12') $ssl.SslProtocol }
        $ssl.Close(); $c.Close()
    } catch { Write-Output ("{0,-6} FAILED: {1}" -f $name, $_.Exception.InnerException.Message + $_.Exception.Message); if ($name -eq 'Tls12') { Check 'TLS 1.2 handshake from .NET / Schannel' $false $_.Exception.Message } }
}
Write-Output "==== curl.exe (Schannel)"
foreach ($v in @('', '--tlsv1.2 --tls-max 1.2', '--tlsv1.3', '--tlsv1.0 --tls-max 1.1')) {
    $o = & cmd.exe /c "curl.exe -k -s -S -v $v -o NUL -w ""HTTP %{http_code}"" https://127.0.0.1:8443/ 2>&1"
    $line = ($o | Select-String 'HTTP \d+|schannel: .*(failed|error)|SEC_E_|curl: \(|TLS|cipher' | Select-Object -First 4) -join ' | '
    Write-Output ("curl [{0}] -> {1}" -f $v, $line)
}
$o = & cmd.exe /c "curl.exe -k -s -S -o NUL -w %{http_code} https://127.0.0.1:8443/ 2>&1"
Check 'curl.exe -k GET / returns 200' ($o -eq '200') $o
Write-Output "==== TLS 1.3 only (Windows 11 / Server 2022 and newer: Schannel speaks TLS 1.3; older builds: informational)"
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber; $need13 = $build -ge 22000
function Check13($name, $ok, $detail) { if ($need13) { Check $name $ok $detail } else { Write-Output ("INFO  (build $build, TLS 1.3 not required) " + $name + ": " + $(if ($ok) { 'works' } else { 'does not work: ' + $detail })) } }
$o = & cmd.exe /c "curl.exe -k -s -S -v --tlsv1.3 --tls-max 1.3 -o NUL -w ""HTTP %{http_code}"" https://127.0.0.1:8443/api/me 2>&1"
Write-Output ("curl --tlsv1.3 --tls-max 1.3: " + (($o | Select-String 'HTTP \d+|SEC_E_|TLS|curl: \(' | Select-Object -First 4) -join ' | '))
Check13 'curl.exe --tlsv1.3 --tls-max 1.3 reaches the site (HTTP 200)' (($o -join ' ') -match 'HTTP 200') ($o -join ' ')
try {
    $c = New-Object Net.Sockets.TcpClient('127.0.0.1', 8443); $ssl = New-Object Net.Security.SslStream($c.GetStream(), $false, ([Net.Security.RemoteCertificateValidationCallback]{ $true }))
    $ssl.AuthenticateAsClient('localhost', $null, [Security.Authentication.SslProtocols]12288, $false)
    Check13 'SslStream restricted to TLS 1.3 negotiates Tls13' ($ssl.SslProtocol.ToString() -match 'Tls13') $ssl.SslProtocol
    $ssl.Close(); $c.Close()
} catch { Check13 'SslStream restricted to TLS 1.3 negotiates Tls13' $false ($_.Exception.Message + ' ' + $_.Exception.InnerException.Message) }
# PowerShell: Invoke-WebRequest with the protocol set to TLS 1.3 only (the process-wide setting of ServicePointManager)
$save = [Net.ServicePointManager]::SecurityProtocol
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]12288; $r = Invoke-WebRequest -UseBasicParsing -Uri "$base/api/me" -TimeoutSec 20; Check13 'PowerShell Invoke-WebRequest with SecurityProtocol = Tls13 only -> 200' ($r.StatusCode -eq 200) $r.StatusCode }
catch { Check13 'PowerShell Invoke-WebRequest with SecurityProtocol = Tls13 only -> 200' $false ($_.Exception.Message) }
finally { [Net.ServicePointManager]::SecurityProtocol = $save }
# HttpClient (.NET Framework 4.x): the same, and a client that offers only TLS 1.2 still works (both versions are served at once)
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]3072; $r = Invoke-WebRequest -UseBasicParsing -Uri "$base/api/me" -TimeoutSec 20; Check 'client limited to TLS 1.2 still gets 200 next to TLS 1.3' ($r.StatusCode -eq 200) $r.StatusCode }
catch { Check 'client limited to TLS 1.2 still gets 200 next to TLS 1.3' $false $_.Exception.Message }
finally { [Net.ServicePointManager]::SecurityProtocol = $save }
Write-Output "==== Invoke-WebRequest"
try { $r = Invoke-WebRequest -UseBasicParsing -Uri "$base/" -TimeoutSec 20; Check 'Invoke-WebRequest / -> 200 + CSP header' ($r.StatusCode -eq 200 -and $r.Headers['Content-Security-Policy']) ($r.StatusCode); Write-Output ($r.Headers.GetEnumerator() | ForEach-Object { "$($_.Key): $($_.Value)" }) } catch { Check 'Invoke-WebRequest /' $false $_.Exception.Message }
Write-Output "==== HttpClient"
try {
    Add-Type -AssemblyName System.Net.Http
    $h = New-Object System.Net.Http.HttpClientHandler; $h.ServerCertificateCustomValidationCallback = $null
    $hc = New-Object System.Net.Http.HttpClient($h); $hc.Timeout = [TimeSpan]::FromSeconds(20)
    $resp = $hc.GetAsync("$base/api/me").Result; $txt = $resp.Content.ReadAsStringAsync().Result
    Check 'HttpClient GET /api/me -> 200 (policy trust-all)' ([int]$resp.StatusCode -eq 200) ("$($resp.StatusCode) $txt")
} catch { Check 'HttpClient' $false $_.Exception.Message }
Write-Output "==== login / session / RPC"
$w = New-Web $base
$me = Web-Call $w 'GET' '/api/me' $null; Check '/api/me says signed out' ($me.Json.authenticated -eq $false) $me.Text
$r = Web-Call $w 'POST' '/api/login' @{ user = 'admin'; password = 'wrong-password' } $false; Check 'wrong password -> 401' ($r.Status -eq 401) ("$($r.Status) $($r.Text)")
Start-Sleep -Seconds 3      # the failed login above starts the documented 1 s throttle
$w = New-Web $base; $r = Web-Login $w; Check 'login admin/admin' ($r.Status -eq 200 -and $r.Json.ok -and $r.Json.role -eq 'admin') $r.Text
$me = Web-Call $w 'GET' '/api/me' $null; Check '/api/me authenticated as admin' ($me.Json.authenticated -eq $true) $me.Text
$st = Api $w 'state' @{}; Check 'state RPC' ($st.Status -eq 200 -and $st.Result.version) $st.Text
Write-Output ("state: " + $st.Text)
$nocsrf = Web-Call $w 'POST' '/api/state' @{} $false; Check 'no CSRF token -> 403' ($nocsrf.Status -eq 403) $nocsrf.Status
$ifs = Api $w 'web.interfaces.list' @{}; Write-Output ("interfaces: " + $ifs.Text)
Check 'web.interfaces.list lists adapters with addresses' ($ifs.Status -eq 200 -and $ifs.Text -match '\d+\.\d+\.\d+\.\d+') $ifs.Text
$real = (Get-WmiObject Win32_NetworkAdapterConfiguration | Where-Object { $_.IPEnabled } | ForEach-Object { $_.IPAddress | Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' } })
Write-Output ("Windows says the adapter addresses are: " + ($real -join ','))
foreach ($ip in $real) { Check "interface list contains the real address $ip" ($ifs.Text.Contains($ip)) $ifs.Text }
$ws = Api $w 'web.status' @{}; Write-Output ("web.status: " + $ws.Text)
Check 'web.status has a certificate fingerprint' ($ws.Text -match 'sha256|fingerprint') $ws.Text
$ac = Api $w 'web.audit.tail' @{ lines = 10 }; Write-Output ("audit tail: " + $ac.Text)
$bye = Api $w 'logout' @{}; $st2 = Api $w 'state' @{}; Check 'after logout the session is dead' ($st2.Status -eq 401) $st2.Status
Write-Output ("FAILED=" + $global:Failed)
exit $global:Failed
