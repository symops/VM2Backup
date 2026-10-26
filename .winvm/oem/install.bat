@echo off
rem dockur/windows OEM hook (w10 / w11), runs once after Windows is installed: makes this machine the VM2Backup test executor.
set SRC=\\host.lan\Data
mkdir C:\vm2b 2>nul
copy /y "%SRC%\poller.bat" C:\vm2b\poller.bat >nul
ver > "%SRC%\results\windows-version.txt"
systeminfo >> "%SRC%\results\windows-version.txt" 2>&1
schtasks /create /tn VM2BPoller /tr "C:\vm2b\poller.bat %SRC%" /sc onlogon /rl highest /f > "%SRC%\results\task-create.txt" 2>&1
netsh advfirewall set allprofiles state off > "%SRC%\results\firewall.txt" 2>&1
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
sc config wuauserv start= disabled >nul
sc stop wuauserv >nul 2>&1
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoUpdate /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v UtilizeNtCaching /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" /f >nul
rem the SMB client caches directory listings for ~2 minutes by default here (dockur's Samba): the executor would see new jobs that late
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v FileInfoCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v DirectoryCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v FileNotFoundCacheLifetime /t REG_DWORD /d 0 /f >nul
reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v AllowInsecureGuestAuth /t REG_DWORD /d 1 /f >nul
powercfg /hibernate off
echo executor installed %DATE% %TIME% > "%SRC%\results\executor-ready.txt"
shutdown /r /t 20
