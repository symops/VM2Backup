# webshots.ps1 (раунд 13): сайт VM2Backup в НАСТОЯЩЕМ Edge на госте (шрифты Windows, в том числе CJK), снимки через протокол DevTools (безголовый Edge, без внешних библиотек).
#   Для каждого языка из webshots-lang.json (en ru de ja zh ko): вход, страница «Настройки» (уровень сжатия 7 по умолчанию, подписи уровней 0-9), диалог настроек,
#   диалог «Резервное копирование ВМ» на вкладке «Снимок» (горячий режим: флажок и предупреждение), страница «Проводник копий»; проверка, что ничто не вылезает за окно
#   (нет горизонтальной прокрутки, подписи флажков переносятся). Снимки: results\shots\web\<язык>-<имя>.png. Служба должна работать (фаза web / install).
. C:\vm2b\guest\lib.ps1
$out = "$Share\results\shots\web"; New-Item -ItemType Directory -Force $out | Out-Null
$langs = Get-Content "$Share\pkg\webshots-lang.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$edge = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Google\Chrome\Application\chrome.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
Check 'a Chromium browser (Edge) is installed on the guest' ([bool]$edge) 'no msedge.exe'
if (-not $edge) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
Write-Output "INFO browser: $edge $((Get-Item $edge).VersionInfo.FileVersion)"
function Web-Ready { for ($i = 0; $i -lt 120; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
$w = Web-Ready; Check 'web site up' ($null -ne $w) ''
if (-not $w) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
# ---- сервер ESXi (эмулятор внутри гостя) и ВМ, чтобы диалог копирования мог открыться
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\esxilocal.ps1 start | Out-Null
$ej = Get-Content 'C:\vm2b\esxi\servers.json' -Raw | ConvertFrom-Json; $es = @($ej.esxi | Where-Object { $_.kind -eq 'esxi' })[0]
foreach ($x in @((Api $w 'servers.list' @{}).Result)) { if ($x.name -eq 'ESX lab') { Api $w 'servers.delete' @{ id = $x.id } | Out-Null } }
$sv = Api $w 'servers.save' @{ server = @{ type = 'esxi'; name = 'ESX lab'; host = $es.host; port = $es.port; user = $es.user; password = $es.password; fingerprint = $es.fingerprint } }
Write-Output "INFO servers.save esxi: $($sv.Status) $($sv.Text.Substring(0, [Math]::Min(200, $sv.Text.Length)))"
$srvId = $sv.Result.id
# ---- Edge с отладочным портом
Get-Process msedge -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'vm2bedge' } | Out-Null
$prof = 'C:\vm2b\vm2bedge'; Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
$ep = Start-Process -FilePath $edge -ArgumentList "--headless", "--disable-gpu", "--remote-debugging-port=9333", "--user-data-dir=$prof", "--ignore-certificate-errors", "--no-first-run", "--window-size=1280,1000", "about:blank" -PassThru
$tgt = $null; for ($i = 0; $i -lt 40 -and -not $tgt; $i++) { Start-Sleep -Milliseconds 500; try { $tgt = (Invoke-RestMethod 'http://127.0.0.1:9333/json' -TimeoutSec 3) | Where-Object { $_.type -eq 'page' } | Select-Object -First 1 } catch {} }
Check 'headless Edge started with the DevTools port' ([bool]$tgt) ''
if (-not $tgt) { Write-Output ("FAILED=" + $global:Failed); exit 1 }
$ws = New-Object Net.WebSockets.ClientWebSocket; $ws.Options.KeepAliveInterval = [TimeSpan]::FromSeconds(30); $ws.ConnectAsync([uri]$tgt.webSocketDebuggerUrl, [Threading.CancellationToken]::None).Wait()
$script:cid = 0
function Cdp($method, $params = @{}) {
    $script:cid++; $id = $script:cid
    $b = [Text.Encoding]::UTF8.GetBytes((@{ id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 12 -Compress))
    $ws.SendAsync((New-Object 'ArraySegment[byte]' -ArgumentList (, $b)), 'Text', $true, [Threading.CancellationToken]::None).Wait()
    while ($true) {
        $sb = New-Object Text.StringBuilder; $buf = New-Object byte[] 65536
        do { $r = $ws.ReceiveAsync((New-Object 'ArraySegment[byte]' -ArgumentList (, $buf)), [Threading.CancellationToken]::None).Result; [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count)) } until ($r.EndOfMessage)
        $t = $sb.ToString(); if ($t -match '^\{"id":(\d+),' -and [int]$Matches[1] -eq $id) { return $t }
    }
}
function Js($expr) {
    $t = Cdp 'Runtime.evaluate' @{ expression = $expr; awaitPromise = $true; returnByValue = $true }
    $j = $t | ConvertFrom-Json; if ($j.result.exceptionDetails) { return "EXC: " + $j.result.exceptionDetails.text + " " + $j.result.exceptionDetails.exception.description }; return $j.result.result.value
}
function Shot($name) { $t = Cdp 'Page.captureScreenshot' @{ format = 'png' }; if ($t -match '"data":"([^"]+)"') { [IO.File]::WriteAllBytes("$out\$name.png", [Convert]::FromBase64String($Matches[1])) } }
function Wait-Js($expr, $sec = 10) { for ($i = 0; $i -lt $sec * 4; $i++) { if ((Js $expr) -eq $true) { return $true }; Start-Sleep -Milliseconds 250 }; return $false }
Cdp 'Page.enable' | Out-Null
Cdp 'Emulation.setDeviceMetricsOverride' @{ width = 1280; height = 900; deviceScaleFactor = 1; mobile = $false } | Out-Null
$overflowJs = "(() => { const bad = []; for (const e of document.querySelectorAll('.dialog, .dialog *, .content, .content *')) { if (e.offsetParent === null) continue; if (e.scrollWidth > e.clientWidth + 1 && getComputedStyle(e).overflowX === 'visible' && e.clientWidth > 0 && !['TABLE','TBODY','TR','svg'].includes(e.tagName)) bad.push(e.tagName + '.' + e.className + ':' + e.scrollWidth + '>' + e.clientWidth); } return JSON.stringify({docScroll: document.documentElement.scrollWidth, win: window.innerWidth, bad: bad.slice(0, 5)}); })()"
foreach ($lg in @($langs.PSObject.Properties)) {
    $code = $lg.Name; $T = $lg.Value
    Write-Output "INFO ===== language $code"
    Cdp 'Page.navigate' @{ url = 'https://127.0.0.1:8443/' } | Out-Null; Start-Sleep -Seconds 2
    $lr = Js "fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({user:'admin',password:'admin'})}).then(r=>r.json()).then(j=>{try{localStorage.setItem('vm2b.lang','$code')}catch(e){}; return j.ok===true})"
    Check "[$code] login in the browser" ($lr -eq $true) "$lr"
    Cdp 'Page.navigate' @{ url = 'https://127.0.0.1:8443/' } | Out-Null; Start-Sleep -Seconds 2
    $navOk = Wait-Js "document.querySelectorAll('.nav a').length >= 5" 15; Check "[$code] the site is shown after login" $navOk ''
    if (-not $navOk) { Shot "$code-fail"; continue }
    # fonts: какая гарнитура реально применена к тексту
    Write-Output ("INFO [$code] body font-family: " + (Js "getComputedStyle(document.body).fontFamily"))
    # 1. Настройки: страница и диалог
    Js "document.querySelectorAll('.nav a')[4].click(); true" | Out-Null; Start-Sleep -Milliseconds 800
    Shot "$code-settings-page"; Write-Output ("INFO [$code] settings page rows: " + (Js "Array.from(document.querySelectorAll('.content tr, .content .row')).map(r=>r.textContent.trim().replace(/\s+/g,' ')).filter(t=>/xz|7|threads|поток|Thread/i.test(t)).slice(0,6).join(' | ')"))
    $btn = Js "(() => { const bs = Array.from(document.querySelectorAll('.content button')); if (!bs.length) return -1; bs[0].click(); return bs.length; })()"
    Start-Sleep -Milliseconds 700
    $dlg = Js "!!document.querySelector('.dialog')"
    Check "[$code] the Settings dialog opens" ($dlg -eq $true) "buttons=$btn"
    if ($dlg -eq $true) {
        $opts = Js "Array.from(document.querySelectorAll('.dialog select')).map(s=>Array.from(s.options).map(o=>o.textContent).join(' / ')).filter(t=>/0/.test(t)).join(' ## ')"
        Write-Output "INFO [$code] selects in the Settings dialog: $opts"
        $lvl = Js "(() => { const s = Array.from(document.querySelectorAll('.dialog select')).find(x => x.options.length >= 10 && x.options[0].value === '0'); if (!s) return 'none'; const r = {sel: s.value, n: s.options.length, texts: Array.from(s.options).map(o=>o.textContent)}; return JSON.stringify(r); })()"
        Write-Output "INFO [$code] level select: $lvl"
        Check "[$code] the level select has 10 entries 0..9 and 7 is selected by default" ($lvl -match '"n":10' -and $lvl -match '"sel":"7"') $lvl
        Shot "$code-settings-dialog"
        $ov = Js $overflowJs; Write-Output "INFO [$code] settings dialog overflow: $ov"
        Check "[$code] the Settings dialog: no horizontal scroll of the window" ($ov -match '"docScroll":(\d+),"win":(\d+)' -and [int]$Matches[1] -le [int]$Matches[2]) $ov
        Js "document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true})); const c=document.querySelector('.dialog .close, .dialog button'); true" | Out-Null; Start-Sleep -Milliseconds 400
        if (Js "!!document.querySelector('.dialog')") { Js "(() => { const bs = Array.from(document.querySelectorAll('.dialog button')); if (bs.length) bs[bs.length-1].click(); return true; })()" | Out-Null; Start-Sleep -Milliseconds 400 }
    }
    # 2. Горячий режим: Datacenter -> сервер ESX lab -> «Копировать ВМ» -> вкладка «Снимок»
    Js "document.querySelectorAll('.nav a')[0].click(); true" | Out-Null; Start-Sleep -Milliseconds 800
    $sel = Js "(() => { const e = Array.from(document.querySelectorAll('a, div, span, td, li')).find(x => x.children.length === 0 && x.textContent.trim() === 'ESX lab'); if (!e) return false; e.click(); return true; })()"
    Start-Sleep -Seconds 2
    $b2 = Js "(() => { const b = Array.from(document.querySelectorAll('button')).find(x => x.textContent.trim() === $(ConvertTo-Json $T.btn -Compress)); if (!b) return false; b.click(); return true; })()"
    Start-Sleep -Seconds 1
    Check "[$code] the backup dialog opens for the ESXi server" ($b2 -eq $true -and (Js "!!document.querySelector('.dialog')") -eq $true) "server clicked=$sel button=$b2"
    if ((Js "!!document.querySelector('.dialog')") -eq $true) {
        Js "(() => { const a = Array.from(document.querySelectorAll('.tabs a')).find(x => x.textContent.trim() === $(ConvertTo-Json $T.tab -Compress)); if (a) a.click(); return !!a; })()" | Out-Null; Start-Sleep -Milliseconds 500
        Shot "$code-snapshot-cold"
        $st = Js "(() => { const d = document.querySelector('.dialog'); return JSON.stringify(Array.from(d.querySelectorAll('label')).filter(l => l.querySelector('input[type=checkbox]')).map(l => ({t: l.textContent.trim().slice(0, 70), c: l.querySelector('input').checked, w: Math.round(l.getBoundingClientRect().width), lines: Math.round(l.getBoundingClientRect().height / parseFloat(getComputedStyle(l).lineHeight || 18))}))); })()"
        Write-Output "INFO [$code] snapshot tab check boxes (cold, default): $st"
        $h1 = Js "(() => { const l = Array.from(document.querySelectorAll('.dialog label')).find(x => x.textContent.trim() === $(ConvertTo-Json $T.hot -Compress)); if (!l) return 'nolabel'; l.querySelector('input').click(); return l.querySelector('input').checked; })()"
        Start-Sleep -Milliseconds 400; Shot "$code-snapshot-hot"
        Check "[$code] the hot backup check box exists and can be switched on" ($h1 -eq $true) "$h1"
        $notes = Js "Array.from(document.querySelectorAll('.dialog .note, .dialog .msg')).filter(n => n.offsetParent !== null).map(n => n.textContent.trim().slice(0, 90)).join(' || ')"
        Write-Output "INFO [$code] notes / warnings shown with the hot mode on: $notes"
        Check "[$code] a note or warning is shown with the hot backup on" ($notes.Length -gt 10) $notes
        $ov = Js $overflowJs; Write-Output "INFO [$code] backup dialog overflow: $ov"
        Check "[$code] the backup dialog does not overflow the window" ($ov -match '"docScroll":(\d+),"win":(\d+)' -and [int]$Matches[1] -le [int]$Matches[2]) $ov
        Js "(() => { const bs = Array.from(document.querySelectorAll('.dialog button')); const c = bs[bs.length-1]; if (c) c.click(); return true; })()" | Out-Null; Start-Sleep -Milliseconds 400
    }
    # 3. Проводник копий
    Js "document.querySelectorAll('.nav a')[1].click(); true" | Out-Null; Start-Sleep -Milliseconds 800; Shot "$code-explorer"
    $ov = Js $overflowJs; Write-Output "INFO [$code] explorer overflow: $ov"
}
try { $ws.CloseAsync('NormalClosure', '', [Threading.CancellationToken]::None).Wait(2000) } catch {}
try { Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -match 'vm2bedge' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue } } catch {}
if ($srvId) { Api $w 'servers.delete' @{ id = $srvId } | Out-Null }
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\esxilocal.ps1 stop | Out-Null
Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
Write-Output ("INFO screenshots: " + ((Get-ChildItem $out -Filter *.png | Measure-Object).Count))
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
