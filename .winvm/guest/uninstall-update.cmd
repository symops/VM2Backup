@echo off
rem Installer end: update in place over a running service, removal (data kept), reinstall.  pkg-old\old.exe = the older package.
copy /y "%SRC%\pkg\VM2Backup-Setup.exe" C:\vm2b\setup\VM2Backup-Setup.exe >nul
set B=%ProgramFiles%\VM2Backup
echo ==== install the OLD package
start "" /wait "%SRC%\pkg\old.exe" /install /silent
echo OLD_INSTALL_RC=%ERRORLEVEL%
echo ==== ACL of the data folder with the OLD package (round 2: Users had Modify on the whole folder)
icacls "%ProgramData%\VM2Backup"
icacls "%ProgramData%\VM2Backup\config.json"
"%B%\VM2BackupService.exe" /?  2>nul
powershell -NoProfile -Command "(Get-Item '%B%\VM2BackupService.exe').VersionInfo.FileVersion"
sc query VM2BackupService | findstr /r "RUNNING STOPPED PENDING"
echo ==== create data (a task + a server) so that we can see it kept
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\kept.ps1 add
for /f %%p in ('powershell -NoProfile -Command "(Get-Process VM2BackupService).Id"') do set OLDPID=%%p
echo service pid before the update: %OLDPID%
echo ==== UPDATE in place over the running service (new package)
start "" /wait C:\vm2b\setup\VM2Backup-Setup.exe /install /silent
echo UPDATE_RC=%ERRORLEVEL%
timeout /t 8 /nobreak >nul
echo ==== ACL of the data folder after the UPDATE (the new service closes the folder to SYSTEM + Administrators)
icacls "%ProgramData%\VM2Backup"
icacls "%ProgramData%\VM2Backup\config.json"
icacls "%ProgramData%\VM2Backup\logs"
powershell -NoProfile -Command "(Get-Item '%B%\VM2BackupService.exe').VersionInfo.FileVersion"
for /f %%p in ('powershell -NoProfile -Command "(Get-Process VM2BackupService).Id"') do set NEWPID=%%p
echo service pid after the update: %NEWPID%
sc query VM2BackupService | findstr /r "RUNNING STOPPED PENDING"
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\kept.ps1 state
echo ==== REMOVE (silent)
start "" /wait C:\vm2b\setup\VM2Backup-Setup.exe /remove /silent
echo REMOVE_RC=%ERRORLEVEL%
sc query VM2BackupService
dir /b "%B%" 2>&1
echo --- data kept?
dir /b "%ProgramData%\VM2Backup"
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" /s /f VM2Backup
netstat -ano | findstr ":8443"
tasklist | findstr /i VM2Backup
echo ==== REINSTALL
start "" /wait C:\vm2b\setup\VM2Backup-Setup.exe /install /silent
echo REINSTALL_RC=%ERRORLEVEL%
sc query VM2BackupService | findstr /r "RUNNING STOPPED PENDING"
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\kept.ps1 list
echo ==== uninstall.cmd of the package
if exist "%B%\uninstall.cmd" (echo uninstall.cmd present) else (echo uninstall.cmd NOT in the install folder)
exit /b 0
