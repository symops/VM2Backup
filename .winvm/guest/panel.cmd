@echo off
rem The panel under the real desktop session: /selftest and /guitest, screenshots.  Usage: panel.cmd [tag]
set TAG=%1
if "%TAG%"=="" set TAG=100
set B=C:\Program Files\VM2Backup
if not exist "%B%\VM2Backup.exe" set B=%ProgramFiles%\VM2Backup
echo session=%SESSIONNAME%
echo ==== selftest
"%B%\VM2Backup.exe" /selftest 700
echo SELFTEST_RC=%ERRORLEVEL%
echo ==== service up?
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\waitsvc.ps1 400
echo ==== screenshot of the panel
start "" "%B%\VM2Backup.exe"
ping -n 12 127.0.0.1 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\shot.ps1 panel-%TAG%
taskkill /f /im VM2Backup.exe >nul 2>&1
exit /b 0
