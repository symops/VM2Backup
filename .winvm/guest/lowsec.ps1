# lowsec.ps1 <start|finish> (ревью безопасности F9, ОБЯЗАТЕЛЬНО на настоящей Windows: Wine не принуждает FILE_FLAG_FIRST_PIPE_INSTANCE):
#   start:  служба остановлена; другая программа (powershell, Everyone - полный доступ) занимает имя канала VM2BackupService; служба ЗАПУСКАЕТСЯ и остаётся в Running
#           (раньше «cannot create the pipe ... is the service already running?» и остановка навсегда), сайт на 127.0.0.1:8443 работает.
#           Дальше хост запускает vm2bautotest --only pipe_default_name_client_checks_the_service с VM2B_SQUAT=1: клиент канала отказывается («not the VM2Backup service»): теперь тест ещё проверяет, что отказ - отдельный вид ошибки
#           IPC_WRONG_SERVER с текстом «wrong server: ...» (а не «служба не запущена»), по которому панель VM2Backup.exe показывает «Blocked: the control channel is held
#           by another program» с именем программы и номером процесса вместо «Stopped» (ожидание обновлено 18.11.2026, прогнано на настоящей Windows 10 x86 в раунде 17; панель под Wine проверяет guitest-сценарий wrong_server).
#   finish: занимавшая программа убита; служба сама занимает имя канала (до 60 с), настоящий клиент (тот же тест без VM2B_SQUAT) проходит.
param([string]$Mode = 'start')
. C:\vm2b\guest\lib.ps1
$svc = 'VM2BackupService'; $pidFile = 'C:\vm2b\lowsec-squat.pid'
function Pipe-Exists { [bool](@([IO.Directory]::GetFiles('\\.\pipe\')) -contains '\\.\pipe\VM2BackupService') }      # раунд 16: Test-Path по каналу ненадёжен
function Svc-Status { $s = Get-Service $svc -ErrorAction SilentlyContinue; if ($s) { [string]$s.Status } else { 'none' } }
function Wait-Svc($want, $sec) { for ($i = 0; $i -lt $sec; $i++) { if ((Svc-Status) -eq $want) { return $true }; Start-Sleep -Seconds 1 }; $false }
function Web-Ready { for ($i = 0; $i -lt 60; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
if ($Mode -eq 'start') {
    Stop-Service $svc -Force -ErrorAction SilentlyContinue; [void](Wait-Svc 'Stopped' 60)
    Check 'the service is stopped and the pipe name is free before the test' ((Svc-Status) -eq 'Stopped' -and -not (Pipe-Exists)) "$(Svc-Status) pipe=$(Pipe-Exists)"
    $code = @'
$ps = New-Object System.IO.Pipes.PipeSecurity
$ps.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule('Everyone', 'FullControl', 'Allow')))
$s = New-Object System.IO.Pipes.NamedPipeServerStream('VM2BackupService', 'InOut', 4, 'Byte', 'None', 4096, 4096, $ps)
# настоящий захватчик СЛУШАЕТ (раунд 16: без WaitForConnection клиент получал 231 PIPE_BUSY и до проверки сервера дело не доходило)
for ($k = 0; $k -lt 3000; $k++) { $s.WaitForConnection(); try { $s.Disconnect() } catch {} }
'@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $p = Start-Process powershell -ArgumentList '-NoProfile', '-EncodedCommand', $enc -WindowStyle Hidden -PassThru
    Set-Content $pidFile $p.Id
    for ($i = 0; $i -lt 40 -and -not (Pipe-Exists); $i++) { Start-Sleep -Milliseconds 500 }
    Check 'another program holds the pipe name now' (Pipe-Exists) ''
    Start-Service $svc -ErrorAction SilentlyContinue
    $up = Wait-Svc 'Running' 90
    Check 'F9: the service RUNS although another program holds the pipe name' $up (Svc-Status)
    $w = Web-Ready; Check 'F9: the web site of the service works while the pipe name is held' ($null -ne $w) ''
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VM2BackupService' } -MaxEvents 5 -ErrorAction SilentlyContinue | ForEach-Object { $_.Message })
    Write-Output "INFO service events: $($ev -join ' || ')"
}
elseif ($Mode -eq 'finish') {
    $sp = if (Test-Path $pidFile) { [int](Get-Content $pidFile) } else { 0 }
    if ($sp) { Stop-Process -Id $sp -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidFile -ErrorAction SilentlyContinue
    $took = $false
    for ($i = 0; $i -lt 120 -and -not $took; $i++) {
        Start-Sleep -Milliseconds 500
        try { $c = New-Object IO.Pipes.NamedPipeClientStream('.', 'VM2BackupService', [IO.Pipes.PipeDirection]::InOut); $c.Connect(500); $c.Close(); $took = $true } catch {}
    }
    Check 'F9: after the other program is gone the service takes the pipe name by itself' $took ''
    Check 'the service is still running' ((Svc-Status) -eq 'Running') (Svc-Status)
}
Write-Output ("FAILED=" + $global:Failed)
exit $global:Failed
