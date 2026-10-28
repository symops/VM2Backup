# domain-wneterr.ps1 <\\сервер\шара> <пользователь> <пароль>: что НА САМОМ ДЕЛЕ возвращает Windows при входе на шару (WNetAddConnection2W): код, и если это 1208 (ERROR_EXTENDED_ERROR) - WNetGetLastError (код провайдера и текст).
param([string]$Remote, [string]$User, [string]$Pw)
$src = @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class WN {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct NR { public int scope, type, disp, usage; public string local, remote, comment, provider; }
  [DllImport("mpr.dll", CharSet=CharSet.Unicode)] public static extern int WNetAddConnection2W(ref NR nr, string pw, string user, int flags);
  [DllImport("mpr.dll", CharSet=CharSet.Unicode)] public static extern int WNetCancelConnection2W(string name, int flags, bool force);
  [DllImport("mpr.dll", CharSet=CharSet.Unicode)] public static extern int WNetGetLastErrorW(out int err, StringBuilder buf, int bufSize, StringBuilder name, int nameSize);
}
'@
Add-Type -TypeDefinition $src
$nr = New-Object WN+NR; $nr.type = 1; $nr.remote = $Remote
$rc = [WN]::WNetAddConnection2W([ref]$nr, $Pw, $User, 4)
Write-Output ("WNET rc=$rc user=$User remote=$Remote")
if ($rc -eq 1208) { $e = 0; $b = New-Object Text.StringBuilder 512; $n = New-Object Text.StringBuilder 128; $r2 = [WN]::WNetGetLastErrorW([ref]$e, $b, 512, $n, 128); Write-Output ("WNetGetLastError rc=$r2 providerCode=$e (0x" + ('{0:X}' -f $e) + ") provider=[" + $n + "] text=[" + $b.ToString().Trim() + "]") }
if ($rc -eq 0) { [WN]::WNetCancelConnection2W($Remote, 0, $true) | Out-Null }
