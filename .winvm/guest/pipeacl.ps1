# The named pipe ACL on the real system: only Administrators + SYSTEM may connect.  Standard user via a scheduled task.
. C:\vm2b\guest\lib.ps1
$pw = 'Std-Pass-123!x'
& net.exe user vm2bstd $pw /add /y | Out-Null
Check 'standard user created' ((& net.exe user vm2bstd 2>&1) -match 'vm2bstd') ''
function Run-As($user, $pass, $out, $tag) {
    Remove-Item $out -ErrorAction SilentlyContinue
    if ($pass) {      # a standard user: a process started with CreateProcessWithLogonW (the task scheduler needs the batch-logon right, which standard users lack)
        $sec = ConvertTo-SecureString $pass -AsPlainText -Force; $cred = New-Object Management.Automation.PSCredential($user, $sec)
        try { Start-Process powershell.exe -Credential $cred -WorkingDirectory C:\vm2b -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'C:\vm2b\guest\pipeclient.ps1', $out -Wait -WindowStyle Hidden -ErrorAction Stop } catch { return 'START FAILED ' + $_.Exception.Message }
    } else {
        $tr = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\pipeclient.ps1 $out"
        & schtasks.exe /create /tn $tag /tr $tr /sc once /st 23:59 /ru $user /rl highest /f | Out-Null
        & schtasks.exe /run /tn $tag | Out-Null
        for ($i = 0; $i -lt 60 -and -not (Test-Path $out); $i++) { Start-Sleep -Milliseconds 500 }
        & schtasks.exe /delete /tn $tag /f | Out-Null
    }
    return $(if (Test-Path $out) { (Get-Content $out) -join ' ' } else { 'NO RESULT' })
}
$std = Run-As "$env:COMPUTERNAME\vm2bstd" $pw 'C:\vm2b\pipe-std.txt' 'VM2BPipeStd'; Write-Output $std
Check 'standard user is DENIED' ($std -match 'DENIED') $std
$sys = Run-As 'SYSTEM' $null 'C:\vm2b\pipe-sys.txt' 'VM2BPipeSys'; Write-Output $sys
Check 'SYSTEM connects' ($sys -match 'CONNECTED') $sys
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\pipeclient.ps1 C:\vm2b\pipe-adm.txt; $adm = (Get-Content C:\vm2b\pipe-adm.txt) -join ' '; Write-Output $adm
Check 'the (elevated) administrator connects' ($adm -match 'CONNECTED') $adm
# the pipe's security descriptor as Windows reports it (accesschk is not here: read it with .NET)
try {
    Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public class PipeSd {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern IntPtr CreateFile(string n, uint a, uint s, IntPtr sa, uint d, uint f, IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true)] static extern uint GetSecurityInfo(IntPtr h, int t, uint i, IntPtr o, IntPtr g, IntPtr d, IntPtr s, out IntPtr sd);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern bool ConvertSecurityDescriptorToStringSecurityDescriptor(IntPtr sd, uint rev, uint info, out IntPtr str, out uint len);
  public static string Get() { IntPtr h = CreateFile(@"\\.\pipe\VM2BackupService", 0x20000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero); if (h == (IntPtr)(-1)) return "open failed " + Marshal.GetLastWin32Error();
    IntPtr sd; uint r = GetSecurityInfo(h, 6, 4, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, out sd); if (r != 0) return "GetSecurityInfo " + r;
    IntPtr s; uint l; ConvertSecurityDescriptorToStringSecurityDescriptor(sd, 1, 4, out s, out l); return Marshal.PtrToStringUni(s); } }
"@
    $sddl = [PipeSd]::Get(); Write-Output "pipe DACL: $sddl"
    Check 'pipe DACL is exactly BA + SY' ($sddl -match '^D:' -and $sddl -match 'BA' -and $sddl -match 'SY' -and $sddl -notmatch 'WD|AU|BU|IU|NS') $sddl
} catch { Write-Output ("DACL read failed: " + $_.Exception.Message) }
& net.exe user vm2bstd /delete | Out-Null
exit $global:Failed
