# rulocale.ps1 (раунд 14): особенности русской Windows, которые не покрывают остальные фазы.
#   1. установка в каталог с кириллицей и пробелом (/dir "C:\Каталог VM2B"): служба зарегистрирована оттуда и работает, сайт отвечает, DACL каталога защищён, затем /remove и обычная установка;
#   2. журнал Windows (Application): события службы читаются без символов замены, имя источника известно;
#   3. консоль с кодовой страницей 866: `VM2BackupService.exe /reset-web /data "C:\Данные-VM2B"` пишет путь с кириллицей; текст читается ИЗ БУФЕРА НАСТОЯЩЕЙ КОНСОЛИ (cmd /k в новом окне,
#      AttachConsole + ReadConsoleOutputCharacter): в кодовой странице 866 байты UTF-8 выглядели бы как «╨Ф╨░...» (этой проверки раньше не было, найдено раундом 14);
#   4. десятичная запятая: числа сайта (JSON) разбираются при языковых параметрах ru-RU, служба в ru-RU пишет и читает настройки.
. C:\vm2b\guest\lib.ps1
$setup = 'C:\vm2b\setup\VM2Backup-Setup.exe'
New-Item -ItemType Directory -Force C:\vm2b\setup | Out-Null; Copy-Item "$env:SRC\pkg\VM2Backup-Setup.exe" $setup -Force      # всегда пакет текущей сборки
Write-Output ("INFO package under test: {0}, {1} bytes" -f (Get-Item $setup).VersionInfo.FileVersion, (Get-Item $setup).Length)
$d1 = 'C:\' + [string]::Join('', [char[]](0x41A, 0x430, 0x442, 0x430, 0x43B, 0x43E, 0x433)) + ' VM2B'          # «Каталог VM2B» (без литералов: файл читается в любой кодировке)
$dat = 'C:\' + [string]::Join('', [char[]](0x414, 0x430, 0x43D, 0x43D, 0x44B, 0x435)) + '-VM2B'               # «Данные-VM2B»
$datName = $dat.Substring(3)
function Svc-Running { (Get-Service VM2BackupService -ErrorAction SilentlyContinue).Status -eq 'Running' }
function Web-Ready { for ($i = 0; $i -lt 90; $i++) { $w = New-Web 'https://127.0.0.1:8443'; $r = Web-Login $w; if ($r.Status -eq 200 -and $r.Json.ok) { return $w }; Start-Sleep -Milliseconds 700 }; return $null }
Write-Output ("INFO culture {0}, decimal separator '{1}', OEM code page {2}, ANSI {3}" -f (Get-Culture).Name, (Get-Culture).NumberFormat.NumberDecimalSeparator, [Console]::OutputEncoding.CodePage, [Text.Encoding]::Default.CodePage)

# --- 1. установка в каталог с кириллицей
Start-Process -FilePath $setup -ArgumentList '/remove', '/silent' -Wait | Out-Null; Remove-Item -Recurse -Force $d1 -ErrorAction SilentlyContinue
$pp = Start-Process -FilePath $setup -ArgumentList '/install', '/silent', '/dir', "`"$d1`"" -Wait -PassThru
Check 'install into a folder with a Cyrillic name and a space: exit code 0' ($pp.ExitCode -eq 0) "rc=$($pp.ExitCode)"
Start-Sleep -Seconds 5
Check 'the service is running' (Svc-Running) ''
$pn = (Get-WmiObject Win32_Service -Filter "Name='VM2BackupService'").PathName
Check 'the registered image path is the Cyrillic folder' ($pn -like "*$d1*") $pn
Check 'service exe, panel and setup are in that folder' ((Test-Path "$d1\VM2BackupService.exe") -and (Test-Path "$d1\VM2Backup.exe") -and (Test-Path "$d1\VM2Backup-Setup.exe")) ''
$sd = (Get-Acl -LiteralPath $d1).GetSecurityDescriptorSddlForm('Access'); Write-Output "INFO SDDL of the folder: $sd"
Check 'the folder DACL is protected (P) and has no Users / Authenticated Users write' ($sd -match 'D:P' -and $sd -notmatch ';;;AU\)' -and $sd -notmatch ';;;WD\)') $sd
$w = Web-Ready; Check 'the web site answers (sign in as admin)' ($null -ne $w) ''

# --- 2. журнал Windows
Stop-Service VM2BackupService -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2; Start-Service VM2BackupService; Start-Sleep -Seconds 4
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'VM2BackupService' } -MaxEvents 20 -ErrorAction SilentlyContinue)
if ($ev.Count -eq 0) { $ev = @(Get-WinEvent -LogName Application -MaxEvents 400 -ErrorAction SilentlyContinue | Where-Object { $_.Message -like '*VM2Backup*' } | Select-Object -First 20) }
Write-Output ("INFO {0} Application events of the service; provider: {1}" -f $ev.Count, ($ev | Select-Object -First 1 -ExpandProperty ProviderName))
foreach ($e in ($ev | Select-Object -First 4)) { Write-Output ("INFO event {0} [{1}]: {2}" -f $e.Id, $e.LevelDisplayName, (([string]$e.Message) -replace '\s+', ' ')) }
Check 'the service writes events into the Windows Application log' ($ev.Count -gt 0) 'no events'
$badEv = @($ev | Where-Object { ([string]$_.Message).Contains([string][char]0xFFFD) -or ([string]$_.Message) -match '\?{3,}' }).Count
Check 'event messages have no replacement characters' ($badEv -eq 0) "$badEv bad"
$hasPath = @($ev | Where-Object { ([string]$_.Message).Contains($d1) }).Count; Write-Output "INFO events that name the Cyrillic install folder: $hasPath"

# --- 3. консоль, кодовая страница 866
Stop-Service VM2BackupService -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3
Remove-Item -Recurse -Force $dat -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $dat | Out-Null
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class Con {
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sec, uint cd, uint fl, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI i);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool ReadConsoleOutputCharacterW(IntPtr h, StringBuilder b, uint len, COORD at, out uint read);
  [DllImport("kernel32.dll")] public static extern uint GetConsoleOutputCP();
  [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct SMALL_RECT { public short L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct CSBI { public COORD Size, Cur; public ushort Attr; public SMALL_RECT Win; public COORD Max; }
  public static string Read(uint pid, out uint cp) {
    cp = 0; FreeConsole(); if (!AttachConsole(pid)) return "ATTACH FAILED " + Marshal.GetLastWin32Error();
    IntPtr h = CreateFileW("CONOUT$", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); CSBI i; if (!GetConsoleScreenBufferInfo(h, out i)) { FreeConsole(); return "NO CSBI"; }
    cp = GetConsoleOutputCP(); StringBuilder all = new StringBuilder();
    for (short y = 0; y <= i.Cur.Y; y++) { StringBuilder b = new StringBuilder(i.Size.X + 1); uint n; COORD c; c.X = 0; c.Y = y; ReadConsoleOutputCharacterW(h, b, (uint)i.Size.X, c, out n); all.AppendLine(b.ToString().TrimEnd()); }
    FreeConsole(); return all.ToString();
  }
}
"@
$exe = "$d1\VM2BackupService.exe"
$cp = Start-Process -FilePath cmd.exe -ArgumentList '/k', "chcp & `"$exe`" /reset-web /data `"$dat`"" -PassThru
Start-Sleep -Seconds 8
[uint32]$cpv = 0; $txt = [Con]::Read([uint32]$cp.Id, [ref]$cpv)
Stop-Process -Id $cp.Id -Force -ErrorAction SilentlyContinue
Write-Output ("INFO console output code page {0}; the buffer:" -f $cpv); foreach ($l in ($txt -split "`r?`n")) { if ($l.Trim()) { Write-Output ("INFO   | " + $l) } }
Check '/reset-web in a CP866 console shows the data folder with the Cyrillic name readably' ($txt.Contains($datName)) "buffer has no '$datName' (code page $cpv)"
Check '/reset-web created web.json in the Cyrillic data folder' (Test-Path (Join-Path $dat 'web.json')) ''
$bak = Get-Content (Join-Path $dat 'web.json') -Raw -ErrorAction SilentlyContinue; Check 'web.json is valid JSON' ($bak -and (($bak | ConvertFrom-Json) -ne $null)) ''
Remove-Item -Recurse -Force $dat -ErrorAction SilentlyContinue

# --- 4. десятичная запятая
$cultureBefore = [Threading.Thread]::CurrentThread.CurrentCulture
Start-Service VM2BackupService; $w = Web-Ready
if ($w) {
    $r = Api $w 'settings.get' @{}; $lv = $r.Result.level
    Check 'JSON numbers are read under ru-RU (decimal comma) as integers' (($lv -is [int] -or $lv -is [long] -or $lv -is [double]) -and ([string]$lv -notmatch '[.,]')) "level=$lv"
    $s = Api $w 'settings.set' @{ level = 7; threads = 0 }; Check 'settings.set level 7 under ru-RU' ($s.Status -eq 200) $s.Text
}
Write-Output ("INFO ru-RU: [double]'1,5' = {0}; ToString of 1.5 = '{1}'" -f [double]::Parse('1,5', (Get-Culture)), (1.5).ToString())

# --- возврат: обычная установка
Start-Process -FilePath $setup -ArgumentList '/remove', '/silent' -Wait | Out-Null; Remove-Item -Recurse -Force $d1 -ErrorAction SilentlyContinue
$pp = Start-Process -FilePath $setup -ArgumentList '/install', '/silent' -Wait -PassThru; Start-Sleep -Seconds 4
Check 'the package is installed back into Program Files, the service runs' ($pp.ExitCode -eq 0 -and (Svc-Running)) "rc=$($pp.ExitCode)"
Write-Output ("FAILED=" + $global:Failed); exit $global:Failed
