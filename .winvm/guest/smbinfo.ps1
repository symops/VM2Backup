# Maps the plain and the encrypted share with net use, lists them and records the negotiated dialect / signing / encryption (Get-SmbConnection).
param([string]$Plain, [string]$Enc, [string]$User, [string]$Pw)
$o = & cmd.exe /c "net use Y: $Plain `"$Pw`" /user:$User /persistent:no 2>&1"; "plain: $o"
$o = & cmd.exe /c "net use X: $Enc `"$Pw`" /user:$User /persistent:no 2>&1"; "enc: $o"
Set-Content -Path 'Y:\smbinfo-plain.txt' -Value 'plain share write' -ErrorAction Continue
Set-Content -Path 'X:\smbinfo-enc.txt' -Value 'encrypted share write' -ErrorAction Continue
"X: read back: " + (Get-Content 'X:\smbinfo-enc.txt' -ErrorAction Continue)
Get-SmbConnection | Select-Object ServerName, ShareName, UserName, Dialect, Signed, Encrypted, Redirected | Format-Table -AutoSize | Out-String -Width 200
Get-SmbConnection | ForEach-Object { if ($_.ShareName -eq 'vm2benc') { if ($_.Encrypted) { 'PASS  encrypted share session is Encrypted (dialect ' + $_.Dialect + ')' } else { 'FAIL  encrypted share session is not Encrypted' } } }
Get-SmbConnection | ForEach-Object { if ($_.ShareName -eq 'vm2b') { 'INFO  plain share: dialect ' + $_.Dialect + ' signed=' + $_.Signed + ' encrypted=' + $_.Encrypted } }
Remove-Item 'Y:\smbinfo-plain.txt','X:\smbinfo-enc.txt' -ErrorAction SilentlyContinue
& cmd.exe /c "net use Y: /delete /y 2>&1"; & cmd.exe /c "net use X: /delete /y 2>&1"
