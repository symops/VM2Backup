# Microsoft Defender за весь прогон: защита в реальном времени включена, нет обнаружений, нет исключений, продукт не помечен (ничего не отключаем и не добавляем).
. C:\vm2b\guest\lib.ps1
try { $st = Get-MpComputerStatus -ErrorAction Stop
    Write-Host ("INFO  Defender: AMServiceEnabled={0} RealTime={1} Antivirus signatures={2} (updated {3}) TamperProtected={4}" -f $st.AMServiceEnabled, $st.RealTimeProtectionEnabled, $st.AntivirusSignatureVersion, $st.AntivirusSignatureLastUpdated, $st.IsTamperProtected)
    Check 'Defender real-time protection is ON at the end of the run' ($st.RealTimeProtectionEnabled -eq $true) ('RealTime=' + $st.RealTimeProtectionEnabled)
} catch { Write-Host "INFO  Get-MpComputerStatus: $($_.Exception.Message)" }
try { $pref = Get-MpPreference -ErrorAction Stop
    $ex = @($pref.ExclusionPath) + @($pref.ExclusionProcess) + @($pref.ExclusionExtension) | Where-Object { $_ }
    Check 'no Defender exclusions were added' (-not $ex) ($ex -join '; ')
    Check 'real-time monitoring was not disabled by a policy (DisableRealtimeMonitoring is False)' ($pref.DisableRealtimeMonitoring -eq $false) ('DisableRealtimeMonitoring=' + $pref.DisableRealtimeMonitoring)
} catch { Write-Host "INFO  Get-MpPreference: $($_.Exception.Message)" }
try { $th = @(Get-MpThreatDetection -ErrorAction Stop); Check 'Defender detected no threats (the product and test files are clean)' ($th.Count -eq 0) (($th | ForEach-Object { "$($_.ThreatID) $($_.Resources -join ',')" }) -join '; ')
} catch { Write-Host "INFO  Get-MpThreatDetection: $($_.Exception.Message)" }
try { $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Windows Defender/Operational'; Id = 1116, 1117, 1015, 5007 } -MaxEvents 30 -ErrorAction SilentlyContinue)
    $bad = @($ev | Where-Object { $_.Id -in 1116, 1117 })
    Write-Host ("INFO  Defender operational events 1116/1117 (threat found / acted): {0}; 5007 (settings changed, includes the harness' own startup): {1}" -f $bad.Count, @($ev | Where-Object { $_.Id -eq 5007 }).Count)
    Check 'no Defender detection events (1116 / 1117) in the log' ($bad.Count -eq 0) (($bad | ForEach-Object { ($_.Message -split "`r?`n")[0..3] -join ' ' }) -join ' || ')
} catch { Write-Host "INFO  Defender log: $($_.Exception.Message)" }
$global:Failed | Out-Null
