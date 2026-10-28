// VM2Backup — network drives and network browsing for the web site (service side), see docs/WEB.md "net.drives / net.browse".
// A browser cannot list the network or the drive letters of a user; the service (SYSTEM) can, so the RPC commands net.drives and net.browse
// (ServiceCore::handle) call the functions below. The code lives in netbrowse.inc, which target_smb.cpp includes (build.sh compiles only
// the listed modules); it uses the SMB connection code (SmbConnection) and the UNC validator of smb.h.
// Everything the functions need from Windows (registry, LookupAccountSid, WNetOpenEnum, NetShareEnum) is reached through NetApi, so the
// tests (tests_netbrowse.cpp) run against a fake network; the connections themselves go through SmbSetApi as everywhere else.
#pragma once
#include "smb.h"

namespace vm2b {

// One value pair of HKEY_USERS\<SID>\Network\<letter>.
struct NetRegDrive { Str letter, remote, user; };
// A node of the network neighbourhood. kind: 0 = container (domain, workgroup, provider), 1 = server, 2 = share (disk). token: opaque, given back to enumNet for a container.
struct NetNode { Str name, comment, token; int kind; NetNode() : kind(0) {} };

struct NetApi {
    bool (*regSids)(Vec<Str>& sids);                                     // sub-keys of HKEY_USERS (loaded hives only)
    bool (*regDrives)(const Str& sid, Vec<NetRegDrive>& out);            // HKEY_USERS\<sid>\Network\*  (RemotePath, UserName); false = no such key
    Str  (*sidName)(const Str& sid);                                     // LookupAccountSidW: "DOMAIN\name", empty when unknown
    void (*ownDrives)(Vec<NetRegDrive>& out, Str& owner);                // the service's own DRIVE_REMOTE letters; owner = its account
    // One level of WNetOpenEnumW / WNetEnumResourceW. token "" = the root (scope 0 = RESOURCE_GLOBALNET, 1 = RESOURCE_CONTEXT); a container's token otherwise;
    // "\\\\server" (token starting with two backslashes) = the shares of that server. Returns a Win32 error (0 = ok).
    unsigned long (*enumNet)(const Str& token, int scope, Vec<NetNode>& out);
    unsigned long (*enumShares)(const Str& server, Vec<NetNode>& out);   // NetShareEnum level 1 (disk shares); the fallback
    // The servers of the domain this computer belongs to: DsGetDcName (the domain controller) + an LDAP search of the domain's computers with a server operating system
    // (modern Windows does not enumerate NetBIOS neighbours, so the root of the network neighbourhood is empty in a domain). `domain` gets the DNS name of the domain.
    // Returns a Win32 error: 0 = ok (out may still be empty), ERROR_NO_SUCH_DOMAIN (1355) = the computer is not a domain member (the root then shows the WNet list only). May be null.
    unsigned long (*enumDomain)(Str& domain, Vec<NetNode>& out);
};
void NetBrowseSetApi(const NetApi* api);                                 // nullptr = the real one
void NetBrowseSetDeadlineMs(int ms);                                     // test hook: the time one browse may take (default 15000); <= 0 = default
int  NetBrowseLiveWorkers();                                             // worker threads still running (an abandoned one finishes by itself)

// net.drives -> {drives:[{letter:"Z:", unc, user, owner, source:"profile"|"service"}]}
bool NetListDrives(Json& result, Str& err);
// net.browse {path, user?, password?} -> {path, kind:"root|server|share|folder", parent, entries:[{name, path, kind:"server|share|folder", comment?}], truncated, hint?}
bool NetBrowse(const Str& path, const Str& user, const Str& password, Json& result, Str& err);

} // namespace vm2b
