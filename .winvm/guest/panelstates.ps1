# panelstates.ps1 <tag> (раунд 17): панель VM2Backup.exe в состояниях Running / Stopped / Blocked и «лже-сервер исчез» на текущем масштабе; снимки + тексты и доступность элементов (UI Automation)
param([string]$Tag = '100')
. C:\vm2b\guest\lib.ps1
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class Dpi2 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[void][Dpi2]::SetProcessDPIAware()
$svc = 'VM2BackupService'; $B = "$env:ProgramFiles\VM2Backup\VM2Backup.exe"
if (-not (Test-Path $B)) { $B = "${env:ProgramFiles(x86)}\VM2Backup\VM2Backup.exe" }
New-Item -ItemType Directory -Force -Path "$Share\results\shots" | Out-Null
function Svc-Status { $s = Get-Service $svc -ErrorAction SilentlyContinue; if ($s) { [string]$s.Status } else { 'none' } }
function Wait-Svc($want, $sec) { for ($i = 0; $i -lt $sec; $i++) { if ((Svc-Status) -eq $want) { return $true }; Start-Sleep -Seconds 1 }; $false }
function Pipe-Exists { [bool](@([IO.Directory]::GetFiles('\\.\pipe\')) -contains '\\.\pipe\VM2BackupService') }
function Panel-Items {
    $root = [Windows.Automation.AutomationElement]::RootElement
    $c = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $script:pp.Id)
    $w = $root.FindFirst('Children', $c); if (-not $w) { return @() }
    $all = $w.FindAll('Descendants', [Windows.Automation.Condition]::TrueCondition)
    $o = @(); foreach ($e in $all) { $n = $e.Current.Name; $r = $e.Current.BoundingRectangle
        if ($n -or $e.Current.ControlType.ProgrammaticName -match 'Button') { $o += ('{0}|{1}|en={2}|{3},{4} {5}x{6}' -f $e.Current.ControlType.ProgrammaticName.Replace('ControlType.',''), $n, $e.Current.IsEnabled, [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height) } }
    $o }
function Panel-Wait($pattern, $sec) { for ($i = 0; $i -lt $sec; $i++) { $t = (Panel-Items) -join "`n"; if ($t -match $pattern) { return $true }; Start-Sleep -Seconds 1 }; $false }
function Panel-Start { $script:pp = Start-Process $B -PassThru; Start-Sleep -Seconds 6 }
function Panel-Stop { if ($script:pp) { Stop-Process -Id $script:pp.Id -Force -ErrorAction SilentlyContinue }; Start-Sleep -Seconds 1 }
function Snap($name, $pat, $sec) { $ok = Panel-Wait $pat $sec; Start-Sleep -Seconds 1; Take-Screenshot "$Share\results\shots\panelstate-$name-$Tag.png"; $f = "$Share\results\panelstate-items-$Tag.txt"; Add-Content $f "ITEMS $name-${Tag}:"; (Panel-Items) | ForEach-Object { Add-Content $f "  $_" }; return $ok }
Stop-Process -Name VM2Backup -Force -ErrorAction SilentlyContinue
# 1. Running
if ((Svc-Status) -ne 'Running') { Start-Service $svc; [void](Wait-Svc 'Running' 90) }
Panel-Start; $ok = Snap 'running' 'Running' 40; Check "[$Tag%] Running: the panel shows Running" $ok ''; Panel-Stop
# 2. Stopped
Stop-Service $svc -Force; [void](Wait-Svc 'Stopped' 60)
Panel-Start; $ok = Snap 'stopped' 'Stopped' 40; Check "[$Tag%] Stopped: the panel shows Stopped (channel free)" $ok ''; Panel-Stop
# 3. Blocked
$code = @'
$ps = New-Object System.IO.Pipes.PipeSecurity
$ps.AddAccessRule((New-Object System.IO.Pipes.PipeAccessRule('Everyone', 'FullControl', 'Allow')))
$s = New-Object System.IO.Pipes.NamedPipeServerStream('VM2BackupService', 'InOut', 4, 'Byte', 'None', 4096, 4096, $ps)
for ($k = 0; $k -lt 3000; $k++) { $s.WaitForConnection(); try { $s.Disconnect() } catch {} }
'@
$enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
$sq = Start-Process powershell -ArgumentList '-NoProfile', '-EncodedCommand', $enc -WindowStyle Hidden -PassThru
for ($i = 0; $i -lt 40 -and -not (Pipe-Exists); $i++) { Start-Sleep -Milliseconds 500 }
Start-Service $svc -ErrorAction SilentlyContinue; [void](Wait-Svc 'Running' 90)
Panel-Start; $ok = Snap 'blocked' 'Blocked: the control channel is held by another program' 40
Check "[$Tag%] Blocked: the panel shows Blocked while the service runs" $ok ''
$t = (Panel-Items) -join "`n"
Check "[$Tag%] Blocked: 'Held by: powershell.exe (process $($sq.Id))'" ($t -match "Held by" -and $t -match "powershell\.exe \(process $($sq.Id)\)") ''
# 4. лже-сервер исчез, панель не перезапускается
Stop-Process -Id $sq.Id -Force -ErrorAction SilentlyContinue
$ok = Snap 'recovered' 'Running' 90
Check "[$Tag%] the squatter is gone: the same panel returns to Running without a restart" ($ok -and -not ($t2 = ((Panel-Items) -join "`n")).Contains('Blocked')) ''
Panel-Stop
Write-Output ("FAILED=" + $global:Failed)
exit $global:Failed
