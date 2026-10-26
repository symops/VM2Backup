# VM2Backup guest test library (dot-source it):  . C:\vm2b\guest\lib.ps1
$ErrorActionPreference = 'Continue'
$global:Share = if ($env:SRC) { $env:SRC } else { '\\10.0.2.4\qemu' }
function Out-Line($s) { Write-Output $s }
function Check($name, $ok, $detail) { if ($ok) { Write-Output ("PASS  " + $name) } else { Write-Output ("FAIL  " + $name + "  :: " + $detail); $global:Failed++ } }
$global:Failed = 0
Add-Type @"
using System.Net; using System.Security.Cryptography.X509Certificates;
public class TrustAllPolicy : ICertificatePolicy { public bool CheckValidationResult(ServicePoint sp, X509Certificate c, WebRequest r, int p) { return true; } }
"@ -ErrorAction SilentlyContinue
[System.Net.ServicePointManager]::CertificatePolicy = New-Object TrustAllPolicy
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

function New-Web([string]$base) { $w = New-Object psobject -Property @{ Base = $base; Cookies = (New-Object System.Net.CookieContainer); Csrf = ''; Role = '' }; return $w }
# returns @{Status=..; Json=..; Text=..}
function Web-Call($w, [string]$method, [string]$path, $body, [bool]$useCsrf = $true) {
    $req = [System.Net.HttpWebRequest]::Create($w.Base + $path)
    $req.Method = $method; $req.CookieContainer = $w.Cookies; $req.Timeout = 60000; $req.ReadWriteTimeout = 60000
    if ($body -ne $null) {
        $json = if ($body -is [string]) { $body } else { ConvertTo-Json $body -Depth 12 -Compress }
        $bytes = [Text.Encoding]::UTF8.GetBytes($json); $req.ContentType = 'application/json'; $req.ContentLength = $bytes.Length
        if ($useCsrf -and $w.Csrf) { $req.Headers.Add('X-CSRF-Token', $w.Csrf) }
        $s = $req.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
    }
    try { $resp = $req.GetResponse() } catch [System.Net.WebException] { $resp = $_.Exception.Response; if (-not $resp) { return @{ Status = 0; Text = $_.Exception.Message; Json = $null } } }
    $sr = New-Object IO.StreamReader($resp.GetResponseStream()); $text = $sr.ReadToEnd(); $sr.Close(); $st = [int]$resp.StatusCode; $resp.Close()
    $j = $null; try { $j = $text | ConvertFrom-Json } catch {}
    $res = $null; if ($j -ne $null -and ($j.PSObject.Properties.Name -contains 'result')) { $res = $j.result }
    return @{ Status = $st; Text = $text; Json = $j; Result = $res }
}
function Web-Login($w, [string]$user = 'admin', [string]$pw = 'admin') {
    $r = Web-Call $w 'POST' '/api/login' @{ user = $user; password = $pw } $false
    if ($r.Status -eq 200 -and $r.Json.ok) { $w.Csrf = $r.Json.csrf; $w.Role = $r.Json.role }
    return $r
}
function Api($w, [string]$cmd, $payload = @{}) { return Web-Call $w 'POST' ('/api/' + $cmd) $payload $true }
function Take-Screenshot([string]$path) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
