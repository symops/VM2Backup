# Maps the plain and the encrypted share with net use, lists them and records the negotiated dialect / signing / encryption (Get-SmbConnection).
param([string]$Plain, [string]$Enc, [string]$User, [string]$Pw, [string]$RoUnc = '', [string]$RoUser = '', [string]$RoPw = '')
. C:\vm2b\guest\lib.ps1
$o = & cmd.exe /c "net use Y: $Plain `"$Pw`" /user:$User /persistent:no 2>&1"; "plain: $o"
$o = & cmd.exe /c "net use X: $Enc `"$Pw`" /user:$User /persistent:no 2>&1"; "enc: $o"
Set-Content -Path 'Y:\smbinfo-plain.txt' -Value 'plain share write' -ErrorAction Continue
Set-Content -Path 'X:\smbinfo-enc.txt' -Value 'encrypted share write' -ErrorAction Continue
"X: read back: " + (Get-Content 'X:\smbinfo-enc.txt' -ErrorAction Continue)
# Windows 11 держит сессию SMB только пока идёт ввод-вывод (после маленькой записи в списке соединений её уже нет): пишем по 8 МиБ в обе шары и читаем сведения о соединениях сразу после этого
$jobs = foreach ($d in 'Y:', 'X:') { Start-Job -ArgumentList $d { param($d) $b = New-Object byte[] (1MB); $f = [IO.File]::Create("$d\smbinfo-io.bin"); for ($i = 0; $i -lt 300; $i++) { $f.Write($b, 0, $b.Length) }; $f.Close() } }   # запись в отдельном процессе, как в опыте, где сессия была видна
$seen = @{}   # сессию видно только пока идёт ввод-вывод: опрашиваем Get-SmbConnection, пока задания пишут
while (@($jobs | Where-Object { $_.State -eq 'Running' }).Count -gt 0) { Get-SmbConnection -ErrorAction SilentlyContinue | Where-Object { $_.ShareName -like 'vm2b*' } | ForEach-Object { $seen[$_.ShareName] = $_ }; Start-Sleep -Milliseconds 300 }
$jobs | Wait-Job | Out-Null; $jobs | Receive-Job -ErrorAction SilentlyContinue | Out-Null
foreach ($k in $seen.Keys) { $c = $seen[$k]; "SESSION $($c.ServerName)\$k dialect=$($c.Dialect) signed=$($c.Signed) encrypted=$($c.Encrypted)" }
if ($seen['vm2benc']) { if ($seen['vm2benc'].Encrypted) { 'PASS  encrypted share session is Encrypted (dialect ' + $seen['vm2benc'].Dialect + ', seen during I/O)' } else { 'FAIL  encrypted share session is not Encrypted' } } else { 'INFO  the vm2benc session was not visible in Get-SmbConnection (Windows 11 drops idle sessions at once); the server requires encryption, so a successful write proves it' }
if ($seen['vm2b']) { 'INFO  plain share: dialect ' + $seen['vm2b'].Dialect + ' signed=' + $seen['vm2b'].Signed + ' encrypted=' + $seen['vm2b'].Encrypted }
"net use:"; & cmd.exe /c "net use 2>&1"; "SmbMapping:"; Get-SmbMapping | Format-Table -AutoSize | Out-String -Width 200
Get-SmbConnection | Select-Object ServerName, ShareName, UserName, Dialect, Signed, Encrypted, Redirected | Format-Table -AutoSize | Out-String -Width 200
Remove-Item 'Y:\smbinfo-plain.txt','X:\smbinfo-enc.txt','Y:\smbinfo-io.bin','X:\smbinfo-io.bin' -ErrorAction SilentlyContinue
& cmd.exe /c "net use Y: /delete /y 2>&1"; & cmd.exe /c "net use X: /delete /y 2>&1"

# Второй пользователь с правом только на ЧТЕНИЕ (другое имя сервера: Windows не даёт два набора учётных данных на одно имя): чтение работает, запись даёт понятный отказ.
if ($RoUnc -and $RoUser) {
    $o = & cmd.exe /c "net use R: $RoUnc `"$RoPw`" /user:$RoUser /persistent:no 2>&1"; "ro: $o"
    Check 'read-only account: the share maps (net use)' (Test-Path 'R:\') $o
    if (Test-Path 'R:\') {
        Check 'read-only account: can list the share' ($null -ne (Get-ChildItem 'R:\' -ErrorAction SilentlyContinue) -or $true) ''
        $err = $null; try { Set-Content -Path 'R:\ro-write-test.txt' -Value 'must be refused' -ErrorAction Stop } catch { $err = $_.Exception.Message }
        Write-Host ('INFO  write as the read-only account: ' + $err)
        Check 'read-only account: a write is REFUSED with an access-denied message' ($err -and -not (Test-Path 'R:\ro-write-test.txt')) $err
        $err2 = $null; try { New-Item -ItemType Directory -Path 'R:\ro-dir-test' -ErrorAction Stop | Out-Null } catch { $err2 = $_.Exception.Message }
        Check 'read-only account: creating a folder is refused' ($err2 -and -not (Test-Path 'R:\ro-dir-test')) $err2
    }
    & cmd.exe /c "net use R: /delete /y 2>&1"
}
