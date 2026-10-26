@echo off
rem Removes VM2Backup (the programs, the service, the shortcuts, the "Apps & features" entry). Run as Administrator.
rem Your settings, the history and the backups you made are kept (C:\ProgramData\VM2Backup).
net session >nul 2>&1 || (echo Run this file as Administrator. / Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"
set ARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=x64
if /i "%PROCESSOR_ARCHITEW6432%"=="AMD64" set ARCH=x64
rem The package root has x64\ and x86\; an installed copy has VM2Backup-Setup.exe next to it.
if exist VM2Backup-Setup.exe (start /wait VM2Backup-Setup.exe /remove /silent) else (start /wait %ARCH%\VM2Backup-Setup.exe /remove /silent)
echo Done. / Готово.
pause
