@echo off
rem VM2Backup test executor (all OSes). Started at every logon of the autologon user by the scheduled task VM2BPoller, so it
rem runs in the interactive session (GUI tests need that).  Usage: poller.bat <share>   (x86: \\10.0.2.4\qemu, dockur: \\host.lan\Data)
rem Takes the host's jobs (<share>\jobs\*.cmd) one by one, in name order: moves the job to <share>\jobs\done, runs a local copy,
rem output -> <share>\results\<job>.out, then <share>\results\<job>.rc holds the exit code.
rem A job that restarts the VM never gets its .rc: the host waits for what the job writes before restarting.
set SRC=%~1
if "%SRC%"=="" set SRC=\\10.0.2.4\qemu
mkdir C:\vm2b\run 2>nul
echo poller started %DATE% %TIME% session=%SESSIONNAME% user=%USERNAME% > "%SRC%\results\poller-alive.txt"
:loop
for %%f in ("%SRC%\jobs\*.cmd") do (
  echo %%~nxf started %DATE% %TIME% > "%SRC%\results\current.txt"
  copy /y "%%f" "C:\vm2b\run\%%~nxf" >nul
  move /y "%%f" "%SRC%\jobs\done\" >nul
  cmd /c "C:\vm2b\run\%%~nxf" > "%SRC%\results\%%~nf.out" 2>&1
  > "%SRC%\results\%%~nf.rc" echo %ERRORLEVEL%
  del "%SRC%\results\current.txt" >nul 2>&1
)
timeout /t 3 /nobreak >nul
goto loop
