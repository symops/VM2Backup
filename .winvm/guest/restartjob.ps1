# restartjob.ps1 start | check : перезапуск Windows (shutdown /r /f) в СЕРЕДИНЕ длинной копии. После загрузки служба должна продолжить задачу по журналу на диске,
# копия закончиться Success, пройти проверку и восстановиться бит в бит.
#   start : готовит 1,2 ГБ несжимаемых данных, запускает задачу с ограничением скорости (чтобы она шла дольше минуты) и ждёт, пока пойдут тома
#   check : (после перезапуска, служба уже поднята) ждёт конца задачи, проверяет историю, проверку архива и восстановление
param([string]$Mode)
. C:\vm2b\guest\lib.ps1
$src = 'C:\vm2b\rjdata'; $store = 'C:\vm2b\rjstore'
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
function Job-Wait($w, $id, $sec) { for ($i = 0; $i -lt $sec; $i += 2) { $j = Api $w 'jobs.list' @{}; $mine = @($j.Result) | Where-Object { $_.id -eq $id }; if (-not $mine -or ($mine.status -ne 'Waiting' -and $mine.status -ne 'Running')) { return $(if ($mine) { $mine.status } else { 'Gone' }) }; Start-Sleep -Seconds 2 }; return 'timeout' }
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if ($Mode -eq 'start') {
    Remove-Item -Recurse -Force $src, $store, 'C:\vm2b\rjrestored' -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$src\vmR" | Out-Null
    $rnd = New-Object Random 99; $buf = New-Object byte[] (1MB); $fs = [IO.File]::Create("$src\vmR\disk-flat.vmdk"); for ($i = 0; $i -lt 1200; $i++) { $rnd.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }; $fs.Close()
    Set-Content C:\vm2b\rj.sha ((Get-FileHash "$src\vmR\disk-flat.vmdk" -Algorithm SHA256).Hash)
    $old = Api $w 'servers.list' @{}; foreach ($x in @($old.Result)) { if ($x.name -eq 'rj src') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
    $sv = Api $w 'servers.save' @{ server = @{ type = 'directory'; name = 'rj src'; path = $src } }
    $tl = Api $w 'tasks.list' @{}; foreach ($x in @($tl.Result)) { if ($x.name -eq 'restart-mid') { Api $w 'tasks.delete' @{ id = $x.id } | Out-Null } }
    $el = @{ source = @{ server = $sv.Result.id; vms = @('vmR') }; target = @{ server = 'local'; directory = "$store\{VM}" }; compress = @{ codec = 'xz'; level = 1 }; encrypt = @{ enabled = $false }; verify = $true; keep = 0; segment_mib = 16; speed_mbit = 60 }
    $tk = Api $w 'tasks.save' @{ task = @{ name = 'restart-mid'; enabled = $true; elements = @($el) } }; Check 'tasks.save' ([bool]$tk.Result.id) $tk.Text
    $r = Api $w 'tasks.run' @{ id = $tk.Result.id }; Check 'tasks.run accepted' ($r.Status -eq 200) $r.Text
    $n = 0; for ($i = 0; $i -lt 60; $i++) { $f = @(Get-ChildItem "$store" -Recurse -Filter '*.0000*' -ErrorAction SilentlyContinue).Count; if ($f -ge 2) { $n = $f; break }; Start-Sleep -Seconds 2 }
    Write-Output "volumes written before the restart: $n"; Check 'the backup is under way (two volumes are already stored)' ($n -ge 2) "volumes=$n"
    Write-Output ("jobs: " + (Api $w 'jobs.list' @{}).Text)
} else {
    $sha = (Get-Content C:\vm2b\rj.sha).Trim()
    Write-Output ("INFO after the restart, jobs: " + (Api $w 'jobs.list' @{}).Text)
    $hit = $null; for ($i = 0; $i -lt 300; $i++) { $h = Api $w 'history.list' @{ limit = 50 }; $hit = @($h.Result) | Where-Object { $_.name -eq 'restart-mid' } | Select-Object -First 1; if ($hit) { break }; Start-Sleep -Seconds 3 }
    Write-Output ("history: " + ($hit | ConvertTo-Json -Compress))
    Check 'the job went on after the restart of Windows and ended Success' ($hit -and $hit.status -eq 'Success') ($hit | ConvertTo-Json -Compress)
    $hn = @(@((Api $w 'history.list' @{ limit = 50 }).Result) | Where-Object { $_.name -eq 'restart-mid' }).Count; Check 'exactly one history entry for the task (no duplicate run)' ($hn -eq 1) "entries=$hn"
    $left = @(Get-ChildItem $store -Recurse -Include '*.resume', '*.part' -ErrorAction SilentlyContinue).Count; Check 'no journal / .part files left' ($left -eq 0) "left=$left"
    $bl = Api $w 'backups.list' @{ server = 'local'; directory = "$store\vmR" }; Write-Output ("backups.list: " + $bl.Text); $b = @($bl.Result.backups)[0]
    Check 'one finished backup is listed' ($null -ne $b) $bl.Text
    if ($b) {
        $vr = Api $w 'backups.verify' @{ server = 'local'; directory = $b.directory; archive = $b.archive }; $vs = Job-Wait $w $vr.Result.job 600; Check 'backups.verify of the resumed backup' ($vs -eq 'Success' -or $vs -eq 'Gone') $vs
        $rr = Api $w 'backups.restore' @{ server = 'local'; directory = $b.directory; archive = $b.archive; dest = 'C:\vm2b\rjrestored' }; $rs = Job-Wait $w $rr.Result.job 900; Check 'backups.restore of the resumed backup' ($rs -eq 'Success' -or $rs -eq 'Gone') $rs
        $got = (Get-FileHash 'C:\vm2b\rjrestored\vmR\disk-flat.vmdk' -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        Check 'the restored disk is bit-identical to the source (SHA-256)' ($got -eq $sha) "got=$got want=$sha"
    }
    Remove-Item -Recurse -Force $src, $store, 'C:\vm2b\rjrestored' -ErrorAction SilentlyContinue
}
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
