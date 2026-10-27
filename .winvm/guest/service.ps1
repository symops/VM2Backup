# The Windows service under the real Service Control Manager.
. C:\vm2b\guest\lib.ps1
function St { $s = (& sc.exe query VM2BackupService | Select-String '\b(STOPPED|START_PENDING|STOP_PENDING|RUNNING|CONTINUE_PENDING|PAUSE_PENDING|PAUSED)\b' | Select-Object -First 1); if ($s) { ($s.ToString() -replace '\s+', ' ').Trim() } else { 'none' } }      # sc.exe output is localized (no 'STATE' on a Russian Windows): match the state names, which stay English
function Wait-State($want, $ms) { $t0 = Get-Date; while (((Get-Date) - $t0).TotalMilliseconds -lt $ms) { if ((St) -match $want) { return $true }; Start-Sleep -Milliseconds 100 }; return $false }
function Svc-Pid { $o = Get-WmiObject Win32_Service -Filter "Name='VM2BackupService'"; if ($o) { [int]$o.ProcessId } else { 0 } }      # not sc queryex: localized
function Web-Up { $w = New-Web 'https://127.0.0.1:8443'; for ($i = 0; $i -lt 60; $i++) { $r = Web-Call $w 'GET' '/api/me' $null; if ($r.Status -eq 200) { return $true }; Start-Sleep -Milliseconds 500 }; return $false }

Check 'service is RUNNING after the install' ((St) -match 'RUNNING') (St)
$cfg = (& sc.exe qc VM2BackupService) -join "`n"
Check 'start type is AUTO_START (DELAYED)' ($cfg -match 'AUTO_START\s+\(DELAYED\)') $cfg
Check 'runs as LocalSystem' ($cfg -match 'LocalSystem') $cfg
Check 'binary path is quoted and has /service' ($cfg -match '"[^"]*VM2BackupService.exe"\s+/service') $cfg
$fl = (& sc.exe qfailure VM2BackupService) -join "`n"
Check 'recovery: RESTART x3 configured' (([regex]::Matches($fl, 'RESTART')).Count -ge 3 -or ([regex]::Matches($fl, '= \d{5,6}\D')).Count -ge 3) $fl
Check 'web site answers' (Web-Up) 'no answer on https://127.0.0.1:8443'
$pipes = [IO.Directory]::GetFiles('\\.\pipe\')
Check 'named pipe \\.\pipe\VM2BackupService exists' ($pipes -contains '\\.\pipe\VM2BackupService') ($pipes -join ',')

# idle stop / start
$t0 = Get-Date; & sc.exe stop VM2BackupService | Out-Null
$seen = @(); while (((Get-Date) - $t0).TotalSeconds -lt 60) { $s = St; if ($seen[-1] -ne $s) { $seen += $s }; if ($s -match 'STOPPED') { break }; Start-Sleep -Milliseconds 100 }
$dt = ((Get-Date) - $t0).TotalSeconds
Write-Output ("idle stop: " + ($seen -join ' -> ') + (" in {0:N1}s" -f $dt))
Check 'idle stop reaches STOPPED' ((St) -match 'STOPPED') (St)
Check 'idle stop < 20 s' ($dt -lt 20) $dt
Check 'pipe gone after stop' (-not ([IO.Directory]::GetFiles('\\.\pipe\') -contains '\\.\pipe\VM2BackupService')) ''
& sc.exe start VM2BackupService | Out-Null
Check 'start reaches RUNNING' (Wait-State 'RUNNING' 30000) (St)
Check 'web up after start' (Web-Up) ''
# restart (stop + start by net)
$o = & net.exe stop VM2BackupService 2>&1; $o2 = & net.exe start VM2BackupService 2>&1
Check 'net stop / net start works' ((St) -match 'RUNNING') (($o + $o2) -join ' ')

# stop while a job runs: STOP_PENDING must show, the job must be kept and continue after the next start
$w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; Check 'login admin/admin' ($r.Status -eq 200 -and $r.Json.ok) $r.Text
$src = 'C:\vm2b\jobdata'; Remove-Item -Recurse -Force $src -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmA" | Out-Null
$rnd = New-Object Random 42; $buf = New-Object byte[] (1MB); $fs = [IO.File]::Create("$src\vmA\disk.vmdk"); for ($i = 0; $i -lt 24; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close()
$tl = Api $w 'tasks.list' @{}; foreach ($x in @($tl.Result)) { if ($x.name -eq 'stoptest') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
$old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq 'guest vms') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'guest vms'; path = $src } }; $srvId = $sv.Result.id
Check 'servers.save directory' ([bool]$srvId) $sv.Text
$el = @{ source = @{ server = $srvId; vms = @('vmA') }; target = @{ server = 'local'; directory = 'C:\vm2b\store\{VM}' }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 0; segment_mib = 4; speed_mbit = 16 }
$sc = Api $w 'scheduler.set' @{ enabled = $true }; Write-Output ("scheduler.set: " + $sc.Text)
$tk = Api $w 'tasks.save' @{ task = @{ name = 'stoptest'; enabled = $true; elements = @($el) } }; $taskId = $tk.Result.id
Check 'tasks.save' ([bool]$taskId) $tk.Text
$br = Api $w 'tasks.run' @{ id = $taskId }
Check 'tasks.run accepted' ($br.Status -eq 200 -and $br.Json.ok) $br.Text
Start-Sleep -Seconds 4
$jl = Api $w 'jobs.list' @{}; Write-Output ("jobs before stop: " + $jl.Text)
$t0 = Get-Date; & sc.exe stop VM2BackupService | Out-Null
$seen = @(); while (((Get-Date) - $t0).TotalSeconds -lt 90) { $s = St; if ($seen[-1] -ne $s) { $seen += $s }; if ($s -match 'STOPPED') { break }; Start-Sleep -Milliseconds 50 }
$dt = ((Get-Date) - $t0).TotalSeconds
Write-Output ("busy stop: " + ($seen -join ' -> ') + (" in {0:N1}s" -f $dt))
Check 'busy stop reaches STOPPED' ((St) -match 'STOPPED') (St)
Check 'no kill needed (< 60 s)' ($dt -lt 60) $dt
& sc.exe start VM2BackupService | Out-Null
Check 'start after busy stop' (Wait-State 'RUNNING' 30000) (St)
Web-Up | Out-Null; $w = New-Web 'https://127.0.0.1:8443'; Web-Login $w | Out-Null
$done = $false; for ($i = 0; $i -lt 120; $i++) { $h = Api $w 'history.list' @{ limit = 5 }; if ($h.Result -and @($h.Result).Count -ge 1 -and ($h.Text -match 'Success')) { $done = $true; break }; Start-Sleep -Seconds 2 }
Write-Output ("history: " + $h.Text)
Check 'the interrupted backup continues after the restart and ends Success' $done $h.Text

# the event source of the service has a message file: Event Viewer shows the text itself, not "the description for Event ID 1 cannot be found"
$tok = 'vm2b-event-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
try { Write-EventLog -LogName Application -Source VM2BackupService -EventId 1 -EntryType Error -Message "VM2Backup test event $tok" -ErrorAction Stop } catch { Write-Output ("INFO  Write-EventLog: " + $_.Exception.Message) }
Start-Sleep -Seconds 2
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VM2BackupService' } -MaxEvents 10 -ErrorAction SilentlyContinue | Where-Object { $_.Properties.Count -gt 0 -and ($_.Properties[0].Value -match $tok) } | Select-Object -First 1
$mf = try { (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\VM2BackupService' -ErrorAction Stop).EventMessageFile } catch { '' }
Write-Output ("INFO  EventMessageFile = $mf; the rendered text: " + $(if ($ev) { ($ev.Message -split "`r?`n")[0] } else { '(event not found)' }))
Check 'event source VM2BackupService is registered with a message file' ([bool]$mf) 'no EventMessageFile'
Check 'Event Viewer shows the text of a VM2BackupService event (message file works)' ($ev -and $ev.Message -match [regex]::Escape($tok)) $(if ($ev) { $ev.Message } else { 'event not found' })

# crash recovery: kill the process, the SCM restarts it after 60 s (first recovery action)
$procId = Svc-Pid; Write-Output "service pid $procId"
& taskkill.exe /F /PID $procId | Out-Null
Check 'killed -> STOPPED' (Wait-State 'STOPPED' 10000) (St)
$t0 = Get-Date; $back = Wait-State 'RUNNING' 400000; $dt = ((Get-Date) - $t0).TotalSeconds
Write-Output ("recovery restart after {0:N0}s" -f $dt)
Check 'SCM recovery restarts the service' $back (St)
Check 'recovery delay = the configured one (60 s for the first and second failure, 300 s from the third within a day)' (($dt -gt 40 -and $dt -lt 100) -or ($dt -gt 280 -and $dt -lt 340)) $dt
Check 'web up after recovery' (Web-Up) ''
Write-Output ("FAILED=" + $global:Failed)
exit $global:Failed
