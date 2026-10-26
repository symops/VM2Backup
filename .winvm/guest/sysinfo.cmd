@echo off
echo ==== ver
ver
echo ==== registry
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v ProductName
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v DisplayVersion
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v CurrentBuildNumber
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion" /v UBR
echo ==== arch
echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE% PROCESSOR_ARCHITEW6432=%PROCESSOR_ARCHITEW6432%
echo ==== whoami
whoami
whoami /groups | findstr /i "Mandatory"
echo SESSIONNAME=%SESSIONNAME%
query session
echo ==== ProgramFiles
echo %ProgramFiles% / %ProgramFiles(x86)% / %ProgramData%
echo ==== ipconfig
ipconfig /all
echo ==== systeminfo
systeminfo
echo ==== tls
reg query "HKLM\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols" /s 2>nul
echo ==== powershell
powershell -NoProfile -Command "$PSVersionTable.PSVersion.ToString(); [Environment]::OSVersion.VersionString; [Environment]::Is64BitOperatingSystem; (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full').Release; Get-WmiObject Win32_VideoController | Select-Object Name,CurrentHorizontalResolution,CurrentVerticalResolution | Format-List; Get-WmiObject Win32_Processor | Select-Object Name,NumberOfCores | Format-List"
echo ==== dpi
reg query "HKCU\Control Panel\Desktop" /v LogPixels 2>nul
reg query "HKCU\Control Panel\Desktop" /v Win8DpiScaling 2>nul
curl.exe --version
exit /b 0
