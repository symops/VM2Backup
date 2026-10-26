# Windows 11 specifics, recorded (no pass/fail except where noted): protections, wmic, SMB client policy, TLS, terminal.
$ErrorActionPreference = 'Continue'
"==== OS"; (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' | Select-Object ProductName, DisplayVersion, CurrentBuild, UBR, EditionID | Format-List | Out-String)
"==== wmic present?"; if (Get-Command wmic.exe -ErrorAction SilentlyContinue) { 'wmic.exe: present' } else { 'wmic.exe: ABSENT (24H2+ behaviour)' }
"==== Defender / Smart App Control"
try { Get-MpComputerStatus | Select-Object AMServiceEnabled, RealTimeProtectionEnabled, AntivirusSignatureVersion, IsTamperProtected | Format-List | Out-String } catch { "Get-MpComputerStatus: $($_.Exception.Message)" }
try { 'SmartAppControl VerifiedAndReputablePolicyState = ' + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -ErrorAction Stop).VerifiedAndReputablePolicyState } catch { 'SmartAppControl state: not present' }
try { 'SmartScreen (Explorer) = ' + (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer' -ErrorAction Stop).SmartScreenEnabled } catch { 'SmartScreenEnabled: n/a' }
"==== TPM / SecureBoot"
try { Get-Tpm | Select-Object TpmPresent, TpmReady | Format-List | Out-String } catch { "Get-Tpm: $($_.Exception.Message)" }
try { 'SecureBoot: ' + (Confirm-SecureBootUEFI) } catch { "SecureBoot: $($_.Exception.Message)" }
"==== SMB client policy"
Get-SmbClientConfiguration | Select-Object RequireSecuritySignature, EnableSecuritySignature, EnableInsecureGuestLogons, RequireSecureNegotiate, EnableMultiChannel | Format-List | Out-String
"==== TLS protocols known to Schannel (registry overrides)"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\SecurityProviders\SCHANNEL\Protocols" /s 2>&1
"==== terminal / console host"
Get-ItemProperty 'HKCU:\Console\%%Startup' -ErrorAction SilentlyContinue | Format-List | Out-String
"==== OOBE / account"
whoami; (Get-LocalUser | Select-Object Name, Enabled, PrincipalSource | Format-Table | Out-String)
"==== BitLocker / Device encryption (informational)"
try { Get-BitLockerVolume | Select-Object MountPoint, ProtectionStatus | Format-Table | Out-String } catch { 'n/a' }
