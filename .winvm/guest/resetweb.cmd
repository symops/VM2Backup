@echo off
rem The documented recovery: stop the service, VM2BackupService.exe /reset-web, start it.
set B=%ProgramFiles%\VM2Backup
echo ---- while the service runs it must refuse
"%B%\VM2BackupService.exe" /reset-web
echo RESETWEB_WHILE_RUNNING_RC=%ERRORLEVEL%
net stop VM2BackupService
"%B%\VM2BackupService.exe" /reset-web
echo RESETWEB_RC=%ERRORLEVEL%
net start VM2BackupService
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\waitsvc.ps1 120
exit /b %ERRORLEVEL%
