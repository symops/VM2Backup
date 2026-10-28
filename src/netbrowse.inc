// Included by target_smb.cpp (inside namespace vm2b, after the SMB connection code). See netbrowse.h.
// net.drives: the drives other sessions mapped (HKEY_USERS\<SID>\Network\<letter>) + the service's own remote drives.
// net.browse: the network neighbourhood, the shares of a server, the sub-folders of a share. Every call runs in a worker thread with a
// deadline; the worker owns copies of the credentials, wipes them when it ends and cancels its temporary connection itself.
#include "netbrowse.h"
#include <lmcons.h>

namespace {

const int kMaxBrowseEntries = 5000, kMaxScanned = 200000, kMaxDrives = 200, kMaxWalkDepth = 4, kMaxContainers = 40, kMaxLiveWorkers = 8;
int g_browseDeadlineMs = 15000;
volatile LONG g_liveWorkers = 0;

void Wipe(Str& s) { if (s.size()) memset(s.mut(), 0, s.size()); s.clear(); }

// ---- the real Windows side -------------------------------------------------------------------------------------
bool RealRegSids(Vec<Str>& sids)
{
    HKEY k; if (RegOpenKeyExW(HKEY_USERS, nullptr, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    for (DWORD i = 0;; ++i) {
        wchar_t n[256]; DWORD len = 256; if (RegEnumKeyExW(k, i, n, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        sids.push_back(FromWide(n)); if (sids.size() > 500) break;
    }
    RegCloseKey(k); return true;
}
bool RegString(HKEY k, const wchar_t* name, Str& out)
{
    wchar_t b[2048]; DWORD n = sizeof(b) - 2, t = 0; if (RegQueryValueExW(k, name, nullptr, &t, (LPBYTE)b, &n) != ERROR_SUCCESS || (t != REG_SZ && t != REG_EXPAND_SZ)) return false;
    b[n / 2 < 2047 ? n / 2 : 2047] = 0; out = FromWide(b); return true;
}
bool RealRegDrives(const Str& sid, Vec<NetRegDrive>& out)
{
    Wide key(sid + "\\Network"); HKEY k;
    if (RegOpenKeyExW(HKEY_USERS, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    for (DWORD i = 0;; ++i) {
        wchar_t n[256]; DWORD len = 256; if (RegEnumKeyExW(k, i, n, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY d; if (RegOpenKeyExW(k, n, 0, KEY_READ | KEY_WOW64_64KEY, &d) != ERROR_SUCCESS) continue;
        NetRegDrive r; r.letter = FromWide(n); RegString(d, L"RemotePath", r.remote); RegString(d, L"UserName", r.user);
        RegCloseKey(d); out.push_back(r); if (out.size() > 100) break;
    }
    RegCloseKey(k); return true;
}
Str RealSidName(const Str& sid)
{
    typedef BOOL (WINAPI *Conv)(LPCWSTR, PSID*);
    HMODULE a = LoadLibraryW(L"advapi32.dll"); Conv conv = a ? (Conv)(void*)GetProcAddress(a, "ConvertStringSidToSidW") : nullptr; if (!conv) return Str();
    Wide w(sid); PSID p = nullptr; if (!conv(w.c_str(), &p) || !p) return Str();
    wchar_t name[256], dom[256]; DWORD nn = 256, dn = 256; SID_NAME_USE use; Str r;
    if (LookupAccountSidW(nullptr, p, name, &nn, dom, &dn, &use)) r = dn ? FromWide(dom) + "\\" + FromWide(name) : FromWide(name);
    LocalFree(p); return r;
}
void RealOwnDrives(Vec<NetRegDrive>& out, Str& owner)
{
    wchar_t un[UNLEN + 1]; DWORD n = UNLEN + 1; owner = GetUserNameW(un, &n) ? FromWide(un) : Str("SYSTEM");
    Vec<Str> L, U; SmbListNetworkDrives(L, U);
    for (size_t i = 0; i < L.size(); ++i) { NetRegDrive r; r.letter = L[i]; r.remote = U[i]; out.push_back(r); }
}

typedef DWORD (WINAPI *OpenEnumFn)(DWORD, DWORD, DWORD, LPNETRESOURCEW, LPHANDLE);
typedef DWORD (WINAPI *EnumResFn)(HANDLE, LPDWORD, LPVOID, LPDWORD);
typedef DWORD (WINAPI *CloseEnumFn)(HANDLE);

unsigned long RealEnumNet(const Str& token, int scope, Vec<NetNode>& out)
{
    HMODULE h = GetMpr().h; if (!h) return ERROR_NO_NETWORK;
    OpenEnumFn op = (OpenEnumFn)(void*)GetProcAddress(h, "WNetOpenEnumW"); EnumResFn en = (EnumResFn)(void*)GetProcAddress(h, "WNetEnumResourceW"); CloseEnumFn cl = (CloseEnumFn)(void*)GetProcAddress(h, "WNetCloseEnum");
    if (!op || !en || !cl) return ERROR_NO_NETWORK;
    NETRESOURCEW nr; memset(&nr, 0, sizeof(nr)); LPNETRESOURCEW pnr = nullptr; Wide *wn = nullptr, *wp = nullptr;
    DWORD sc = scope == 1 ? RESOURCE_CONTEXT : RESOURCE_GLOBALNET;
    if (!token.empty()) {
        int sep = token.find('\x01'); Str prov = sep >= 0 ? token.substr(0, (size_t)sep) : Str(), name = sep >= 0 ? token.substr((size_t)sep + 1) : token;
        wn = new Wide(name); wp = new Wide(prov);
        nr.dwScope = RESOURCE_GLOBALNET; nr.dwType = RESOURCETYPE_ANY; nr.dwUsage = RESOURCEUSAGE_CONTAINER; nr.lpRemoteName = (LPWSTR)wn->c_str(); if (!prov.empty()) nr.lpProvider = (LPWSTR)wp->c_str();
        pnr = &nr; sc = RESOURCE_GLOBALNET;
    }
    HANDLE e; DWORD rc = op(sc, RESOURCETYPE_ANY, 0, pnr, &e);
    if (rc == NO_ERROR) {
        Vec<char> buf; buf.resize(16384);
        while (out.size() < (size_t)kMaxBrowseEntries) {
            DWORD cnt = (DWORD)-1, sz = 16384; rc = en(e, &cnt, buf.data(), &sz);
            if (rc == ERROR_NO_MORE_ITEMS) { rc = NO_ERROR; break; }
            if (rc != NO_ERROR) break;
            NETRESOURCEW* r = (NETRESOURCEW*)buf.data();
            for (DWORD i = 0; i < cnt && out.size() < (size_t)kMaxBrowseEntries; ++i) {
                if (!r[i].lpRemoteName) continue;
                NetNode nd; Str rn = FromWide(r[i].lpRemoteName); nd.comment = r[i].lpComment ? FromWide(r[i].lpComment) : Str();
                if (r[i].dwDisplayType == RESOURCEDISPLAYTYPE_SERVER) { nd.kind = 1; nd.name = rn.starts_with("\\\\") ? rn.substr(2) : rn; }
                else if (r[i].dwDisplayType == RESOURCEDISPLAYTYPE_SHARE) { if (r[i].dwType != RESOURCETYPE_DISK) continue; nd.kind = 2; int c = rn.rfind('\\'); nd.name = c >= 0 ? rn.substr((size_t)c + 1) : rn; }
                else if (r[i].dwUsage & RESOURCEUSAGE_CONTAINER) { nd.kind = 0; nd.name = rn; nd.token = (r[i].lpProvider ? FromWide(r[i].lpProvider) : Str()) + "\x01" + rn; }
                else continue;
                out.push_back(nd);
            }
        }
        cl(e);
    }
    delete wn; delete wp; return rc;
}

struct ShareInfo1 { LPWSTR name; DWORD type; LPWSTR remark; };
unsigned long RealEnumShares(const Str& server, Vec<NetNode>& out)
{
    typedef DWORD (WINAPI *ShareEnumFn)(LPWSTR, DWORD, LPBYTE*, DWORD, LPDWORD, LPDWORD, LPDWORD);
    typedef DWORD (WINAPI *FreeFn)(LPVOID);
    HMODULE h = LoadLibraryW(L"netapi32.dll"); if (!h) return ERROR_NO_NETWORK;
    ShareEnumFn en = (ShareEnumFn)(void*)GetProcAddress(h, "NetShareEnum"); FreeFn fr = (FreeFn)(void*)GetProcAddress(h, "NetApiBufferFree");
    if (!en || !fr) return ERROR_NO_NETWORK;
    Wide sv(server); LPBYTE buf = nullptr; DWORD n = 0, tot = 0, resume = 0;
    DWORD rc = en((LPWSTR)sv.c_str(), 1, &buf, MAX_PREFERRED_LENGTH, &n, &tot, &resume);
    if (rc == NO_ERROR || rc == ERROR_MORE_DATA) {
        ShareInfo1* s = (ShareInfo1*)buf;
        for (DWORD i = 0; i < n && out.size() < (size_t)kMaxBrowseEntries; ++i) {
            if ((s[i].type & 0xFF) != 0 || (s[i].type & 0x80000000u) || !s[i].name) continue;          // disk trees only, no special (administrative) shares
            NetNode nd; nd.kind = 2; nd.name = FromWide(s[i].name); nd.comment = s[i].remark ? FromWide(s[i].remark) : Str(); out.push_back(nd);
        }
        rc = NO_ERROR;
    }
    if (buf) fr(buf);
    return rc;
}

// ---- the servers of the domain: DsGetDcName + LDAP (wldap32 is loaded by name: no link-time dependency, nothing happens on a computer without a domain) ----
struct DsDcInfo { LPWSTR dcName, dcAddress; ULONG addrType; GUID guid; LPWSTR domainName, forestName; ULONG flags; LPWSTR dcSite, clientSite; };
struct LdapTimeval { LONG sec, usec; };

Str LdapFirst(void* (*getVals)(void*, void*, const wchar_t*), int (*freeVals)(void*), void* ld, void* entry, const wchar_t* attr)
{
    void* v = getVals(ld, entry, attr); if (!v) return Str();
    wchar_t** a = (wchar_t**)v; Str r = a[0] ? FromWide(a[0]) : Str(); freeVals(v); return r;
}

unsigned long RealEnumDomain(Str& domain, Vec<NetNode>& out)
{
    DWORD t0 = GetTickCount();
    typedef DWORD (WINAPI *DsGetFn)(LPCWSTR, LPCWSTR, GUID*, LPCWSTR, ULONG, void**);
    typedef DWORD (WINAPI *FreeFn)(LPVOID);
    HMODULE na = LoadLibraryW(L"netapi32.dll"); if (!na) return ERROR_NO_NETWORK;
    DsGetFn ds = (DsGetFn)(void*)GetProcAddress(na, "DsGetDcNameW"); FreeFn fr = (FreeFn)(void*)GetProcAddress(na, "NetApiBufferFree");
    if (!ds || !fr) return ERROR_NO_NETWORK;
    void* p = nullptr; DWORD rc = ds(nullptr, nullptr, nullptr, nullptr, 0x40000000u /* DS_RETURN_DNS_NAME */, &p);
    if (rc != 0 || !p) return rc ? rc : ERROR_NO_SUCH_DOMAIN;
    DsDcInfo* info = (DsDcInfo*)p; Str dc = info->dcName ? FromWide(info->dcName) : Str(); domain = info->domainName ? FromWide(info->domainName) : Str(); fr(p);
    while (dc.starts_with("\\")) dc = dc.substr(1);
    if (domain.empty()) return ERROR_NO_SUCH_DOMAIN;
    if (!dc.empty()) { NetNode n; n.kind = 1; n.name = dc; n.comment = "domain controller"; out.push_back(n); }
    // base of the search from the DNS name of the domain: vm2b.test -> DC=vm2b,DC=test (only letters, digits, '-' and '.' are accepted)
    Str base; { size_t from = 0; for (size_t i = 0; i <= domain.size(); ++i) if (i == domain.size() || domain[i] == '.') {
        Str lab = domain.substr(from, i - from); from = i + 1; if (lab.empty()) return 0;
        for (size_t k = 0; k < lab.size(); ++k) { char c = lab[k]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) return 0; }
        if (!base.empty()) base += ","; base += "DC=" + lab; } }
    HMODULE lh = LoadLibraryW(L"wldap32.dll"); if (!lh) return 0;
    typedef void* (__cdecl *InitFn)(wchar_t*, ULONG); typedef ULONG (__cdecl *SetOptFn)(void*, int, void*); typedef ULONG (__cdecl *BindFn)(void*, wchar_t*, wchar_t*, ULONG);
    typedef ULONG (__cdecl *SearchFn)(void*, wchar_t*, ULONG, wchar_t*, wchar_t**, ULONG, void*, void*, LdapTimeval*, ULONG, void**);
    typedef void* (__cdecl *FirstFn)(void*, void*); typedef void* (__cdecl *GetValsFn)(void*, void*, const wchar_t*); typedef int (__cdecl *FreeValsFn)(void*); typedef int (__cdecl *MsgFreeFn)(void*); typedef ULONG (__cdecl *UnbindFn)(void*);
    InitFn init = (InitFn)(void*)GetProcAddress(lh, "ldap_initW"); SetOptFn setopt = (SetOptFn)(void*)GetProcAddress(lh, "ldap_set_optionW"); BindFn bind = (BindFn)(void*)GetProcAddress(lh, "ldap_bind_sW");
    SearchFn search = (SearchFn)(void*)GetProcAddress(lh, "ldap_search_ext_sW"); FirstFn first = (FirstFn)(void*)GetProcAddress(lh, "ldap_first_entry"); FirstFn next = (FirstFn)(void*)GetProcAddress(lh, "ldap_next_entry");
    GetValsFn getvals = (GetValsFn)(void*)GetProcAddress(lh, "ldap_get_valuesW"); FreeValsFn freevals = (FreeValsFn)(void*)GetProcAddress(lh, "ldap_value_freeW"); MsgFreeFn msgfree = (MsgFreeFn)(void*)GetProcAddress(lh, "ldap_msgfree");
    UnbindFn unbind = (UnbindFn)(void*)GetProcAddress(lh, "ldap_unbind");
    if (!init || !setopt || !bind || !search || !first || !next || !getvals || !freevals || !msgfree || !unbind) return 0;
    Wide host(dc.empty() ? domain : dc);
    void* ld = init((wchar_t*)host.c_str(), 389); if (!ld) return 0;
    ULONG v3 = 3; setopt(ld, 0x11 /* LDAP_OPT_PROTOCOL_VERSION */, &v3);
    ULONG brc = bind(ld, nullptr, nullptr, 0x0486 /* LDAP_AUTH_NEGOTIATE: the account of the process (LocalSystem = the computer account) */);
    if (brc != 0) { unbind(ld); return 0; }
    static wchar_t a1[] = L"name", a2[] = L"dNSHostName", a3[] = L"operatingSystem"; wchar_t* attrs[] = { a1, a2, a3, nullptr };
    Wide wb(base), wf(Str("(&(objectCategory=computer)(|(operatingSystem=*Server*)(userAccountControl:1.2.840.113556.1.4.803:=8192)))"));
    LdapTimeval tv = { 8, 0 }; void* res = nullptr;
    ULONG src = search(ld, (wchar_t*)wb.c_str(), 2 /* LDAP_SCOPE_SUBTREE */, (wchar_t*)wf.c_str(), attrs, 0, nullptr, nullptr, &tv, 2000, &res);
    if ((src == 0 || src == 4 /* LDAP_SIZELIMIT_EXCEEDED */) && res) {
        for (void* e = first(ld, res); e && out.size() < (size_t)kMaxBrowseEntries; e = next(ld, e)) {
            Str name = LdapFirst(getvals, freevals, ld, e, L"dNSHostName"); if (name.empty()) name = LdapFirst(getvals, freevals, ld, e, L"name");
            if (name.empty()) continue;
            NetNode n; n.kind = 1; n.name = name; n.comment = LdapFirst(getvals, freevals, ld, e, L"operatingSystem"); out.push_back(n);
        }
    }
    if (res) msgfree(res);
    unbind(ld);
    return 0;
}

const NetApi kRealNet = { RealRegSids, RealRegDrives, RealSidName, RealOwnDrives, RealEnumNet, RealEnumShares, RealEnumDomain };
const NetApi* volatile g_net = &kRealNet;
const NetApi& Net() { return *g_net; }

// ---- net.drives -----------------------------------------------------------------------------------------------
bool AddDrive(Json& arr, const NetRegDrive& d, const Str& owner, const char* source)
{
    if (d.letter.size() != 1 || !((d.letter[0] >= 'A' && d.letter[0] <= 'Z') || (d.letter[0] >= 'a' && d.letter[0] <= 'z'))) return false;
    Str unc, e; if (!SmbIsUncPath(d.remote) || !SmbNormalizeUnc(d.remote, unc, e)) return false;        // only entries with a UNC path
    Str letter = Str::fmt("%c:", (char)toupper((unsigned char)d.letter[0])); Str user = d.user.trim(); if (user == "0") user.clear();       // Windows stores "0" for "no user"
    for (size_t i = 0; i < arr.size(); ++i) { const Json& x = arr.at(i); if (x.get("letter").str() == letter && x.get("unc").str() == unc && x.get("owner").str() == owner && x.get("source").str() == source) return false; }
    Json o = Json::object(); o["letter"] = letter; o["unc"] = unc; o["user"] = user; o["owner"] = owner; o["source"] = source; arr.push(o); return true;
}

} // namespace

namespace { void DomCacheClear(); }
void NetBrowseSetApi(const NetApi* api) { g_net = api ? api : &kRealNet; DomCacheClear(); }
void NetBrowseSetDeadlineMs(int ms) { g_browseDeadlineMs = ms > 0 ? ms : 15000; }
int NetBrowseLiveWorkers() { return (int)g_liveWorkers; }

bool NetListDrives(Json& result, Str& err)
{
    result = Json::object(); Json arr = Json::array(); const NetApi& n = Net();
    Vec<Str> sids; if (n.regSids) n.regSids(sids);
    for (size_t i = 0; i < sids.size() && arr.size() < (size_t)kMaxDrives; ++i) {
        const Str& sid = sids[i];
        if (!sid.starts_with("S-1-") || sid.ends_with("_Classes") || sid == "S-1-5-18" || sid == "S-1-5-19" || sid == "S-1-5-20") continue;
        bool good = true; for (size_t k = 4; k < sid.size(); ++k) if (!((sid[k] >= '0' && sid[k] <= '9') || sid[k] == '-')) good = false;
        if (!good) continue;
        Vec<NetRegDrive> ds; if (!n.regDrives(sid, ds)) continue;
        Str owner = n.sidName ? n.sidName(sid) : Str(); if (owner.empty()) owner = sid;
        for (size_t k = 0; k < ds.size() && arr.size() < (size_t)kMaxDrives; ++k) AddDrive(arr, ds[k], owner, "profile");
    }
    Vec<NetRegDrive> own; Str owner; if (n.ownDrives) n.ownDrives(own, owner);
    for (size_t k = 0; k < own.size() && arr.size() < (size_t)kMaxDrives; ++k) AddDrive(arr, own[k], owner, "service");
    result["drives"] = arr; (void)err; return true;
}

// ---- net.browse -----------------------------------------------------------------------------------------------
namespace {

struct BrowseJob {
    volatile LONG refs, abandoned; HANDLE done;
    Str path, user, pass; int level;             // 0 root, 1 server, 2 share, 3 folder
    Str host, root; DWORD startTick, budgetMs;
    Json result; Str err; bool ok;
    Json partial; volatile LONG partialReady;      // root: the servers of the domain, published before the slow NetBIOS walk; the caller uses them when the walk does not finish in time
    BrowseJob() : refs(2), abandoned(0), partialReady(0), done(CreateEventW(nullptr, TRUE, FALSE, nullptr)), level(0), startTick(GetTickCount()), budgetMs(0), ok(false) {}
    ~BrowseJob() { if (done) CloseHandle(done); Wipe(pass); Wipe(user); }
    bool late() const { return abandoned || (DWORD)(GetTickCount() - startTick) > budgetMs; }
};
void ReleaseJob(BrowseJob* j) { if (InterlockedDecrement(&j->refs) == 0) delete j; }

bool NameOk(const Str& host, const Str& name, bool share)
{   // a name received from the network: the validator decides whether it can be used in a path
    Str out, e; return SmbNormalizeUnc("\\\\" + host + "\\" + (share ? name : Str("x")) + (share ? Str() : "\\" + name), out, e);
}
bool HostOk(const Str& h) { Str out, e; return !h.empty() && SmbNormalizeUnc("\\\\" + h + "\\x", out, e); }

bool LessName(const Str& a, const Str& b) { return a.lower() < b.lower(); }
void SortEntries(Vec<Json>& v)
{   // insertion by case-insensitive name (n <= 5000: use a simple merge-free shell sort)
    for (size_t gap = v.size() / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < v.size(); ++i) { Json t = v[i]; size_t k = i; while (k >= gap && LessName(t.get("name").str(), v[k - gap].get("name").str())) { v[k] = v[k - gap]; k -= gap; } v[k] = t; }
}
void PutEntries(Json& result, Vec<Json>& v, size_t cap, bool& truncated)
{
    SortEntries(v); Json arr = Json::array();
    for (size_t i = 0; i < v.size(); ++i) { if (i >= cap) { truncated = true; break; } arr.push(v[i]); }
    result["entries"] = arr;
}

Str ServerFromToken(const Str& host) { return "\\\\" + host; }

// The last good answer of the directory lookup: the lookup can take a long time (measured 48 s for the LDAP bind on a busy test domain), longer than one request may wait. The worker
// finishes it in the background and stores it here; the next request (and a request that times out) answers with it at once.
struct DomCache { CRITICAL_SECTION cs; Json partial; DWORD tick; bool have; DomCache() : tick(0), have(false) { InitializeCriticalSection(&cs); } };
DomCache& GetDomCache() { static DomCache c; return c; }
void DomCacheClear() { DomCache& c = GetDomCache(); EnterCriticalSection(&c.cs); c.have = false; c.partial = Json(); LeaveCriticalSection(&c.cs); }

void BrowseRoot(BrowseJob* j)
{
    Vec<Str> seen; Vec<Json> servers; unsigned long lastRc = 0; bool partial = false, truncated = false; int containers = 0;
    // the domain of this computer FIRST (fast: DsGetDcName + one LDAP search): the NetBIOS walk below is slow and on a modern domain member empty; if it does not finish in time
    // the caller answers with these servers (partialReady)
    Str domain; unsigned long drc = 1355; Vec<NetNode> dn;
    { DomCache& c = GetDomCache(); EnterCriticalSection(&c.cs); if (c.have && GetTickCount() - c.tick < 3600000u) { j->partial = c.partial; InterlockedExchange(&j->partialReady, 1); } LeaveCriticalSection(&c.cs); }
    if (Net().enumDomain) drc = Net().enumDomain(domain, dn);
    if (drc != 0 && drc != 1355) j->result["domain_error"] = (i64)drc;      // diagnostics: the directory lookup failed with this Windows error (the list is then the NetBIOS one)
    if (drc == 0 && dn.size()) {
        Vec<Json> early; Vec<Str> es; bool tr2 = false;
        for (size_t i = 0; i < dn.size(); ++i) { const NetNode& nd = dn[i]; if (nd.kind != 1 || !HostOk(nd.name)) continue; Str l = nd.name.lower(); bool dup = false; for (size_t k = 0; k < es.size(); ++k) if (es[k] == l) dup = true; if (dup || early.size() >= (size_t)kMaxBrowseEntries) continue;
            es.push_back(l); Json o = Json::object(); o["name"] = nd.name; o["path"] = ServerFromToken(nd.name); o["kind"] = "server"; if (!nd.comment.empty()) o["comment"] = nd.comment; early.push_back(o); }
        PutEntries(j->partial, early, kMaxBrowseEntries, tr2); j->partial["domain"] = domain; j->partial["hint"] = "The list may be incomplete: the network was slow to answer. Type a server name if yours is missing.";
        { DomCache& c = GetDomCache(); EnterCriticalSection(&c.cs); c.partial = j->partial; c.tick = GetTickCount(); c.have = true; LeaveCriticalSection(&c.cs); }
        InterlockedExchange(&j->partialReady, 1);
    }
    for (int scope = 0; scope < 2 && servers.empty(); ++scope) {
        struct Item { Str token; int depth; }; Vec<Item> queue; Item first; first.depth = 0; queue.push_back(first);
        for (size_t qi = 0; qi < queue.size(); ++qi) {
            if (j->late()) { partial = true; break; }
            Vec<NetNode> nodes; unsigned long rc = Net().enumNet(queue[qi].token, scope, nodes);
            if (rc != 0) { lastRc = rc; continue; }
            int depth = queue[qi].depth;
            for (size_t i = 0; i < nodes.size(); ++i) {
                const NetNode& nd = nodes[i];
                if (nd.kind == 1) {
                    Str l = nd.name.lower(); bool dup = false; for (size_t k = 0; k < seen.size(); ++k) if (seen[k] == l) dup = true;
                    if (dup || !HostOk(nd.name)) continue;
                    if (servers.size() >= (size_t)kMaxBrowseEntries) { truncated = true; continue; }
                    seen.push_back(l); Json o = Json::object(); o["name"] = nd.name; o["path"] = ServerFromToken(nd.name); o["kind"] = "server"; if (!nd.comment.empty()) o["comment"] = nd.comment; servers.push_back(o);
                } else if (nd.kind == 0 && depth + 1 < kMaxWalkDepth && containers < kMaxContainers) { ++containers; Item it; it.token = nd.token; it.depth = depth + 1; queue.push_back(it); }
            }
        }
    }
    if (drc == 0) {
        for (size_t i = 0; i < dn.size(); ++i) {
            const NetNode& nd = dn[i]; if (nd.kind != 1 || !HostOk(nd.name)) continue;
            Str l = nd.name.lower(), lshort = l.substr(0, l.find('.') >= 0 ? (size_t)l.find('.') : l.size()); bool dup = false;
            for (size_t k = 0; k < seen.size(); ++k) { Str o = seen[k], os = o.substr(0, o.find('.') >= 0 ? (size_t)o.find('.') : o.size()); if (o == l || os == lshort) dup = true; }       // "dc01" from WNet and "dc01.corp.local" from the directory are one server
            if (dup) continue;
            if (servers.size() >= (size_t)kMaxBrowseEntries) { truncated = true; break; }
            seen.push_back(l); Json o = Json::object(); o["name"] = nd.name; o["path"] = ServerFromToken(nd.name); o["kind"] = "server"; if (!nd.comment.empty()) o["comment"] = nd.comment; servers.push_back(o);
        }
        if (!domain.empty()) j->result["domain"] = domain;
    }
    PutEntries(j->result, servers, kMaxBrowseEntries, truncated);
    if (truncated) j->result["truncated"] = true;
    if (servers.empty()) {
        Str h = "No computers were listed. Network discovery may be switched off, or this network does not publish its list of computers. Type the name of the server instead (for example \\\\server), then browse its shares.";
        if (lastRc) h += " Windows said: " + WinErrorText(lastRc) + ".";
        j->result["hint"] = h;
    } else if (partial) j->result["hint"] = "The list may be incomplete: the network was slow to answer. Type a server name if yours is missing.";
    j->ok = true;
}

bool FailText(BrowseJob* j, unsigned long rc, const Str& share)
{   // the clear English text with the Windows code, like the SMB target
    bool tr; j->err = SmbErrorText(rc, j->host, share, tr); return false;
}

void BrowseServer(BrowseJob* j)
{
    SmbConnection conn; Str e; bool tr = false;
    if (!conn.open("\\\\" + j->host + "\\IPC$", j->user, j->pass, false, e, tr)) { j->err = e; return; }
    if (j->abandoned) return;
    Vec<NetNode> nodes; unsigned long rc = Net().enumNet(ServerFromToken(j->host), 0, nodes);
    if (rc != 0 || nodes.empty()) { nodes.clear(); unsigned long rc2 = Net().enumShares(ServerFromToken(j->host), nodes); if (rc2 != 0 && rc != 0) { FailText(j, rc2 ? rc2 : rc, Str()); return; } }
    Vec<Json> shares; Vec<Str> seen;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const NetNode& nd = nodes[i]; if (nd.kind != 2 || nd.name.ends_with("$") || !NameOk(j->host, nd.name, true)) continue;
        Str l = nd.name.lower(); bool dup = false; for (size_t k = 0; k < seen.size(); ++k) if (seen[k] == l) dup = true; if (dup) continue; seen.push_back(l);
        Json o = Json::object(); o["name"] = nd.name; o["path"] = "\\\\" + j->host + "\\" + nd.name; o["kind"] = "share"; if (!nd.comment.empty()) o["comment"] = nd.comment; shares.push_back(o);
    }
    bool truncated = false; PutEntries(j->result, shares, kMaxBrowseEntries, truncated); if (truncated) j->result["truncated"] = true;
    if (shares.empty()) j->result["hint"] = "No shared folders were listed on this server (hidden shares are not shown). If you know the share name, type the whole path (\\\\server\\share).";
    j->ok = true;
}

void BrowseFolder(BrowseJob* j)
{
    SmbConnection conn; Str e; bool tr = false;
    if (!conn.open(j->root, j->user, j->pass, false, e, tr)) { j->err = e; return; }
    if (j->abandoned) return;
    Str share = j->root.substr((size_t)j->root.rfind('\\') + 1);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileExW(FilePath(SmbMapPath(j->path) + "\\*"), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_BAD_NETPATH) j->err = "the folder " + j->path + " cannot be opened: " + WinErrorText(code);
        else if (code == ERROR_DIRECTORY) j->err = j->path + " is not a folder";
        else FailText(j, code, share);
        return;
    }
    Vec<Json> dirs; int scanned = 0; bool truncated = false;
    do {
        if (j->abandoned) break;
        if (++scanned > kMaxScanned) { truncated = true; break; }
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Str name = FromWide(fd.cFileName);
        Str full = j->path + "\\" + name, norm, ce; if (!SmbNormalizeUnc(full, norm, ce)) continue;       // a name the validator refuses cannot be used as a target folder
        if (dirs.size() >= (size_t)kMaxBrowseEntries) { truncated = true; break; }
        Json o = Json::object(); o["name"] = name; o["path"] = norm; o["kind"] = "folder"; dirs.push_back(o);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    PutEntries(j->result, dirs, kMaxBrowseEntries, truncated); if (truncated) j->result["truncated"] = true;
    j->ok = true;
}

DWORD WINAPI BrowseThread(LPVOID p)
{
    BrowseJob* j = (BrowseJob*)p;
    if (j->level == 0) BrowseRoot(j); else if (j->level == 1) BrowseServer(j); else BrowseFolder(j);
    Wipe(j->pass); Wipe(j->user);
    SetEvent(j->done);
    InterlockedDecrement(&g_liveWorkers);
    ReleaseJob(j);
    return 0;
}

} // namespace

bool NetBrowse(const Str& pathIn, const Str& userIn, const Str& password, Json& result, Str& err)
{
    result = Json::object();
    Str user = userIn.trim();
    if (user.size() > 256 || user.find('\n') >= 0 || user.find('\r') >= 0 || user.find('\0') >= 0 || !Utf8Valid(user.data(), user.size())) { err = "the user name is not valid"; return false; }
    if (password.size() > 1024 || password.find('\0') >= 0) { err = "the password is not valid"; return false; }
    Str p = pathIn.trim().replace_all("/", "\\"); while (p.size() > 2 && p.ends_with("\\")) p = p.substr(0, p.size() - 1);
    if (p.size() > 2048) { err = "the path is too long"; return false; }
    BrowseJob* j = new BrowseJob(); Str e; j->level = 0;
    if (!p.empty()) {
        if (!p.starts_with("\\\\")) { err = SmbHasDriveLetter(p) ? "this is a drive letter, not a network path: use \\\\server\\share" : "this is not a network path: use \\\\server or \\\\server\\share"; delete j; return false; }
        Vec<Str> parts; { size_t from = 2; for (size_t i = 2; i <= p.size(); ++i) if (i == p.size() || p[i] == '\\') { parts.push_back(p.substr(from, i - from)); from = i + 1; } }
        bool good;
        if (parts.size() == 1) { Str n, e2; good = SmbNormalizeUnc(p + "\\x", n, e2); e = e2; j->level = 1; j->host = parts[0]; j->path = "\\\\" + parts[0]; }
        else { Str n, e2; good = SmbNormalizeUnc(p, n, e2); e = e2; if (good) { Str root, sub, host; SmbSplitUnc(n, root, sub, host); j->host = host; j->root = root; j->path = n; j->level = sub.empty() ? 2 : 3; } }
        if (!good) { err = e; delete j; return false; }
    }
    if (j->level == 1 || j->level == 2 || j->level == 3) {
        if (!user.empty() && SmbServerHeld(j->host)) user.clear();       // Windows allows one set of credentials per server: the connection of a running backup is used as it is, never replaced
        j->user = user; j->pass = user.empty() ? Str() : password;
    }
    if (InterlockedIncrement(&g_liveWorkers) > kMaxLiveWorkers) { InterlockedDecrement(&g_liveWorkers); delete j; err = "the network lookups are busy (earlier ones are still waiting for the network): try again in a minute (1460)"; return false; }
    int deadline = g_browseDeadlineMs; j->budgetMs = deadline > 1500 ? (DWORD)deadline - 1000 : (DWORD)deadline / 2;
    HANDLE th = CreateThread(nullptr, 0, BrowseThread, j, 0, nullptr);
    if (!th) { InterlockedDecrement(&g_liveWorkers); delete j; err = "cannot start the network lookup"; return false; }
    CloseHandle(th);
    DWORD w = WaitForSingleObject(j->done, (DWORD)deadline);
    bool ok;
    if (w != WAIT_OBJECT_0) {                                          // the worker keeps its own copies, wipes them, cancels its connection and ends by itself
        InterlockedExchange(&j->abandoned, 1);
        if (j->level == 0 && j->partialReady) {                         // the NetBIOS walk is stuck, but the domain's servers were already found: answer with them
            result = j->partial; result["path"] = ""; result["kind"] = "root"; result["parent"] = ""; if (!result.has("truncated")) result["truncated"] = false;
            ReleaseJob(j); return true;
        }
        ReleaseJob(j);
        err = Str::fmt("the network did not answer within %d seconds: this is temporary, try again (1460)", (deadline + 500) / 1000); return false;
    }
    ok = j->ok; if (ok) {
        result = j->result;
        result["path"] = j->path; result["kind"] = j->level == 0 ? "root" : j->level == 1 ? "server" : j->level == 2 ? "share" : "folder";
        Str parent; if (j->level == 3) { int c = j->path.rfind('\\'); parent = j->path.substr(0, (size_t)c); } else if (j->level == 2) parent = "\\\\" + j->host;
        result["parent"] = parent;
        if (!result.has("truncated")) result["truncated"] = false;
    } else err = j->err.empty() ? Str("the network lookup failed") : j->err;
    ReleaseJob(j);
    return ok;
}
