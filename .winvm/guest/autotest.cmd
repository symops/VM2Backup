@echo off
rem Called by a job after: set TAG=<name> & set ARGS=<vm2bautotest arguments>  (commas in ARGS are fine in a set).  Runs the native autotest from a local
rem copy of the build; output also in C:\vm2b\logs\<TAG>.txt and %SRC%\results\autotest-<TAG>.txt.
mkdir C:\vm2b\t 2>nul
mkdir C:\vm2b\logs 2>nul
copy /y "%SRC%\pkg\bin\*.exe" C:\vm2b\t\ >nul
cd /d C:\vm2b\t
set VM2B_GUI_TESTS=1
vm2bautotest.exe %ARGS% > C:\vm2b\logs\%TAG%.txt 2>&1
set RC=%ERRORLEVEL%
copy /y C:\vm2b\logs\%TAG%.txt "%SRC%\results\autotest-%TAG%.txt" >nul
type C:\vm2b\logs\%TAG%.txt | findstr /b /c:"FAIL" /c:"vm2bautotest seed" /c:" passed,"
echo AUTOTEST_RC=%RC%
exit /b %RC%
