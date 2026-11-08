# instacl-user.ps1 <out> <папка>: запускается от ОБЫЧНОГО пользователя (не администратора). Пытается подменить программы в каталоге установки.
# Каталог лежит вне Program Files (C:\VM2B-acltest): без исправления F1 он унаследовал бы от корня тома «Прошедшие проверку: изменение».
param([string]$Out, [string]$Dir)
$o = @()
$svc = "$Dir\VM2BackupService.exe"
try { $fs = [IO.File]::Open($svc, 'Open', 'ReadWrite', 'ReadWrite'); $fs.Close(); $o += 'WRITE_OK VM2BackupService.exe' } catch { $o += 'WRITE_DENIED VM2BackupService.exe' }
try { [IO.File]::AppendAllText("$Dir\VM2Backup.exe", ' '); $o += 'WRITE_OK VM2Backup.exe' } catch { $o += 'WRITE_DENIED VM2Backup.exe' }
try { [IO.File]::WriteAllText("$Dir\planted.dll", 'x'); $o += 'CREATE_OK planted.dll' } catch { $o += 'CREATE_DENIED planted.dll' }
try { Rename-Item -LiteralPath $svc -NewName 'VM2BackupService.old' -ErrorAction Stop; $o += 'RENAME_OK VM2BackupService.exe' } catch { $o += 'RENAME_DENIED VM2BackupService.exe' }
try { Remove-Item -LiteralPath "$Dir\VM2Backup-Setup.exe" -Force -ErrorAction Stop; $o += 'DELETE_OK VM2Backup-Setup.exe' } catch { $o += 'DELETE_DENIED VM2Backup-Setup.exe' }
try { Copy-Item -LiteralPath "$env:SystemRoot\System32\notepad.exe" -Destination $svc -Force -ErrorAction Stop; $o += 'REPLACE_OK VM2BackupService.exe' } catch { $o += 'REPLACE_DENIED VM2BackupService.exe' }
try { $b = [IO.File]::ReadAllBytes($svc); $o += "READ_OK VM2BackupService.exe $($b.Length)" } catch { $o += 'READ_DENIED VM2BackupService.exe' }
try { $x = Get-ChildItem -LiteralPath $Dir -ErrorAction Stop; $o += "LIST_OK $($x.Count)" } catch { $o += 'LIST_DENIED' }
$o | Set-Content -Path $Out
