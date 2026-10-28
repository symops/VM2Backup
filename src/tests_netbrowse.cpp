// Autotests of net.drives / net.browse (group "smb"): a fake network (servers, shares, users, delays, errors) behind SmbSetApi + NetBrowseSetApi.
// What stays unverified here: the real registry, LookupAccountSidW, WNetOpenEnumW, NetShareEnum and real SMB servers (see docs/INSTALL.en.txt).
#include "autotest.h"
#include "netbrowse.h"
#include "tests_svc.h"

namespace vm2b {
namespace {

struct FShare { Str name, local; };
struct FServer {
    Str name, user, pass;                // user empty = open to everyone
    Vec<FShare> shares; int delayMs; unsigned long failCode; bool noWnetShares;   // noWnetShares: WNetEnum fails, NetShareEnum works
    FServer() : delayMs(0), failCode(0), noWnetShares(false) {}
};
struct FNet {
    CRITICAL_SECTION cs; Vec<FServer> servers; Vec<Str> domains[2]; Str log, passwordSeen;   // log of every call (the password must never be in it: we log only user names)
    Vec<Str> conns; int rootDelay; unsigned long rootRc; int adds, cancels, scopeCalls;
    Vec<Str> sids; Vec<NetRegDrive> reg[4]; Str sidNames[4]; Vec<NetRegDrive> own; Str ownOwner;
    Str domainName; Vec<NetNode> domainNodes; unsigned long domainRc; int domainDelay, domainCalls;      // the fake directory of the domain (enumDomain); domainRc 1355 = not a domain member
    FNet() : rootDelay(0), rootRc(0), adds(0), cancels(0), scopeCalls(0), domainRc(1355), domainDelay(0), domainCalls(0) { InitializeCriticalSection(&cs); }
};
FNet* g_n = nullptr;

Str Lower(const Str& s) { return s.lower(); }
FServer* Find(const Str& host) { for (size_t i = 0; i < g_n->servers.size(); ++i) if (Lower(g_n->servers[i].name) == Lower(host)) return &g_n->servers[i]; return nullptr; }
Str HostOf(const Str& r) { Str t = r.substr(2); int c = t.find('\\'); return c >= 0 ? t.substr(0, (size_t)c) : t; }

unsigned long FAdd(const wchar_t* remote, const wchar_t* user, const wchar_t* pass)
{
    Str r = FromWide(remote), u = user ? FromWide(user) : Str(), p = pass ? FromWide(pass) : Str();
    EnterCriticalSection(&g_n->cs); ++g_n->adds; g_n->log += "add " + r + " user=" + (user ? u : Str("<none>")) + "\n";
    FServer* s = Find(HostOf(r)); int delay = s ? s->delayMs : 0; unsigned long rc = 0;
    if (!s) rc = 53; else if (s->failCode) rc = s->failCode;
    else if (!s->user.empty() && (!user || u != s->user || p != s->pass)) rc = user ? 1326 : 5;
    LeaveCriticalSection(&g_n->cs);
    if (delay) Sleep((DWORD)delay);
    if (rc) return rc;
    EnterCriticalSection(&g_n->cs); g_n->conns.push_back(HostOf(r).lower()); LeaveCriticalSection(&g_n->cs);
    return 0;
}
unsigned long FCancel(const wchar_t* remote, int force)
{
    Str h = HostOf(FromWide(remote)).lower(); EnterCriticalSection(&g_n->cs); ++g_n->cancels; unsigned long rc = 2250;
    for (size_t i = 0; i < g_n->conns.size(); ++i) if (g_n->conns[i] == h) { g_n->conns.erase(i); rc = 0; break; }
    LeaveCriticalSection(&g_n->cs); return rc;
}
Str FMap(const Str& unc)
{
    Str t = unc.substr(2); Vec<Str> parts; size_t from = 0;
    for (size_t i = 0; i <= t.size(); ++i) if (i == t.size() || t[i] == '\\') { parts.push_back(t.substr(from, i - from)); from = i + 1; }
    FServer* s = Find(parts[0]); if (!s || parts.size() < 2) return unc;
    for (size_t i = 0; i < s->shares.size(); ++i) if (Lower(s->shares[i].name) == Lower(parts[1])) {
        Str r = s->shares[i].local; for (size_t k = 2; k < parts.size(); ++k) r += "\\" + parts[k]; return r;
    }
    return unc;
}
const SmbApi kSmb = { FAdd, FCancel, FMap };

bool FSids(Vec<Str>& v) { v = g_n->sids; return true; }
bool FRegDrives(const Str& sid, Vec<NetRegDrive>& out) { for (size_t i = 0; i < g_n->sids.size(); ++i) if (g_n->sids[i] == sid) { out = g_n->reg[i]; return true; } return false; }
Str FSidName(const Str& sid) { for (size_t i = 0; i < g_n->sids.size(); ++i) if (g_n->sids[i] == sid) return g_n->sidNames[i]; return Str(); }
void FOwn(Vec<NetRegDrive>& out, Str& owner) { out = g_n->own; owner = g_n->ownOwner; }
unsigned long FEnumNet(const Str& token, int scope, Vec<NetNode>& out)
{
    if (token.starts_with("\\\\")) {                                          // the shares of a server
        FServer* s = Find(token.substr(2)); if (!s) return 53; if (s->noWnetShares) return 1203;
        if (s->delayMs && false) Sleep(0);
        for (size_t i = 0; i < s->shares.size(); ++i) { NetNode n; n.kind = 2; n.name = s->shares[i].name; n.comment = "share " + s->shares[i].name; out.push_back(n); }
        // a hostile server also announces a bad name and a hidden share
        NetNode bad; bad.kind = 2; bad.name = "bad:name"; out.push_back(bad); NetNode hid; hid.kind = 2; hid.name = "C$"; out.push_back(hid);
        return 0;
    }
    if (g_n->rootDelay) Sleep((DWORD)g_n->rootDelay);
    EnterCriticalSection(&g_n->cs); g_n->scopeCalls++; LeaveCriticalSection(&g_n->cs);
    if (g_n->rootRc && token.empty()) return g_n->rootRc;
    if (token.empty()) {
        if (scope == 1) return 0;                                              // the context scope is empty in this fake
        NetNode top; top.kind = 0; top.name = "Microsoft Windows Network"; top.token = "NET"; out.push_back(top); return 0;
    }
    if (token == "NET") { for (size_t i = 0; i < 2; ++i) if (g_n->domains[i].size()) { NetNode d; d.kind = 0; d.name = i ? "CORP" : "WORKGROUP"; d.token = i ? "D1" : "D0"; out.push_back(d); } return 0; }
    int di = token == "D0" ? 0 : token == "D1" ? 1 : -1; if (di < 0) return 87;
    for (size_t i = 0; i < g_n->domains[di].size(); ++i) { NetNode n; n.kind = 1; n.name = g_n->domains[di][i]; n.comment = "computer " + n.name; out.push_back(n); }
    return 0;
}
unsigned long FEnumShares(const Str& server, Vec<NetNode>& out)
{
    FServer* s = Find(server.substr(2)); if (!s) return 53;
    for (size_t i = 0; i < s->shares.size(); ++i) { NetNode n; n.kind = 2; n.name = s->shares[i].name; out.push_back(n); }
    return 0;
}
unsigned long FEnumDomain(Str& domain, Vec<NetNode>& out)
{
    if (g_n->domainDelay) Sleep((DWORD)g_n->domainDelay);
    EnterCriticalSection(&g_n->cs); g_n->domainCalls++; LeaveCriticalSection(&g_n->cs);
    if (g_n->domainRc) return g_n->domainRc;
    domain = g_n->domainName; out = g_n->domainNodes; return 0;
}
const NetApi kNet = { FSids, FRegDrives, FSidName, FOwn, FEnumNet, FEnumShares, FEnumDomain };

const char* const kPw = "Br0wse-Pw-\xc3\xa4-99";

struct Scope {
    Tmp tmp; FNet n; Str nasRoot;
    Scope(Ctx& c, const char* tag) : tmp(c, tag) {
        g_n = &n;
        FServer nas; nas.name = "nas"; FShare b; b.name = "backup"; b.local = tmp.sub("nas-backup"); nas.shares.push_back(b);
        FShare m; m.name = "media"; m.local = tmp.sub("nas-media"); nas.shares.push_back(m); n.servers.push_back(nas);
        FServer dc; dc.name = "dc01"; dc.user = "corp\\admin"; dc.pass = kPw; FShare s; s.name = "secret"; s.local = tmp.sub("dc-secret"); dc.shares.push_back(s); n.servers.push_back(dc);
        FServer slow; slow.name = "slow"; slow.delayMs = 700; FShare z; z.name = "z"; z.local = tmp.sub("slow-z"); slow.shares.push_back(z); n.servers.push_back(slow);
        FServer nowm; nowm.name = "legacy"; nowm.noWnetShares = true; FShare l; l.name = "old"; l.local = tmp.sub("legacy-old"); nowm.shares.push_back(l); n.servers.push_back(nowm);
        FServer down; down.name = "down"; down.failCode = 1231; n.servers.push_back(down);
        n.domains[0].push_back("nas"); n.domains[0].push_back("dc01"); n.domains[0].push_back("slow"); n.domains[1].push_back("legacy"); n.domains[1].push_back("NAS"); n.domains[1].push_back("bad name!");
        Str e; MakeDirs(tmp.sub("nas-backup\\vm2b\\sub1"), e); MakeDirs(tmp.sub("nas-backup\\Alpha"), e); MakeDirs(tmp.sub("nas-backup\\beta"), e); MakeDirs(tmp.sub("nas-backup\\hid"), e); MakeDirs(tmp.sub("nas-backup\\sys"), e);
        WriteBytes(tmp.sub("nas-backup\\file.txt"), Bytes());
        SetFileAttributesW(Wide(tmp.sub("nas-backup\\hid")).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY);
        SetFileAttributesW(Wide(tmp.sub("nas-backup\\sys")).c_str(), FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY);
        MakeDirs(tmp.sub("dc-secret\\payroll"), e); MakeDirs(tmp.sub("slow-z\\q"), e); MakeDirs(tmp.sub("legacy-old\\x"), e);
        SmbSetApi(&kSmb); NetBrowseSetApi(&kNet); NetBrowseSetDeadlineMs(4000);
    }
    ~Scope() { for (int i = 0; i < 200 && NetBrowseLiveWorkers(); ++i) Sleep(50); SmbSetApi(nullptr); NetBrowseSetApi(nullptr); NetBrowseSetDeadlineMs(0); g_n = nullptr; }
};

bool Has(const Json& entries, const char* name, const char* kind) { for (size_t i = 0; i < entries.size(); ++i) if (entries.at(i).get("name").str() == name && entries.at(i).get("kind").str() == kind) return true; return false; }
Json Args(const char* path, const char* user = nullptr, const char* pass = nullptr) { Json a = Json::object(); a["path"] = path; if (user) a["user"] = user; if (pass) a["password"] = pass; return a; }

} // namespace

VM2B_TEST("smb", netdrives_registry_parsing)
{
    Scope s(c_, "nbdrv"); FNet& n = s.n;
    n.sids.push_back("S-1-5-18"); n.sids.push_back("S-1-5-21-1-2-3-1001"); n.sids.push_back("S-1-5-21-1-2-3-1001_Classes"); n.sids.push_back("S-1-5-21-1-2-3-1002"); n.sids.push_back(".DEFAULT");
    n.reg[0].push_back(NetRegDrive{ "Z", "\\\\nas\\backup", "" });             // would be skipped: S-1-5-18
    n.reg[1].push_back(NetRegDrive{ "Z", "\\\\nas\\backup", "CORP\\bob" }); n.reg[1].push_back(NetRegDrive{ "y", "\\\\srv\\vms\\sub", "0" });
    n.reg[1].push_back(NetRegDrive{ "X", "", "" }); n.reg[1].push_back(NetRegDrive{ "W", "C:\\local", "" }); n.reg[1].push_back(NetRegDrive{ "VV", "\\\\a\\b", "" });
    n.reg[1].push_back(NetRegDrive{ "U", "\\\\?\\UNC\\a\\b", "" }); n.reg[1].push_back(NetRegDrive{ "T", "\\\\h\\s\\..\\x", "" }); n.reg[1].push_back(NetRegDrive{ "Z", "\\\\nas\\backup", "CORP\\bob" });
    n.sidNames[1] = "CORP\\bob"; n.sids[2] = "S-1-5-21-1-2-3-1001_Classes";
    n.reg[3].push_back(NetRegDrive{ "R", "\\\\files\\home", "" }); n.sidNames[3] = "";           // account name unknown: the SID is the owner
    n.own.push_back(NetRegDrive{ "Q", "\\\\nas\\media", "" }); n.ownOwner = "SYSTEM";
    Json r; Str e; T_CHECK(NetListDrives(r, e));
    const Json& d = r.get("drives"); T_CHECK(d.size() == 4);
    T_CHECK(d.at(0).get("letter").str() == "Z:" && d.at(0).get("unc").str() == "\\\\nas\\backup" && d.at(0).get("user").str() == "CORP\\bob" && d.at(0).get("owner").str() == "CORP\\bob" && d.at(0).get("source").str() == "profile");
    T_CHECK(d.at(1).get("letter").str() == "Y:" && d.at(1).get("unc").str() == "\\\\srv\\vms\\sub" && d.at(1).get("user").str().empty());
    T_CHECK(d.at(2).get("owner").str() == "S-1-5-21-1-2-3-1002" && d.at(2).get("letter").str() == "R:");
    T_CHECK(d.at(3).get("source").str() == "service" && d.at(3).get("owner").str() == "SYSTEM" && d.at(3).get("letter").str() == "Q:");
    T_CHECK(r.dump().find("pass") < 0);
    return true;
}

VM2B_TEST("smb", netbrowse_levels)
{
    Scope s(c_, "nblev"); Json r; Str e;
    // root: the neighbourhood is walked (domains -> servers), de-duplicated, bad names dropped
    T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("kind").str() == "root" && r.get("parent").str().empty() && r.get("path").str().empty());
    const Json& ent = r.get("entries"); T_CHECK(ent.size() == 4); T_CHECK(Has(ent, "nas", "server") && Has(ent, "dc01", "server") && Has(ent, "slow", "server") && Has(ent, "legacy", "server"));
    T_CHECK(ent.at(0).get("name").str() == "dc01"); T_CHECK(ent.at(0).get("path").str() == "\\\\dc01");
    T_CHECK(r.get("truncated").boolean() == false && !r.has("hint"));
    // server: disk shares only; the hidden C$ and the invalid name are not shown; WNet failing -> NetShareEnum fallback
    T_CHECK(NetBrowse("\\\\nas", "", "", r, e)); T_CHECK(r.get("kind").str() == "server" && r.get("parent").str().empty());
    T_CHECK(r.get("entries").size() == 2 && Has(r.get("entries"), "backup", "share") && Has(r.get("entries"), "media", "share"));
    T_CHECK(r.get("entries").at(0).get("path").str() == "\\\\nas\\backup");
    T_CHECK(NetBrowse("//LEGACY/", "", "", r, e)); T_CHECK(r.get("entries").size() == 1 && Has(r.get("entries"), "old", "share")); T_CHECK(r.get("path").str() == "\\\\LEGACY");
    // share and folder: sub-folders only, hidden / system hidden, sorted without case, files absent
    T_CHECK(NetBrowse("\\\\nas\\backup", "", "", r, e)); T_CHECK(r.get("kind").str() == "share" && r.get("parent").str() == "\\\\nas");
    const Json& f = r.get("entries"); T_CHECK(f.size() == 3); T_CHECK(f.at(0).get("name").str() == "Alpha" && f.at(1).get("name").str() == "beta" && f.at(2).get("name").str() == "vm2b");
    T_CHECK(f.at(2).get("path").str() == "\\\\nas\\backup\\vm2b" && f.at(2).get("kind").str() == "folder");
    T_CHECK(NetBrowse("\\\\nas\\backup\\vm2b\\", "", "", r, e)); T_CHECK(r.get("kind").str() == "folder" && r.get("parent").str() == "\\\\nas\\backup" && Has(r.get("entries"), "sub1", "folder"));
    T_CHECK(NetBrowse("\\\\nas\\backup\\vm2b\\sub1", "", "", r, e)); T_CHECK(r.get("entries").size() == 0 && r.get("parent").str() == "\\\\nas\\backup\\vm2b");
    // a folder that is not there; a share that is not there; a server that is not there
    T_CHECK(!NetBrowse("\\\\nas\\backup\\nope", "", "", r, e)); T_CHECK(e.find("cannot be opened") >= 0);
    T_CHECK(!NetBrowse("\\\\ghost", "", "", r, e)); T_CHECK(e.find("not found") >= 0 && e.find("(53)") >= 0);
    T_CHECK(!NetBrowse("\\\\down\\x", "", "", r, e)); T_CHECK(SmbErrorCodeOf(e) == 1231);
    T_CHECK(s.n.conns.size() == 0);                                             // every temporary connection was cancelled
    // an empty neighbourhood: empty list plus a hint, not an error; a failing one too
    s.n.domains[0].clear(); s.n.domains[1].clear(); T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("entries").size() == 0 && r.get("hint").str().find("server") >= 0);
    s.n.rootRc = 1222; T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("entries").size() == 0 && r.get("hint").str().find("1222") >= 0);
    return true;
}

NetNode DomNode(const char* name, const char* comment) { NetNode n; n.kind = 1; n.name = name; n.comment = comment; return n; }

VM2B_TEST("smb", netbrowse_root_in_a_domain_lists_the_domain_servers)
{
    // a modern domain member: the NetBIOS list is empty, the root shows the controller and the servers of the domain (DsGetDcName + LDAP)
    Scope s(c_, "nbdom"); Json r; Str e; FNet& n = s.n;
    n.domains[0].clear(); n.domains[1].clear();
    n.domainRc = 0; n.domainName = "corp.example";
    n.domainNodes.push_back(DomNode("dc01.corp.example", "domain controller")); n.domainNodes.push_back(DomNode("files01.corp.example", "Windows Server 2022 Standard"));
    n.domainNodes.push_back(DomNode("dc01.corp.example", "again")); n.domainNodes.push_back(DomNode("bad name!.corp.example", "hostile")); n.domainNodes.push_back(DomNode("nas", "short name only"));
    T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("kind").str() == "root");
    const Json& ent = r.get("entries"); c_.note("domain root: %d entries: %s", (int)ent.size(), ent.dump().c_str());
    T_CHECK(ent.size() == 3); T_CHECK(Has(ent, "dc01.corp.example", "server") && Has(ent, "files01.corp.example", "server") && Has(ent, "nas", "server"));
    T_CHECK(r.get("domain").str() == "corp.example"); T_CHECK(!r.has("hint")); T_CHECK(n.domainCalls == 1);
    T_CHECK(ent.at(0).get("path").str() == "\\\\dc01.corp.example"); T_CHECK(ent.at(0).get("comment").str() == "domain controller");
    // the WNet list and the directory name the same server (short and full name): one entry, the WNet one stays
    n.domains[0].push_back("dc01"); n.domains[0].push_back("legacy");
    T_CHECK(NetBrowse("", "", "", r, e)); const Json& e2 = r.get("entries");
    T_CHECK(e2.size() == 4); T_CHECK(Has(e2, "dc01", "server") && Has(e2, "legacy", "server") && Has(e2, "files01.corp.example", "server") && Has(e2, "nas", "server") && !Has(e2, "dc01.corp.example", "server"));
    // a server of the domain is browsable like any other (its shares)
    T_CHECK(NetBrowse("\\\\nas", "", "", r, e)); T_CHECK(Has(r.get("entries"), "backup", "share"));
    return true;
}

VM2B_TEST("smb", netbrowse_root_answers_with_the_domain_when_the_netbios_walk_hangs)
{
    // found on the real domain (Server 2022): the NetBIOS walk of the root takes longer than the deadline, but the directory answers at once: the servers of the domain are returned
    Scope s(c_, "nbdomslow"); Json r; Str e; FNet& n = s.n;
    n.domains[0].clear(); n.domains[1].clear(); n.domainRc = 0; n.domainName = "corp.example";
    n.domainNodes.push_back(DomNode("dc01.corp.example", "domain controller")); n.domainNodes.push_back(DomNode("files01.corp.example", "Windows Server 2022"));
    n.rootDelay = 6000; NetBrowseSetDeadlineMs(2500);
    DWORD t0 = GetTickCount(); bool ok = NetBrowse("", "", "", r, e); DWORD dt = GetTickCount() - t0;
    c_.note("a hanging NetBIOS walk: %s after %lu ms, %d entries [%s]", ok ? "ok" : "error", (unsigned long)dt, ok ? (int)r.get("entries").size() : 0, e.c_str());
    T_CHECK(ok); T_CHECK(dt < 4500); T_CHECK(r.get("kind").str() == "root"); T_CHECK(r.get("entries").size() == 2 && Has(r.get("entries"), "dc01.corp.example", "server"));
    T_CHECK(r.get("domain").str() == "corp.example"); T_CHECK(r.get("hint").str().find("incomplete") >= 0);
    return true;
}

VM2B_TEST("smb", netbrowse_root_remembers_the_last_directory_answer)
{
    // the directory lookup is slower than one request may wait: the first request times out, the worker finishes in the background, the next request answers at once from what was found
    Scope s(c_, "nbdomcache"); Json r; Str e; FNet& n = s.n;
    n.domains[0].clear(); n.domains[1].clear(); n.domainRc = 0; n.domainName = "corp.example";
    n.domainNodes.push_back(DomNode("dc01.corp.example", "domain controller"));
    n.domainDelay = 2500; NetBrowseSetDeadlineMs(1500);
    T_CHECK(!NetBrowse("", "", "", r, e)); T_CHECK(e.find("1460") >= 0);
    for (int i = 0; i < 100 && n.domainCalls < 1; ++i) Sleep(50);
    Sleep(2800);                                                                    // the abandoned worker has stored its answer
    n.domainDelay = 6000;
    DWORD t0 = GetTickCount(); bool ok = NetBrowse("", "", "", r, e); DWORD dt = GetTickCount() - t0;
    c_.note("second request, the directory is slow again: %s after %lu ms, %d entries", ok ? "ok" : e.c_str(), (unsigned long)dt, ok ? (int)r.get("entries").size() : 0);
    T_CHECK(ok); T_CHECK(dt < 3500); T_CHECK(r.get("entries").size() == 1 && Has(r.get("entries"), "dc01.corp.example", "server"));
    return true;
}

VM2B_TEST("smb", netbrowse_root_without_a_domain_keeps_the_hint)
{
    // no domain (workgroup): the old behaviour, the empty root explains itself; a directory that fails or is slow changes nothing
    Scope s(c_, "nbnodom"); Json r; Str e; FNet& n = s.n;
    n.domains[0].clear(); n.domains[1].clear(); n.domainRc = 1355;
    T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("entries").size() == 0); T_CHECK(r.get("hint").str().find("server") >= 0); T_CHECK(!r.has("domain")); T_CHECK(n.domainCalls == 1);
    n.domains[0].push_back("nas"); T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("entries").size() == 1 && !r.has("hint") && !r.has("domain"));
    n.domainRc = 1355 + 2; T_CHECK(NetBrowse("", "", "", r, e)); T_CHECK(r.get("entries").size() == 1);             // the directory is unreachable: the WNet answer stands
    n.domainRc = 0; n.domainName = "corp.example"; n.domainDelay = 6000; NetBrowseSetDeadlineMs(2500);
    DWORD t0 = GetTickCount(); bool ok = NetBrowse("", "", "", r, e); DWORD dt = GetTickCount() - t0;
    c_.note("a slow directory: %s after %lu ms [%s]", ok ? "ok" : "error", (unsigned long)dt, e.c_str());
    T_CHECK(dt < 4500); T_CHECK(ok || e.find("1460") >= 0);                                                          // the deadline holds: no hanging request
    return true;
}

VM2B_TEST("smb", netbrowse_credentials_never_leak)
{
    Scope s(c_, "nbcred"); Json r; Str e; Str all;
    // wrong password / no password: permanent errors, the secret is nowhere
    T_CHECK(!NetBrowse("\\\\dc01", "corp\\admin", "wrong-Pw-11", r, e)); all += e + r.dump(); T_CHECK(SmbErrorCodeOf(e) == 1326 && !SmbIsTransientCode(1326)); T_CHECK(e.find("wrong") >= 0);
    T_CHECK(!NetBrowse("\\\\dc01\\secret", "", "", r, e)); all += e; T_CHECK(SmbErrorCodeOf(e) == 5);
    T_CHECK(NetBrowse("\\\\dc01", "corp\\admin", kPw, r, e)); all += r.dump(); T_CHECK(Has(r.get("entries"), "secret", "share"));
    T_CHECK(NetBrowse("\\\\dc01\\secret", "corp\\admin", kPw, r, e)); all += r.dump(); T_CHECK(Has(r.get("entries"), "payroll", "folder"));
    T_CHECK(!NetBrowse("\\\\dc01\\secret\\payroll", "corp\\admin", "wrong-Pw-11", r, e)); all += e;
    T_CHECK(all.find("wrong-Pw-11") < 0 && all.find("Br0wse") < 0);
    T_CHECK(s.n.log.find("Br0wse") < 0 && s.n.log.find("wrong-Pw") < 0);
    T_CHECK(s.n.conns.size() == 0);
    // a server that one of our connections is open to keeps its connection: the credentials are not used to replace it
    { SmbConnection hold; bool tr; Str e2; T_CHECK(hold.open("\\\\nas\\backup", "", "", false, e2, tr));
      int before = s.n.adds; T_CHECK(NetBrowse("\\\\nas\\backup", "someone", "x", r, e)); T_CHECK(s.n.log.find("user=someone") < 0); T_CHECK(s.n.adds > before); }
    // bad user names / passwords are refused before any call
    Str longu; for (int i = 0; i < 300; ++i) longu += 'u';
    int adds = s.n.adds; T_CHECK(!NetBrowse("\\\\nas", "a\nb", "", r, e)); T_CHECK(!NetBrowse("\\\\nas", "ok", Str("p\0q", 3), r, e)); T_CHECK(!NetBrowse("\\\\nas", longu, "", r, e)); T_CHECK(s.n.adds == adds);
    return true;
}

VM2B_TEST("smb", netbrowse_timeout_and_workers)
{
    Scope s(c_, "nbtime"); Json r; Str e; NetBrowseSetDeadlineMs(250);
    DWORD t0 = GetTickCount();
    T_CHECK(!NetBrowse("\\\\slow", "u", "Secret-Timeout-1", r, e)); T_CHECK(GetTickCount() - t0 < 600);
    T_CHECK(SmbIsTransientCode(SmbErrorCodeOf(e)) && e.find("temporary") >= 0 && e.find("Secret-Timeout") < 0);
    T_CHECK(NetBrowseLiveWorkers() >= 0);
    for (int i = 0; i < 100 && NetBrowseLiveWorkers(); ++i) Sleep(30);
    T_CHECK(NetBrowseLiveWorkers() == 0); T_CHECK(s.n.conns.size() == 0);            // the abandoned worker cancelled its connection by itself
    T_CHECK(s.n.log.find("Secret-Timeout") < 0);
    // a slow root walk gives the partial list or the timeout, never a hang
    s.n.rootDelay = 400; t0 = GetTickCount(); bool ok = NetBrowse("", "", "", r, e); T_CHECK(GetTickCount() - t0 < 900); if (!ok) T_CHECK(SmbIsTransientCode(SmbErrorCodeOf(e)));
    // too many hung lookups: refused at once with a temporary error
    NetBrowseSetDeadlineMs(60); int refused = 0;
    for (int i = 0; i < 14; ++i) { if (!NetBrowse("\\\\slow", "", "", r, e) && e.find("busy") >= 0) ++refused; }
    T_CHECK(refused > 0);
    return true;
}

VM2B_TEST("smb", netbrowse_hostile_and_truncation)
{
    Scope s(c_, "nbhost"); Json r; Str e;
    static const char* const bad[] = { "C:\\x", "Z:", "relative\\x", "\\x", "\\\\?\\C:\\x", "\\\\.\\pipe\\x", "\\\\?\\UNC\\h\\s", "\\\\nas\\backup\\..", "\\\\nas\\backup\\a\\..\\b", "\\\\nas\\..\\x", "\\\\..", "\\\\.",
        "\\\\ho st", "\\\\nas;calc", "\\\\nas&calc\\x", "\\\\nas|x", "\\\\nas\\ba:ckup", "\\\\nas\\backup\\a*", "\\\\nas\\backup\\CON", "\\\\nas\\backup\\x.", "\\\\[::1]", "\\\\fe80::1\\x", "\\\\\\nas", "http://x/y", "\\\\nas\\ba\x01" "ckup", "\\\\nas\\backup\\\xff" };
    int adds = s.n.adds;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) { Json x; Str ee; if (NetBrowse(bad[i], "", "", x, ee)) { c_.note("accepted: %s", bad[i]); return false; } if (ee.empty()) return false; }
    T_CHECK(s.n.adds == adds);                                                   // nothing hostile reached the network layer
    Str longp = "\\\\nas\\backup"; for (int i = 0; i < 400; ++i) longp += "\\dir"; T_CHECK(!NetBrowse(longp, "", "", r, e));
    // truncation: more than 5000 sub-folders
    Str base = s.tmp.sub("nas-backup\\many"); Str ee; MakeDirs(base, ee);
    for (int i = 0; i < 5100; ++i) CreateDirectoryW(FilePath(Str::fmt("%s\\d%05d", base.c_str(), i)), nullptr);
    NetBrowseSetDeadlineMs(20000);
    T_CHECK(NetBrowse("\\\\nas\\backup\\many", "", "", r, e)); T_CHECK(r.get("entries").size() == 5000 && r.get("truncated").boolean());
    return true;
}

VM2B_TEST("smb", netbrowse_through_service)
{
    Scope s(c_, "nbsvc"); Svc v(c_, "nbsvc2"); if (!v.up) return false; v.record = true;
    s.n.sids.push_back("S-1-5-21-9-9-9-1001"); s.n.reg[0].push_back(NetRegDrive{ "Z", "\\\\dc01\\secret", "corp\\admin" }); s.n.sidNames[0] = "CORP\\admin";
    Json d = v.ok("net.drives", Json::object()); T_CHECK(d.get("drives").size() == 1 && d.get("drives").at(0).get("unc").str() == "\\\\dc01\\secret");
    Json r = v.ok("net.browse", Args("")); T_CHECK(r.get("kind").str() == "root" && r.get("entries").size() == 4);
    r = v.ok("net.browse", Args("\\\\dc01\\secret", "corp\\admin", kPw)); T_CHECK(Has(r.get("entries"), "payroll", "folder"));
    T_CHECK(!v.call("net.browse", Args("\\\\dc01\\secret", "corp\\admin", "wrong-Pw-22"))); T_CHECK(v.lastErr.find("password") >= 0);
    T_CHECK(!v.call("net.browse", Args("\\\\?\\C:\\"))); T_CHECK(!v.call("net.browse", Args("..\\..")));
    T_CHECK(v.transcript.find("wrong-Pw-22") < 0 && v.transcript.find("Br0wse") < 0);
    T_CHECK(v.core.runningJobs() == 0);
    return true;
}

} // namespace vm2b
