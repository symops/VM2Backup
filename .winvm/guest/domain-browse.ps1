# domain-browse.ps1 (ВМ vm2b-member): какие способы обзора сети реально находят контроллер и рядовые серверы домена (пункт G раунда 6).
# Способы: WNetOpenEnum(RESOURCE_GLOBALNET и RESOURCE_CONTEXT), NetServerEnum (SV_TYPE_SERVER / DOMAIN_CTRL / ALL, домен VM2B), DsGetDcName, LDAP (wldap32, вход
# от имени текущей учётной записи / компьютера) и перечень компьютеров домена.  Запускается от имени текущего пользователя и (с параметром -Out) от SYSTEM = MEMBER$.
param([string]$Out = '')
. C:\vm2b\guest\lib.ps1; . C:\vm2b\guest\domain-lib.ps1
$src = @'
using System; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Text;
public static class Probe {
  [StructLayout(LayoutKind.Sequential)] public struct NETRESOURCE { public int dwScope, dwType, dwDisplayType, dwUsage; public IntPtr lpLocalName, lpRemoteName, lpComment, lpProvider; }
  [DllImport("mpr.dll", CharSet=CharSet.Unicode)] static extern int WNetOpenEnumW(int scope, int type, int usage, IntPtr nr, out IntPtr h);
  [DllImport("mpr.dll", CharSet=CharSet.Unicode)] static extern int WNetEnumResourceW(IntPtr h, ref int cnt, IntPtr buf, ref int size);
  [DllImport("mpr.dll")] static extern int WNetCloseEnum(IntPtr h);
  public static string Wnet(int scope, int depth, IntPtr parent, int level, List<string> names) {
    IntPtr h; int rc = WNetOpenEnumW(scope, 0, 0, parent, out h); if (rc != 0) return "WNetOpenEnum rc=" + rc;
    IntPtr buf = Marshal.AllocHGlobal(65536); string last = "";
    while (true) { int cnt = -1, size = 65536; rc = WNetEnumResourceW(h, ref cnt, buf, ref size); if (rc != 0) { if (rc != 259) last = "WNetEnumResource rc=" + rc; break; }
      for (int i = 0; i < cnt; i++) { NETRESOURCE r = (NETRESOURCE)Marshal.PtrToStructure(new IntPtr(buf.ToInt64() + i * Marshal.SizeOf(typeof(NETRESOURCE))), typeof(NETRESOURCE));
        string rn = r.lpRemoteName == IntPtr.Zero ? "" : Marshal.PtrToStringUni(r.lpRemoteName); string pv = r.lpProvider == IntPtr.Zero ? "" : Marshal.PtrToStringUni(r.lpProvider);
        names.Add(new string(' ', level * 2) + "[disp=" + r.dwDisplayType + " usage=" + r.dwUsage + " prov=" + pv + "] " + rn);
        if ((r.dwUsage & 2) != 0 && level < 3) { IntPtr copy = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(NETRESOURCE))); Marshal.StructureToPtr(r, copy, false); Wnet(2, depth, copy, level + 1, names); } } }
    WNetCloseEnum(h); return last;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct SERVER_INFO_101 { public int platform; public string name; public int vmaj, vmin; public uint type; public string comment; }
  [DllImport("netapi32.dll", CharSet=CharSet.Unicode)] static extern int NetServerEnum(string server, int level, out IntPtr buf, int prefmax, out int read, out int total, uint type, string domain, ref int resume);
  [DllImport("netapi32.dll")] static extern int NetApiBufferFree(IntPtr p);
  public static string SrvEnum(uint type, string domain, List<string> names) {
    IntPtr buf; int read, total, resume = 0; int rc = NetServerEnum(null, 101, out buf, -1, out read, out total, type, domain, ref resume);
    if (rc != 0 && rc != 234) return "NetServerEnum rc=" + rc;
    for (int i = 0; i < read; i++) { SERVER_INFO_101 s = (SERVER_INFO_101)Marshal.PtrToStructure(new IntPtr(buf.ToInt64() + i * Marshal.SizeOf(typeof(SERVER_INFO_101))), typeof(SERVER_INFO_101)); names.Add(s.name + " type=0x" + s.type.ToString("X")); }
    NetApiBufferFree(buf); return "ok read=" + read;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct DCINFO { public string DomainControllerName, DomainControllerAddress; public int AddressType; public Guid DomainGuid; public string DomainName, DnsForestName; public uint Flags; public string DcSiteName, ClientSiteName; }
  [DllImport("netapi32.dll", CharSet=CharSet.Unicode)] static extern int DsGetDcNameW(string cn, string dom, IntPtr g, string site, uint flags, out IntPtr info);
  public static string DcName(uint flags) {
    IntPtr p; int rc = DsGetDcNameW(null, null, IntPtr.Zero, null, flags, out p); if (rc != 0) return "DsGetDcName rc=" + rc;
    DCINFO d = (DCINFO)Marshal.PtrToStructure(p, typeof(DCINFO)); string r = d.DomainControllerName + " addr=" + d.DomainControllerAddress + " domain=" + d.DomainName + " forest=" + d.DnsForestName + " site=" + d.DcSiteName + " flags=0x" + d.Flags.ToString("X"); NetApiBufferFree(p); return r;
  }
  [DllImport("wldap32.dll", CharSet=CharSet.Unicode)] static extern IntPtr ldap_initW(string host, int port);
  [DllImport("wldap32.dll", CharSet=CharSet.Unicode)] static extern int ldap_set_optionW(IntPtr ld, int opt, ref int val);
  [DllImport("wldap32.dll", CharSet=CharSet.Unicode)] static extern int ldap_bind_sW(IntPtr ld, string dn, string cred, int method);
  [DllImport("wldap32.dll", CharSet=CharSet.Unicode)] static extern int ldap_search_sW(IntPtr ld, string b, int scope, string filter, string[] attrs, int attrsonly, out IntPtr res);
  [DllImport("wldap32.dll")] static extern IntPtr ldap_first_entry(IntPtr ld, IntPtr res);
  [DllImport("wldap32.dll")] static extern IntPtr ldap_next_entry(IntPtr ld, IntPtr e);
  [DllImport("wldap32.dll", CharSet=CharSet.Unicode)] static extern IntPtr ldap_get_valuesW(IntPtr ld, IntPtr e, string attr);
  [DllImport("wldap32.dll")] static extern int ldap_value_freeW(IntPtr v);
  [DllImport("wldap32.dll")] static extern int ldap_msgfree(IntPtr res);
  [DllImport("wldap32.dll")] static extern int ldap_unbind(IntPtr ld);
  static string First(IntPtr ld, IntPtr e, string attr) { IntPtr v = ldap_get_valuesW(ld, e, attr); if (v == IntPtr.Zero) return ""; IntPtr s = Marshal.ReadIntPtr(v); string r = s == IntPtr.Zero ? "" : Marshal.PtrToStringUni(s); ldap_value_freeW(v); return r; }
  public static string Ldap(string host, string baseDn, string filter, List<string> names) {
    IntPtr ld = ldap_initW(host, 389); if (ld == IntPtr.Zero) return "ldap_init failed"; int v3 = 3; ldap_set_optionW(ld, 0x11, ref v3);
    int rc = ldap_bind_sW(ld, null, null, 0x0486); if (rc != 0) { ldap_unbind(ld); return "ldap_bind rc=" + rc; }
    IntPtr res; rc = ldap_search_sW(ld, baseDn, 2, filter, new string[] { "name", "dNSHostName", "operatingSystem", "userAccountControl", null }, 0, out res);
    if (rc != 0) { ldap_unbind(ld); return "ldap_search rc=" + rc; }
    for (IntPtr e = ldap_first_entry(ld, res); e != IntPtr.Zero; e = ldap_next_entry(ld, e)) names.Add(First(ld, e, "name") + " | " + First(ld, e, "dNSHostName") + " | " + First(ld, e, "operatingSystem") + " | uac=" + First(ld, e, "userAccountControl"));
    ldap_msgfree(res); ldap_unbind(ld); return "ok n=" + names.Count;
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
function Out-Line([string]$s) { if ($Out) { Add-Content -Path $Out -Value $s -Encoding UTF8 } else { Write-Output $s } }
if ($Out) { Remove-Item $Out -ErrorAction SilentlyContinue }
Out-Line ("INFO who: " + [Security.Principal.WindowsIdentity]::GetCurrent().Name + " on " + $env:COMPUTERNAME)
foreach ($sc in 2, 5) { $n = New-Object 'System.Collections.Generic.List[string]'; $r = [Probe]::Wnet($sc, 0, [IntPtr]::Zero, 0, $n); Out-Line ("INFO WNetOpenEnum scope=" + $sc + " (2=GLOBALNET 5=CONTEXT): " + $r + "; " + $n.Count + " entries"); $n | ForEach-Object { Out-Line ("INFO   " + $_) } }
foreach ($t in @(@('SV_TYPE_SERVER', 2), @('SV_TYPE_DOMAIN_CTRL', 0x10), @('SV_TYPE_DOMAIN_BAKCTRL', 0x20), @('SV_TYPE_NT', 0x1000), @('SV_TYPE_ALL', 0xFFFFFFFF))) {
    foreach ($dom in @($null, 'VM2B')) { $n = New-Object 'System.Collections.Generic.List[string]'; $r = [Probe]::SrvEnum([uint32]$t[1], $dom, $n); Out-Line ("INFO NetServerEnum " + $t[0] + " domain=" + $(if ($dom) { $dom } else { '(null)' }) + ": " + $r + " -> " + ($n -join '; ')) }
}
foreach ($fl in @(@('default', 0), @('DS_RETURN_DNS_NAME', 0x40000000), @('DS_FORCE_REDISCOVERY|DNS', 0x40000001))) { Out-Line ("INFO DsGetDcName " + $fl[0] + ": " + [Probe]::DcName([uint32]$fl[1])) }
$dc = ([Probe]::DcName(0x40000000) -split ' ')[0].TrimStart('\')
Out-Line ("INFO DC for LDAP: $dc")
foreach ($q in @(@('computers (all)', '(objectCategory=computer)'), @('server OS', '(&(objectCategory=computer)(operatingSystem=*Server*))'), @('domain controllers', '(&(objectCategory=computer)(userAccountControl:1.2.840.113556.1.4.803:=8192))'))) {
    $n = New-Object 'System.Collections.Generic.List[string]'; $r = [Probe]::Ldap($dc, 'DC=vm2b,DC=test', $q[1], $n)
    Out-Line ("INFO LDAP " + $q[0] + ": " + $r); $n | ForEach-Object { Out-Line ("INFO   " + $_) }
}
if (-not $Out) { Write-Output ("FAILED=" + $global:Failed) }
Out-Line "INFO DONE"
