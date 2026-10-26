# pipeclient.ps1 <result file>: tries to open the service's named pipe as the current user and writes what happened.
param([string]$Out)
$who = [Security.Principal.WindowsIdentity]::GetCurrent().Name
try {
    $p = New-Object IO.Pipes.NamedPipeClientStream('.', 'VM2BackupService', [IO.Pipes.PipeDirection]::InOut)
    $p.Connect(3000); $res = 'CONNECTED'; $p.Close()
} catch [UnauthorizedAccessException] { $res = 'DENIED (access denied)' }
catch { $res = 'ERROR ' + $_.Exception.GetType().Name + ' ' + $_.Exception.Message }
Set-Content -Path $Out -Value "$who : $res"
