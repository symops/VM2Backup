# acltest.ps1 <out>: запускается от ОБЫЧНОГО пользователя; пытается прочитать config.json службы и снять с секрета защиту DPAPI (область "компьютер").
# Зачем: машинная область DPAPI читается любой учётной записью компьютера, поэтому от кражи пароля серверов защищают только права на папку данных.
param([string]$Out)
$o = @(); $dir = "$env:ProgramData\VM2Backup"
foreach ($f in 'config.json', 'web.json', 'state.json', 'history.json') {
    try { $t = [IO.File]::ReadAllText("$dir\$f"); $o += "READ_OK $f"
        if ($f -eq 'config.json') { $m = [regex]::Match($t, 'dpapi:[A-Za-z0-9+/=]+')
            if ($m.Success) { Add-Type -AssemblyName System.Security
                try { $p = [Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($m.Value.Substring(6)), $null, 'LocalMachine'); $o += "UNPROTECT_OK len=" + $p.Length } catch { $o += 'UNPROTECT_FAILED' } } else { $o += 'NO_DPAPI_BLOB' } } }
    catch [System.UnauthorizedAccessException] { $o += "READ_DENIED $f" } catch [System.IO.FileNotFoundException] { $o += "NO_FILE $f" } catch { $o += "READ_ERROR $f " + $_.Exception.Message }
}
try { [IO.File]::AppendAllText("$dir\config.json", ' '); $o += 'WRITE_OK config.json' } catch { $o += 'WRITE_DENIED config.json' }
try { $x = Get-ChildItem "$dir\logs" -ErrorAction Stop; $o += 'LOGS_LIST_OK' } catch { $o += 'LOGS_LIST_DENIED' }
$o | Set-Content -Path $Out
