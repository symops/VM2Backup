# defender-toggle.ps1 on|off: Microsoft Defender на тестовой ВМ ВЫКЛЮЧЕН политикой DisableAntiSpyware=1 (её ставит bootstrap.cmd для всех тестовых ВМ: службы Defender не запущены,
# Get-MpComputerStatus даёт 0x800106BA). Для проверки EICAR политика на время снимается (on, нужен перезапуск Windows), в конце возвращается (off, тоже с перезапуском).
# Ничего в самом Defender (исключения, параметры защиты) не меняется; ВМ удаляется после прогона.
param([string]$Mode = 'info')
. C:\vm2b\guest\lib.ps1
$pol = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows Defender'
if ($Mode -eq 'on') {
    $old = (Get-ItemProperty $pol -ErrorAction SilentlyContinue).DisableAntiSpyware
    Write-Output "INFO policy DisableAntiSpyware was: $old; removing it (restart needed)"
    Remove-ItemProperty $pol -Name DisableAntiSpyware -ErrorAction SilentlyContinue
    Remove-ItemProperty "$pol\Real-Time Protection" -Name DisableRealtimeMonitoring -ErrorAction SilentlyContinue
    $svc = Get-CimInstance Win32_Service -Filter "Name='WinDefend'"; Write-Output "INFO WinDefend: $($svc.State) / $($svc.StartMode)"
} elseif ($Mode -eq 'off') {
    New-Item -Path $pol -Force | Out-Null; Set-ItemProperty $pol -Name DisableAntiSpyware -Value 1 -Type DWord
    Write-Output "INFO policy DisableAntiSpyware=1 restored (restart needed)"
} else {
    $svc = Get-CimInstance Win32_Service -Filter "Name='WinDefend'"; Write-Output "INFO WinDefend: $($svc.State) / $($svc.StartMode)"
    try { $mp = Get-MpComputerStatus; Write-Output "INFO Defender: RealTimeProtectionEnabled=$($mp.RealTimeProtectionEnabled) AMServiceEnabled=$($mp.AMServiceEnabled) signatures=$($mp.AntivirusSignatureVersion) updated=$($mp.AntivirusSignatureLastUpdated)" } catch { Write-Output "INFO Get-MpComputerStatus: $($_.Exception.Message)" }
}
exit 0
