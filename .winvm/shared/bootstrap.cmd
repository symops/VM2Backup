@echo off
rem Run once at the first logon (FirstLogonCommands, via the answer CD's vm2bboot.cmd): makes this machine the VM2Backup test executor.
set SRC=\\10.0.2.4\qemu
mkdir C:\vm2b 2>nul
copy /y "%SRC%\poller.bat" C:\vm2b\poller.bat >nul
ver > "%SRC%\results\windows-version.txt"
systeminfo >> "%SRC%\results\windows-version.txt" 2>&1
schtasks /create /tn VM2BPoller /tr "C:\vm2b\poller.bat %SRC%" /sc onlogon /rl highest /f > "%SRC%\results\task-create.txt" 2>&1
netsh advfirewall set allprofiles state off > "%SRC%\results\firewall.txt" 2>&1
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
powercfg /hibernate off
reg add "HKCU\Control Panel\Desktop" /v ScreenSaveActive /t REG_SZ /d 0 /f >nul
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows Defender" /v DisableAntiSpyware /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\System" /v EnableSmartScreen /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v ConsentPromptBehaviorAdmin /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v FileInfoCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v DirectoryCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v FileNotFoundCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v AllowInsecureGuestAuth /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" /f >nul
sc config wuauserv start= disabled >nul
sc stop wuauserv >nul 2>&1
sc config UsoSvc start= disabled >nul
sc config WaaSMedicSvc start= disabled >nul 2>&1
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoUpdate /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate" /v DoNotConnectToWindowsUpdateInternetLocations /t REG_DWORD /d 1 /f >nul
echo executor installed %DATE% %TIME% > "%SRC%\results\executor-ready.txt"
shutdown /r /t 10
