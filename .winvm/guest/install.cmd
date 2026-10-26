@echo off
rem Installs the package the way a user would (single-file Setup, silent) and records what Windows says about it.
set P=%SRC%\pkg
mkdir C:\vm2b\setup 2>nul
copy /y "%P%\VM2Backup-Setup.exe" C:\vm2b\setup\ >nul
for %%F in (C:\vm2b\setup\VM2Backup-Setup.exe) do echo setup size %%~zF
echo ==== install
start "" /wait C:\vm2b\setup\VM2Backup-Setup.exe /install /silent
echo INSTALL_RC=%ERRORLEVEL%
echo ==== files
dir /s /b "%ProgramFiles%\VM2Backup"
echo ==== sc query
sc query VM2BackupService
sc query VM2BackupService | findstr /r "RUNNING STOPPED PENDING"
echo ==== sc qc
sc qc VM2BackupService
echo ==== sc qdescription
sc qdescription VM2BackupService
echo ==== sc qfailure
sc qfailure VM2BackupService
echo ==== sc sdshow
sc sdshow VM2BackupService
echo ==== delayed autostart
reg query "HKLM\SYSTEM\CurrentControlSet\Services\VM2BackupService" /v DelayedAutostart
echo ==== uninstall entry
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" /s /f VM2Backup
reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall" /s /f VM2Backup 2>nul
echo ==== shortcuts
dir /b "%PUBLIC%\Desktop" 2>nul
dir /s /b "%ProgramData%\Microsoft\Windows\Start Menu\Programs\VM2Backup*" 2>nul
echo ==== data
dir /s /b "%ProgramData%\VM2Backup"
echo ==== listening
netstat -ano | findstr ":8443"
echo ==== log
type "%ProgramData%\VM2Backup\logs\service.log"
exit /b 0
