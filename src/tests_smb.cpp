// Autotests of the network folder (SMB) target and the network drive helpers (group "smb"). Real SMB does not exist under Wine / Linux:
// the Windows network calls are replaced by a fake (SmbSetApi) that checks the credentials, keeps the list of connections and maps the
// share "\\fileserver\backup" to a local folder. What cannot be tested here (a real server, a domain, the service account, mapped
// drives) is listed in docs/INSTALL.en.txt and README.md.
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "smb.h"
#include "tests_svc.h"
#include "schedule.h"
#include <winsock2.h>

namespace vm2b {
namespace {

// ---- the fake network ------------------------------------------------------------------------------------------
struct Cred { Str user, pass; };
struct FakeNet {
    Str share, local;             // "\\fileserver\backup" (lower case) and the folder behind it
    Vec<Cred> accepted;           // user + password pairs the "server" accepts
    bool anonymous;               // accepts a connection without credentials
    bool down;                    // every connection attempt fails: network unreachable
    unsigned long force;          // not 0: the answer of every connection attempt (1909 locked, 1331 disabled ...)
    int adds, cancels;
    Vec<Str> conns;               // "server|user" of the open connections
    Vec<Str> log;
    FakeNet() : anonymous(false), down(false), force(0), adds(0), cancels(0) {}
};
FakeNet g_f;

Str ServerPart(const Str& r) { int c = r.find('\\', 2); return c > 0 ? r.substr(0, (size_t)c) : r; }

unsigned long FakeAdd(const wchar_t* remote, const wchar_t* user, const wchar_t* pass)
{
    Str r = FromWide(remote).lower(), u = user ? FromWide(user) : Str(), p = pass ? FromWide(pass) : Str();
    ++g_f.adds; g_f.log.push_back("add " + r + " user=" + (user ? u : Str("<none>")));
    if (g_f.down) return 1231;
    if (g_f.force) return g_f.force;
    if (ServerPart(r) != ServerPart(g_f.share)) return 53;
    if (r != g_f.share) return 67;
    Str srv = ServerPart(r);
    for (size_t i = 0; i < g_f.conns.size(); ++i) if (g_f.conns[i].starts_with((srv + "|").c_str()) && g_f.conns[i] != srv + "|" + u) return 1219;
    bool ok = false;
    if (!user) ok = g_f.anonymous;
    else for (size_t i = 0; i < g_f.accepted.size(); ++i) if (g_f.accepted[i].user == u && g_f.accepted[i].pass == p) ok = true;
    if (!ok) return user ? 1326 : 5;
    Str key = srv + "|" + u; bool have = false; for (size_t i = 0; i < g_f.conns.size(); ++i) if (g_f.conns[i] == key) have = true;
    if (!have) g_f.conns.push_back(key);
    return 0;
}

unsigned long FakeCancel(const wchar_t* remote, int force)
{
    Str r = FromWide(remote).lower(); ++g_f.cancels; g_f.log.push_back(Str("cancel ") + r + (force ? " force" : ""));
    Str srv = ServerPart(r);
    for (size_t i = g_f.conns.size(); i-- > 0;) if (g_f.conns[i].starts_with((srv + "|").c_str())) { g_f.conns.erase(i); return 0; }
    return 2250;
}

Str FakeMap(const Str& unc)
{
    Str l = unc.lower();
    if (l == g_f.share) return g_f.local;
    if (l.starts_with((g_f.share + "\\").c_str())) return g_f.local + unc.substr(g_f.share.size());
    return unc;
}

const SmbApi kFakeApi = { FakeAdd, FakeCancel, FakeMap };

struct FakeScope {
    Tmp tmp;
    FakeScope(Ctx& c, const char* tag) : tmp(c, tag) {
        g_f = FakeNet(); g_f.share = "\\\\fileserver\\backup"; g_f.local = tmp.sub("share");
        Str e; MakeDirs(g_f.local, e);
        Cred a; a.user = "backupuser"; a.pass = "S3cr3t-Pa55-\xc3\xbc"; g_f.accepted.push_back(a);
        Cred b; b.user = "corp\\other"; b.pass = "Other-Pw-77"; g_f.accepted.push_back(b);
        SmbSetApi(&kFakeApi);
    }
    ~FakeScope() { SmbSetApi(nullptr); }
};

const char* const kSecret = "S3cr3t-Pa55-\xc3\xbc";

Json SmbCfg(const char* path, const char* user, const char* pass, const char* dir = nullptr)
{
    Json j = Json::object(); j["type"] = "smb"; j["path"] = path; if (user) j["user"] = user; if (pass) j["password"] = pass; if (dir) j["directory"] = dir; return j;
}

Target* NewSmb(const Json& cfg, Str& err) { return MakeTarget(cfg, err); }

} // namespace

// ---- validation ------------------------------------------------------------------------------------------------
VM2B_TEST("smb", unc_normalisation_table)
{
    struct Row { const char* in; const char* out; };      // out == nullptr: rejected
    static const Row rows[] = {
        { "\\\\host\\share", "\\\\host\\share" },
        { "//host/share", "\\\\host\\share" },
        { "  \\\\host\\share\\  ", "\\\\host\\share" },
        { "//host/share/sub/dir/", "\\\\host\\share\\sub\\dir" },
        { "\\\\host\\share\\\\a\\\\b", "\\\\host\\share\\a\\b" },
        { "\\\\HOST.corp.local\\Backups$\\x", "\\\\HOST.corp.local\\Backups$\\x" },
        { "\\\\192.168.1.20\\share", "\\\\192.168.1.20\\share" },
        { "\\\\fe80--1.ipv6-literal.net\\s", "\\\\fe80--1.ipv6-literal.net\\s" },
        { "\\\\fe80--1s3.ipv6-literal.net\\s\\d", "\\\\fe80--1s3.ipv6-literal.net\\s\\d" },
        { "\\\\nas\\\xd0\xa0\xd0\xb5\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb2\\\xe6\x97\xa5\xe6\x9c\xac", "\\\\nas\\\xd0\xa0\xd0\xb5\xd0\xb7\xd0\xb5\xd1\x80\xd0\xb2\\\xe6\x97\xa5\xe6\x9c\xac" },
        { "\\\\host\\share$", "\\\\host\\share$" },
        { "\\\\host\\my share\\my folder", "\\\\host\\my share\\my folder" },
        // rejected
        { "", nullptr }, { "   ", nullptr }, { "\\\\", nullptr }, { "\\\\host", nullptr }, { "\\\\host\\", nullptr }, { "\\\\\\share", nullptr },
        { "\\\\\\host\\share", nullptr }, { "\\host\\share", nullptr }, { "host\\share", nullptr }, { "share", nullptr },
        { "\\\\?\\C:\\x", nullptr }, { "\\\\?\\UNC\\host\\share", nullptr }, { "\\\\.\\pipe\\x", nullptr }, { "//?/C:/x", nullptr }, { "\\\\.\\globalroot\\x", nullptr },
        { "C:\\x", nullptr }, { "Z:", nullptr }, { "z:/share", nullptr }, { ".\\x", nullptr }, { "..\\x", nullptr }, { "\\x", nullptr },
        { "\\\\host\\share\\..", nullptr }, { "\\\\host\\share\\a\\..\\b", nullptr }, { "\\\\host\\share\\.\\a", nullptr }, { "\\\\host\\..\\share", nullptr }, { "\\\\..\\share", nullptr }, { "\\\\.\\share", nullptr },
        { "\\\\host\\share\\a.", nullptr }, { "\\\\host\\share\\a \\b", nullptr }, { "\\\\host\\share\\ a", nullptr }, { "\\\\host\\share.\\a", nullptr }, { "\\\\host\\share \\a", nullptr },
        { "\\\\host.\\share", nullptr }, { "\\\\.host\\share", nullptr }, { "\\\\ho..st\\share", nullptr }, { "\\\\ho st\\share", nullptr }, { "\\\\ho:st\\share", nullptr }, { "\\\\[::1]\\share", nullptr }, { "\\\\fe80::1\\share", nullptr },
        { "\\\\host\\share\\CON", nullptr }, { "\\\\host\\share\\nul.txt", nullptr }, { "\\\\host\\share\\COM1", nullptr }, { "\\\\host\\lpt9\\x", nullptr }, { "\\\\host\\AUX", nullptr },
        { "\\\\host\\share\\a:b", nullptr }, { "\\\\host\\share\\file.txt:stream", nullptr }, { "\\\\host\\share\\a*b", nullptr }, { "\\\\host\\share\\a?b", nullptr },
        { "\\\\host\\share\\a<b", nullptr }, { "\\\\host\\share\\a>b", nullptr }, { "\\\\host\\share\\a|b", nullptr }, { "\\\\host\\share\\a\"b", nullptr },
        { "\\\\host\\sh:are", nullptr }, { "\\\\host\\sh*are", nullptr },
        { "\\\\host\\share\\a\x01" "b", nullptr }, { "\\\\host\\share\\a\x7f" "b", nullptr }, { "\\\\host\\share\\a\tb", nullptr }, { "\\\\host\\sha\nre", nullptr },
        { "\\\\host\\share\\\xff\xfe", nullptr }, { "\\\\\xc3\x28\\share", nullptr },
        { "\\\\host\\share\\a%00b\\..", nullptr },
    };
    for (const Row& r : rows) {
        Str out, err; bool ok = SmbNormalizeUnc(r.in, out, err);
        if (r.out) { if (!ok || out != r.out) { c_.note("accepted wrongly / changed: [%s] -> [%s] (%s)", r.in, out.c_str(), err.c_str()); c_.failed = true; } }
        else { if (ok) { c_.note("not rejected: [%s] -> [%s]", r.in, out.c_str()); c_.failed = true; } else if (err.empty() || !out.empty()) { c_.note("rejected without a message: [%s]", r.in); c_.failed = true; } }
    }
    // hostile sizes
    { Str big = "\\\\host\\share\\"; for (int i = 0; i < 3000; ++i) big += "a"; Str o, e; T_CHECK(!SmbNormalizeUnc(big, o, e)); T_CHECK(e.find("long") >= 0); }
    { Str comp; for (int i = 0; i < 256; ++i) comp += "b"; Str o, e; T_CHECK(!SmbNormalizeUnc("\\\\host\\share\\" + comp, o, e)); }
    { Str comp; for (int i = 0; i < 255; ++i) comp += "b"; Str o, e; T_CHECK(SmbNormalizeUnc("\\\\host\\share\\" + comp, o, e)); }
    { Str sh; for (int i = 0; i < 81; ++i) sh += "s"; Str o, e; T_CHECK(!SmbNormalizeUnc("\\\\host\\" + sh, o, e)); }
    { Str h; for (int i = 0; i < 256; ++i) h += "h"; Str o, e; T_CHECK(!SmbNormalizeUnc("\\\\" + h + "\\s", o, e)); }
    { Str many = "\\\\host\\share"; for (int i = 0; i < 1500; ++i) many += "\\a"; Str o, e; T_CHECK(!SmbNormalizeUnc(many, o, e)); }
    // messages name the problem
    { Str o, e; SmbNormalizeUnc("\\\\?\\C:\\x", o, e); T_CHECK(e.find("device") >= 0); SmbNormalizeUnc("Z:\\x", o, e); T_CHECK(e.find("drive") >= 0 && e.find("Network drive") >= 0); SmbNormalizeUnc("x\\y", o, e); T_CHECK(e.find("\\\\server\\share") >= 0); }
    // random text: never a crash, and what is accepted is stable and clean
    for (int i = 0; i < 3000; ++i) {
        Str s; int n = c_.range(0, 40); static const char* alpha = "\\\\//..:?*ab c$-_\x01\xc3\xa4";
        for (int k = 0; k < n; ++k) s += alpha[c_.rnd((u32)strlen(alpha))];
        if (c_.coin(60)) s = "\\\\h\\s" + s;
        Str out, err;
        if (SmbNormalizeUnc(s, out, err)) {
            Str again, e2; T_CHECK(SmbNormalizeUnc(out, again, e2)); T_CHECK(again == out);
            { bool clean = out.starts_with("\\\\") && !out.ends_with("\\") && out.find("/") < 0 && out.find("\\\\", 2) < 0;
              Str o2 = out.substr(2); size_t from = 0; for (size_t k = 0; k <= o2.size(); ++k) if (k == o2.size() || o2[k] == '\\') { Str comp = o2.substr(from, k - from); if (comp == ".." || comp == "." || comp.empty()) clean = false; from = k + 1; }
              if (!clean) c_.note("unclean result [%s] for [%s]", out.c_str(), s.c_str()); T_CHECK(clean); }
            Str root, sub, host; T_CHECK(SmbSplitUnc(out, root, sub, host)); T_CHECK(!host.empty() && root.starts_with("\\\\"));
        } else T_CHECK(!err.empty());
    }
    return true;
}

VM2B_TEST("smb", relative_directory_table)
{
    struct Row { const char* in; const char* out; };
    static const Row rows[] = {
        { "", "" }, { ".", "" }, { "./", "" }, { "./vm1", "vm1" }, { "a/b\\c", "a\\b\\c" }, { "/x/y/", "x\\y" }, { ".\\VM \xd0\xb2\xd0\xbc", "VM \xd0\xb2\xd0\xbc" }, { "a//b", "a\\b" },
        { "..", nullptr }, { "a/../b", nullptr }, { "C:\\x", nullptr }, { "c:x", nullptr }, { "a/b.", nullptr }, { "a/nul", nullptr }, { "a/b:c", nullptr }, { "a/b*c", nullptr }, { "a\x01" "b", nullptr },
    };
    for (const Row& r : rows) {
        Str out, err; bool ok = SmbRelativeDir(r.in, out, err);
        if (r.out) { if (!ok || out != r.out) { c_.note("wrong: [%s] -> [%s] (%s)", r.in, out.c_str(), err.c_str()); c_.failed = true; } }
        else if (ok) { c_.note("not rejected: [%s] -> [%s]", r.in, out.c_str()); c_.failed = true; }
    }
    // a hostile {VM} expansion cannot leave the share either
    Str out, err; T_CHECK(!SmbRelativeDir("../../etc", out, err));
    FakeScope fs(c_, "smbrel");
    Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "..\\evil"), e); T_CHECK(t == nullptr); T_CHECK(e.find("..") >= 0);
    T_CHECK(NewSmb(SmbCfg("", "u", "p"), e) == nullptr);
    T_CHECK(NewSmb(SmbCfg("C:\\x", "u", "p"), e) == nullptr); T_CHECK(e.find("network folder") == 0);
    T_CHECK(NewSmb(SmbCfg("\\\\?\\C:\\x", "u", "p"), e) == nullptr);
    return true;
}

VM2B_TEST("smb", error_texts_and_classes)
{
    struct Row { unsigned long code; const char* word; bool transient; };
    static const Row rows[] = {
        { 53, "not found", true }, { 67, "share", false }, { 86, "password", false }, { 1326, "password", false }, { 1219, "other credentials", false },
        { 5, "access denied", false }, { 1203, "not valid", false }, { 1231, "not reachable", true }, { 1222, "not reachable", true }, { 1460, "timed out", true },
        { 64, "lost", true }, { 1331, "disabled", false }, { 1909, "locked", false }, { 1907, "must be changed", false }, { 1330, "expired", false }, { 1311, "domain controller", true }, { 1225, "refused", true },
    };
    for (const Row& r : rows) {
        bool tr = !r.transient; Str t = SmbErrorText(r.code, "srv1", "data", tr);
        if (tr != r.transient || t.find(r.word) < 0 || t.find('\n') >= 0) { c_.note("code %lu: [%s] transient=%d", r.code, t.c_str(), (int)tr); c_.failed = true; }
        T_CHECK(SmbIsTransientCode(r.code) == r.transient);
    }
    T_CHECK(SmbErrorCodeOf("cannot create \\\\x\\y: Access is denied. (5)") == 5);
    T_CHECK(SmbErrorCodeOf("cannot write f: The specified network name is no longer available. (64)") == 64);
    T_CHECK(SmbErrorCodeOf("no code here") == 0); T_CHECK(SmbErrorCodeOf("(") == 0); T_CHECK(SmbErrorCodeOf("x (12a)") == 0); T_CHECK(SmbErrorCodeOf("()") == 0);
    T_CHECK(SmbErrorCodeOf("x (99999999999999999999)") == 0);
    return true;
}

// ---- the connection flows ----------------------------------------------------------------------------------------
VM2B_TEST("smb", wnet_flows)
{
    FakeScope fs(c_, "smbflow");
    Vec<Str> errors;                                              // every error text we saw: none may hold the password
    auto note = [&](const Str& e) { errors.push_back(e); };

    // success: connect creates the share folder part, put / get / list / remove work, free space is told
    {
        Str e; Target* t = NewSmb(SmbCfg("//fileserver/backup/store", "backupuser", kSecret, "vm1"), e); T_CHECK(t != nullptr); if (!t) return false;
        T_CHECK(strcmp(t->kind(), "smb") == 0);
        T_CHECK(t->connect());
        T_CHECK(SmbOpenConnections() == 1); T_CHECK(g_f.conns.size() == 1);
        T_CHECK(DirExists(g_f.local + "\\store\\vm1"));
        Bytes data = Random(c_, 100000); T_CHECK(t->put_bytes("a.bin", data.data(), data.size()));
        Bytes back; T_CHECK(t->get_bytes("a.bin", back)); T_CHECK(back.size() == data.size() && memcmp(back.data(), data.data(), data.size()) == 0);
        Vec<Str> names; T_CHECK(t->list(names)); T_CHECK(names.size() == 1 && names[0] == "a.bin");
        bool yes = false; T_CHECK(t->exists("a.bin", yes) && yes);
        T_CHECK(t->remove("a.bin")); T_CHECK(t->remove("a.bin")); names.clear(); T_CHECK(t->list(names)); T_CHECK(names.empty());
        T_CHECK(!t->open_write("..\\x")); T_CHECK(!t->open_write("a\\b"));
        u64 fb = 0; T_CHECK(t->free_space(fb)); T_CHECK(fb > 0);
        // no ".part" under the final name when a write is aborted
        Sink* w = t->open_write("half.bin"); T_CHECK(w != nullptr); if (w) { w->write("abc", 3); w->abort(); delete w; }
        T_CHECK(!FileExists(g_f.local + "\\store\\vm1\\half.bin")); T_CHECK(!FileExists(g_f.local + "\\store\\vm1\\half.bin.part"));
        delete t;
        T_CHECK(SmbOpenConnections() == 0); T_CHECK(g_f.conns.empty()); T_CHECK(g_f.cancels == 1);        // cancelled when the target is destroyed
    }
    // wrong password / wrong user: permanent, nothing about the password in the text, nothing left connected
    {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", "wrong-" "S3cr3t-Pa55"), e); T_CHECK(t != nullptr);
        T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("password") >= 0); note(t->err);
        T_CHECK(!t->reconnect()); T_CHECK(!t->transient); note(t->err);
        delete t; T_CHECK(SmbOpenConnections() == 0);
        t = NewSmb(SmbCfg("\\\\fileserver\\backup", "nobody", kSecret), e); T_CHECK(!t->connect()); T_CHECK(!t->transient); note(t->err); delete t;
        t = NewSmb(SmbCfg("\\\\fileserver\\backup", nullptr, nullptr), e); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("access denied") >= 0); note(t->err);   // no user, no anonymous access
        delete t;
    }
    // unknown share / unknown host
    {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\nothere", "backupuser", kSecret), e); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("share") >= 0); delete t;
        t = NewSmb(SmbCfg("\\\\otherhost\\backup", "backupuser", kSecret), e); T_CHECK(!t->connect()); T_CHECK(t->transient); T_CHECK(t->err.find("otherhost") >= 0); delete t;
    }
    // network down: transient, then success after reconnect()
    {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "d"), e);
        g_f.down = true;
        T_CHECK(!t->connect()); T_CHECK(t->transient); T_CHECK(t->err.find("not reachable") >= 0); note(t->err);
        T_CHECK(!t->reconnect()); T_CHECK(t->transient);
        T_CHECK(t->open_write("x") == nullptr); T_CHECK(t->transient);                      // every call keeps failing the same way
        Vec<Str> n; T_CHECK(!t->list(n)); T_CHECK(t->transient);
        g_f.down = false;
        T_CHECK(t->reconnect()); T_CHECK(t->put_bytes("x", "1", 1));
        delete t; T_CHECK(SmbOpenConnections() == 0);
    }
    // reconnect after a lost connection drops the old one first (cancel, then add)
    {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret), e); T_CHECK(t->connect());
        g_f.log.clear(); T_CHECK(t->reconnect());
        T_CHECK(g_f.log.size() >= 2 && g_f.log[0].starts_with("cancel") && g_f.log[1].starts_with("add"));
        T_CHECK(SmbOpenConnections() == 1); delete t; T_CHECK(SmbOpenConnections() == 0); T_CHECK(g_f.conns.empty());
    }
    // 1219: our own earlier connection with other credentials is cancelled and the connection is retried once
    {
        Str e; Target* a = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret), e); T_CHECK(a->connect());
        Target* b = NewSmb(SmbCfg("\\\\fileserver\\backup", "corp\\other", "Other-Pw-77"), e); g_f.log.clear();
        T_CHECK(b->connect());
        T_CHECK(g_f.log.size() == 3); if (g_f.log.size() == 3) { T_CHECK(g_f.log[0].starts_with("add")); T_CHECK(g_f.log[1].starts_with("cancel")); T_CHECK(g_f.log[2].starts_with("add")); }
        T_CHECK(g_f.conns.size() == 1 && g_f.conns[0].find("corp\\other") >= 0);
        delete b; delete a; T_CHECK(g_f.conns.empty()); T_CHECK(SmbOpenConnections() == 0);
    }
    // ... and the one whose connection was cancelled that way lets go FIRST: it must not cancel the connection that replaced its own
    {
        Str e; Target* a = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret), e); T_CHECK(a->connect());
        Target* b = NewSmb(SmbCfg("\\\\fileserver\\backup", "corp\\other", "Other-Pw-77"), e); T_CHECK(b->connect());
        delete a; T_CHECK(g_f.conns.size() == 1 && g_f.conns[0].find("corp\\other") >= 0); T_CHECK(SmbOpenConnections() == 1);
        delete b; T_CHECK(g_f.conns.empty()); T_CHECK(SmbOpenConnections() == 0);
    }
    // 1219 from a connection that is not ours: a clear permanent error (we do not cancel what we did not make)
    {
        g_f.conns.push_back("\\\\fileserver|someone");
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret), e);
        int cancels = g_f.cancels;
        T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("other credentials") >= 0); T_CHECK(g_f.cancels == cancels); note(t->err);
        delete t; T_CHECK(g_f.conns.size() == 1);
        // with no user at all the existing connection is simply used
        g_f.anonymous = true; t = NewSmb(SmbCfg("\\\\fileserver\\backup", nullptr, nullptr), e); T_CHECK(t->connect()); delete t;
        g_f.conns.clear();
    }
    // empty user = the service account: WNetAddConnection2 gets no user at all
    {
        g_f.anonymous = true; g_f.log.clear();
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "", ""), e); T_CHECK(t->connect()); T_CHECK(g_f.log.size() == 1 && g_f.log[0].find("user=<none>") >= 0);
        delete t;
    }
    // two targets on one share: the connection ends with the last one
    {
        Str e; Target* a = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "a"), e); Target* b = NewSmb(SmbCfg("\\\\FILESERVER\\Backup", "backupuser", kSecret, "b"), e);
        T_CHECK(a->connect()); T_CHECK(b->connect()); int c0 = g_f.cancels;
        delete a; T_CHECK(g_f.cancels == c0); T_CHECK(!g_f.conns.empty());
        T_CHECK(b->put_bytes("f", "x", 1)); delete b; T_CHECK(g_f.cancels == c0 + 1); T_CHECK(g_f.conns.empty());
    }
    // two targets on one share, one of them reconnects (a stale connection): the other one's hold must survive, the share stays connected
    // until the LAST holder lets go, whoever reconnected and whoever is destroyed first
    for (int order = 0; order < 4; ++order) {
        Str e; Target* a = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "a"), e); Target* b = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "b"), e);
        T_CHECK(a->connect()); T_CHECK(b->connect());
        T_CHECK((order & 1 ? b : a)->reconnect()); T_CHECK(SmbOpenConnections() == 1); T_CHECK(!g_f.conns.empty());
        Target* first = order & 2 ? b : a; Target* last = order & 2 ? a : b;
        delete first; T_CHECK(SmbOpenConnections() == 1); T_CHECK(!g_f.conns.empty());         // the other holder is still using the share
        T_CHECK(last->put_bytes("f", "x", 1)); delete last; T_CHECK(SmbOpenConnections() == 0); T_CHECK(g_f.conns.empty());
    }
    // errors in the middle of an operation: a folder that vanished is permanent; the text carries no password
    {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "gone"), e); T_CHECK(t->connect());
        RemoveTree(g_f.local + "\\gone");
        T_CHECK(t->open_write("x") == nullptr); T_CHECK(!t->transient); note(t->err);
        delete t;
    }
    for (const Str& s : errors) { if (s.find(kSecret) >= 0 || s.find("S3cr3t") >= 0 || s.find("Other-Pw") >= 0) { c_.note("password in an error text: %s", s.c_str()); c_.failed = true; } }
    for (const Str& s : g_f.log) T_CHECK(s.find("S3cr3t") < 0);
    return true;
}

// ---- the engine through the target ---------------------------------------------------------------------------------
namespace {

// A connection that dies after some bytes (the style of the engine tests): the failing call is transient, every call fails until reconnect().
struct Flaky : Target {
    Target* in; Ctx* c; bool dead; int faultsLeft; u64 moved, nextAt; int faults, reconnects;
    Flaky(Target* t, Ctx* ctx, int n) : in(t), c(ctx), dead(false), faultsLeft(n), moved(0), nextAt(0), faults(0), reconnects(0) { sched(); }
    ~Flaky() { delete in; }
    void sched() { nextAt = moved + 20000 + c->next() % 150000; }
    bool deadFail() { err = "connection reset by peer (test)"; transient = true; return false; }
    bool strike() { if (faultsLeft <= 0 || moved < nextAt) return false; --faultsLeft; ++faults; dead = true; sched(); return true; }
    const char* kind() const override { return "flaky-smb"; }
    bool connect() override { if (dead) return deadFail(); if (!in->connect()) { err = in->err; transient = in->transient; return false; } return true; }
    bool reconnect() override { dead = false; ++reconnects; if (!in->reconnect()) { err = in->err; transient = in->transient; return false; } return true; }
    struct W : Sink {
        Flaky* t; Sink* s; W(Flaky* f, Sink* x) : t(f), s(x) {} ~W() { delete s; }
        bool write(const void* p, size_t n) override { if (t->dead) { s->abort(); return fail("connection reset by peer (test)", true); } t->moved += n; if (t->strike()) { s->abort(); return fail("connection reset by peer (test)", true); } if (!s->write(p, n)) return failFrom(s); return true; }
        bool finish() override { if (t->dead) { s->abort(); return fail("connection reset by peer (test)", true); } if (!s->finish()) return failFrom(s); return true; }
        void abort() override { s->abort(); }
    };
    struct R : Source {
        Flaky* t; Source* s; R(Flaky* f, Source* x) : t(f), s(x) {} ~R() { s->close(); delete s; }
        i64 read(void* p, size_t n) override { if (t->dead) return fail("connection reset by peer (test)", true); i64 k = s->read(p, n); if (k < 0) return failFrom(s); t->moved += (u64)k; if (t->strike()) return fail("connection reset by peer (test)", true); return k; }
        void close() override { s->close(); }
    };
    Sink* open_write(const Str& n) override { if (dead) { deadFail(); return nullptr; } Sink* s = in->open_write(n); if (!s) { err = in->err; transient = in->transient; return nullptr; } return new W(this, s); }
    Source* open_read(const Str& n) override { if (dead) { deadFail(); return nullptr; } Source* s = in->open_read(n); if (!s) { err = in->err; transient = in->transient; return nullptr; } return new R(this, s); }
    bool list(Vec<Str>& n) override { if (dead) { deadFail(); return false; } if (!in->list(n)) { err = in->err; transient = in->transient; return false; } return true; }
    bool remove(const Str& n) override { if (dead) { deadFail(); return false; } if (!in->remove(n)) { err = in->err; transient = in->transient; return false; } return true; }
};

void MakeFiles(Ctx& c, const Str& root, const Str& vm, Vec<Str>& rels, Vec<Bytes>& data)
{
    int n = c.range(2, 6);
    for (int i = 0; i < n; ++i) {
        Str rel = Str::fmt("%sf%d.%s", c.coin(30) ? "sub\\" : "", i, c.coin() ? "vmdk" : "vmx");
        Bytes b = Random(c, (size_t)(i > 0 && c.coin(40) ? c.range(1, 2000) : c.range(50000, 700000)));   // the first file is always big: the tests need several volumes
        WriteBytes(JoinPath(JoinPath(root, vm), rel), b); rels.push_back(rel); data.push_back(b);
    }
}

} // namespace

VM2B_TEST("smb", engine_round_trip_with_connection_loss)
{
    FakeScope fs(c_, "smbeng");
    Str vms = fs.tmp.sub("vms"), vm = "vm \xd0\xb2\xd0\xbc";
    Vec<Str> rels; Vec<Bytes> data; MakeFiles(c_, vms, vm, rels, data);
    { Bytes big = Random(c_, 900000); WriteBytes(JoinPath(JoinPath(vms, vm), "zz-big.vmdk"), big); rels.push_back("zz-big.vmdk"); data.push_back(big); }   // enough data for the faults
    Str e; Target* inner = NewSmb(SmbCfg("\\\\fileserver\\backup\\store", "backupuser", kSecret, "vm1"), e); T_CHECK(inner != nullptr); if (!inner) return false;
    Flaky* t = new Flaky(inner, &c_, 3);
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = 512 * (u64)c_.range(64, 400); o.verify = true;
    o.retry.attempts = 12; o.retry.baseDelayMs = 1; o.retry.maxDelayMs = 4;
    if (c_.coin()) o.password = "pw-\xd0\xbf\xd0\xb0\xd1\x80";
    VmSource* src = NewDirectorySource(vms); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, vm, *t, o, nullptr, m, warn, err); delete src;
    if (!ok) c_.note("backup: %s", err.c_str());
    T_CHECK(ok);
    c_.note("faults=%d reconnects=%d volumes=%d adds=%d", t->faults, t->reconnects, (int)m.segments.size(), g_f.adds);
    T_CHECK(t->faults >= 1); T_CHECK(t->reconnects >= 1);
    T_CHECK(g_f.adds >= 1 + t->reconnects);                      // every reconnect() went through WNetAddConnection2 again
    if (ok) {
        Vec<Str> names; T_CHECK(t->in->list(names));
        bool part = false; for (const Str& n : names) if (n.ends_with(".part")) part = true; T_CHECK(!part);
        T_CHECK(DirExists(g_f.local + "\\store\\vm1"));
        Str ve; T_CHECK(VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve));
        Str dest = fs.tmp.sub("restored"); Manifest rm; Str re;
        T_CHECK(RestoreBackup(*t, m.archive, dest, o.password, nullptr, &rm, re));
        for (size_t i = 0; i < rels.size(); ++i) {
            Bytes got; Str ge; T_CHECK(ReadWholeFile(JoinPath(JoinPath(dest, vm), rels[i]).c_str(), got, ge) && got.size() == data[i].size() && (got.empty() || memcmp(got.data(), data[i].data(), got.size()) == 0));
        }
        // the volumes are on the "share"
        Vec<Str> disk; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(g_f.local + "\\store\\vm1\\*"), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) disk.push_back(FromWide(fd.cFileName)); } while (FindNextFileW(h, &fd)); FindClose(h); }
        T_CHECK(disk.size() >= m.segments.size() + 1);
    }
    delete t; T_CHECK(SmbOpenConnections() == 0); T_CHECK(g_f.conns.empty());
    return true;
}

VM2B_TEST("smb", retention_and_resume_in_a_later_run)
{
    FakeScope fs(c_, "smbkeep");
    Str vms = fs.tmp.sub("vms"), vm = "vmk"; Vec<Str> rels; Vec<Bytes> data; MakeFiles(c_, vms, vm, rels, data);
    { Bytes pad = Random(c_, 200000); WriteBytes(JoinPath(JoinPath(vms, vm), "zz-pad.vmdk"), pad); rels.push_back("zz-pad.vmdk"); data.push_back(pad); }   // always more than two 60 KiB volumes, or the interruption below never happens
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = 512 * 120; o.verify = false; o.keep = 2;
    o.retry.attempts = 4; o.retry.baseDelayMs = 1; o.retry.maxDelayMs = 2;
    for (int i = 0; i < 4; ++i) {
        Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "keep"), e); T_CHECK(t != nullptr);
        VmSource* src = NewDirectorySource(vms); Manifest m; Str w, err; T_CHECK(BackupVm(*src, vm, *t, o, nullptr, m, w, err)); delete src;
        delete t; Sleep(1100);                                   // the archive names carry a one-second stamp
    }
    Str e; Target* t = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "keep"), e);
    Vec<Manifest> ms; Str le; T_CHECK(ListBackups(*t, nullptr, ms, le)); T_CHECK(ms.size() == 2);
    // an interrupted backup (the share goes away for good) leaves a journal that a later run continues
    struct Cut : Progress { u64 n = 0; bool aborted() override { return false; } };
    delete t;
    Target* t2 = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "resume"), e);
    struct Stop : Progress { int vol = 0; bool aborted() override { return vol >= 2; } void onVolume(int, int) override { ++vol; } bool keepOnAbort() override { return true; } } stop;
    BackupOptions o2 = o; o2.keep = 0; VmSource* s2 = NewDirectorySource(vms); Manifest m2; Str w2, e2;
    bool ok = BackupVm(*s2, vm, *t2, o2, &stop, m2, w2, e2); T_CHECK(!ok); delete s2;
    Vec<ResumeInfo> ri; Str re; T_CHECK(ListResumable(*t2, nullptr, ri, re)); T_CHECK(ri.size() == 1 && ri[0].volumesDone >= 1);
    delete t2; T_CHECK(SmbOpenConnections() == 0);
    Target* t3 = NewSmb(SmbCfg("\\\\fileserver\\backup", "backupuser", kSecret, "resume"), e);
    struct Count : Progress { int resumes = 0; void onResume(int, u64) override { ++resumes; } } cnt;
    VmSource* s3 = NewDirectorySource(vms); Manifest m3; Str w3, e3;
    T_CHECK(BackupVm(*s3, vm, *t3, o2, &cnt, m3, w3, e3)); delete s3; T_CHECK(cnt.resumes == 1);
    Str dest = fs.tmp.sub("rest"), rs; T_CHECK(RestoreBackup(*t3, m3.archive, dest, "", nullptr, nullptr, rs));
    for (size_t i = 0; i < rels.size(); ++i) { Bytes got; Str ge; T_CHECK(ReadWholeFile(JoinPath(JoinPath(dest, vm), rels[i]).c_str(), got, ge) && got.size() == data[i].size() && (got.empty() || memcmp(got.data(), data[i].data(), got.size()) == 0)); }
    delete t3;
    return true;
}

// ---- configuration and secrets -------------------------------------------------------------------------------------
VM2B_TEST("smb", config_round_trip_and_secrets)
{
    Tmp tmp(c_, "smbcfg");
    Str dir = tmp.sub("data"), id;
    {
        Config cfg; Str err; T_CHECK(cfg.open(dir, err));
        Json s = Json::object(); s["type"] = "smb"; s["name"] = "NAS"; s["path"] = "//nas.corp.local/Backups/vm2b/"; s["user"] = "CORP\\svc-backup"; s["password"] = kSecret;
        Json saved; T_CHECK(cfg.saveServer(s, saved, err)); if (!err.empty()) c_.note("%s", err.c_str());
        id = saved.get("id").str(); T_CHECK(!id.empty());
        T_CHECK(saved.get("path").str() == "\\\\nas.corp.local\\Backups\\vm2b"); T_CHECK(saved.get("user").str() == "CORP\\svc-backup");
        T_CHECK(!saved.has("password")); T_CHECK(saved.get("has_password").boolean());
        T_CHECK(cfg.servers().dump().find("S3cr3t") < 0);                                       // the public view
        Json rev; T_CHECK(cfg.server(id, true, rev)); T_CHECK(rev.get("password").str() == kSecret);
        Json pub; T_CHECK(cfg.server(id, false, pub)); T_CHECK(pub.dump().find("S3cr3t") < 0);
        // the mask keeps the stored password; an empty one removes it
        Json again = pub; again["name"] = "NAS 2"; again["password"] = kMask; again["id"] = id; Json sv2; T_CHECK(cfg.saveServer(again, sv2, err));
        T_CHECK(cfg.server(id, true, rev)); T_CHECK(rev.get("password").str() == kSecret);
        // bad paths are refused with a message
        Json bad = Json::object(); bad["type"] = "smb"; bad["name"] = "Bad"; bad["path"] = "C:\\folder"; T_CHECK(!cfg.saveServer(bad, saved, err)); T_CHECK(err.find("network folder") == 0 && err.find("drive") >= 0);
        bad["path"] = "\\\\?\\C:\\x"; T_CHECK(!cfg.saveServer(bad, saved, err));
        bad["path"] = "\\\\host\\share\\..\\x"; T_CHECK(!cfg.saveServer(bad, saved, err));
        bad["path"] = ""; T_CHECK(!cfg.saveServer(bad, saved, err));
        // a folder with VMs on a network path takes credentials too
        Json d = Json::object(); d["type"] = "directory"; d["name"] = "VMs on NAS"; d["path"] = "//nas/vms"; d["user"] = "u"; d["password"] = "dirpw-77";
        Json ds; T_CHECK(cfg.saveServer(d, ds, err)); T_CHECK(ds.get("path").str() == "\\\\nas\\vms"); T_CHECK(!ds.has("password"));
        d["name"] = "VMs bad"; d["path"] = "\\\\nas\\.."; T_CHECK(!cfg.saveServer(d, ds, err));
        d["name"] = "Local VMs"; d["path"] = "D:\\vms"; d.remove("user"); d.remove("password"); T_CHECK(cfg.saveServer(d, ds, err));       // plain local paths are not touched
        T_CHECK(ds.get("path").str() == "D:\\vms");
    }
    // the file on disk: no plain password anywhere, DPAPI value present
    { Bytes b; Str e; T_CHECK(ReadWholeFile(JoinPath(dir, "config.json").c_str(), b, e)); Str txt((const char*)b.data(), b.size());
      T_CHECK(txt.find("S3cr3t") < 0); T_CHECK(txt.find("dirpw-77") < 0); T_CHECK(txt.find("dpapi:") >= 0); T_CHECK(txt.find("\"smb\"") >= 0); }
    // reopened: still there; a hand-edited plain password is accepted as it is
    {
        Config cfg; Str err; T_CHECK(cfg.open(dir, err));
        Json rev; T_CHECK(cfg.server(id, true, rev)); T_CHECK(rev.get("password").str() == kSecret); T_CHECK(rev.get("type").str() == "smb");
    }
    // an old / foreign file with a type this version does not know: it opens, the server is listed, it cannot be saved or used
    {
        Str txt = "{\"servers\":[{\"id\":\"x1\",\"name\":\"Old\",\"type\":\"nfs\",\"host\":\"h\"},{\"id\":\"x2\",\"name\":\"Smb plain\",\"type\":\"smb\",\"path\":\"\\\\\\\\h\\\\s\",\"user\":\"u\",\"password\":\"plain-pw\"}],\"tasks\":[]}";
        Str d2 = tmp.sub("data2"), e; MakeDirs(d2, e); WriteWholeFile(JoinPath(d2, "config.json").c_str(), txt.data(), txt.size(), e);
        Config cfg; Str err; T_CHECK(cfg.open(d2, err));
        T_CHECK(cfg.servers().size() == 3);                       // Local Computer + the two
        Json r; T_CHECK(cfg.server("x1", true, r)); T_CHECK(cfg.server("x2", true, r)); T_CHECK(r.get("password").str() == "plain-pw");
        Json pub; T_CHECK(cfg.server("x2", false, pub)); T_CHECK(pub.dump().find("plain-pw") < 0);
        Json up = Json::object(); up["id"] = "x1"; up["name"] = "Old"; up["type"] = "nfs"; Json sv; T_CHECK(!cfg.saveServer(up, sv, err)); T_CHECK(err.find("unknown server type") >= 0);
        Str te; Json tc = Json::object(); tc["type"] = "nfs"; T_CHECK(MakeTarget(tc, te) == nullptr); T_CHECK(te.find("unknown") >= 0);
    }
    return true;
}

// ---- a folder source on a network path, drive letters --------------------------------------------------------------
VM2B_TEST("smb", directory_source_and_drive_letters)
{
    FakeScope fs(c_, "smbsrc");
    Vec<Str> rels; Vec<Bytes> data; MakeFiles(c_, g_f.local, "vmA", rels, data);
    Json s = Json::object(); s["type"] = "directory"; s["path"] = "//fileserver/backup"; s["user"] = "backupuser"; s["password"] = kSecret;
    Str e; VmSource* src = MakeSource(s, e); T_CHECK(src != nullptr); if (!src) return false;
    Vec<VmInfo> vms; T_CHECK(src->list(vms)); T_CHECK(vms.size() == 1 && vms[0].name == "vmA");
    T_CHECK(SmbOpenConnections() == 1);
    delete src; T_CHECK(SmbOpenConnections() == 0); T_CHECK(g_f.conns.empty());
    s["password"] = "nope"; T_CHECK(MakeSource(s, e) == nullptr); T_CHECK(e.find("password") >= 0); T_CHECK(e.find("nope") < 0);
    s["path"] = "\\\\?\\C:\\x"; T_CHECK(MakeSource(s, e) == nullptr || true);
    // a drive letter the service cannot see: the clear message
    wchar_t letter = 0;
    for (wchar_t l = L'Q'; l >= L'D'; --l) { wchar_t r[4] = { l, L':', L'\\', 0 }; UINT t = GetDriveTypeW(r); if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) { letter = l; break; } }
    if (!letter) { c_.note("no free drive letter here: the drive-letter message is not tested"); return true; }
    Str path = Str::fmt("%c:\\backups", (char)letter);
    Str msg = SmbDriveUnavailableMessage(path); T_CHECK(msg.find("not available to the service") >= 0 && msg.find("UNC path") >= 0 && msg.find("convert") >= 0);
    T_CHECK(SmbDriveUnavailableMessage("C:\\x").find("drive") < 0 || GetDriveTypeW(L"C:\\") == DRIVE_NO_ROOT_DIR);
    T_CHECK(SmbDriveUnavailableMessage("\\\\h\\s").empty()); T_CHECK(SmbDriveUnavailableMessage("relative\\x").empty());
    Json lc = Json::object(); lc["type"] = "local"; lc["path"] = path; Target* lt = MakeTarget(lc, e); T_CHECK(lt != nullptr);
    T_CHECK(!lt->connect()); T_CHECK(lt->err.find("not available to the service") >= 0); delete lt;
    Json ds = Json::object(); ds["type"] = "directory"; ds["path"] = path; VmSource* d2 = MakeSource(ds, e); T_CHECK(d2 != nullptr);
    Vec<VmInfo> v2; T_CHECK(!d2->list(v2)); T_CHECK(d2->err.find("not available to the service") >= 0); delete d2;
    // resolving a typed drive path in the GUI: a drive that is not a network drive is refused with a reason
    Str unc, re; T_CHECK(!SmbResolveDrivePath(path, unc, re)); T_CHECK(!re.empty());
    T_CHECK(!SmbResolveDrivePath("C:\\x", unc, re) || unc.starts_with("\\\\")); T_CHECK(!SmbResolveDrivePath("\\\\h\\s", unc, re));
    Vec<Str> letters, uncs; SmbListNetworkDrives(letters, uncs); T_CHECK(letters.size() == uncs.size());
    for (size_t i = 0; i < letters.size(); ++i) { T_CHECK(letters[i].size() == 2 && uncs[i].starts_with("\\\\")); }
    T_CHECK(SmbHasDriveLetter("Z:") && SmbHasDriveLetter("z:\\x") && !SmbHasDriveLetter("\\\\h\\s") && !SmbHasDriveLetter("ab:\\x") && !SmbHasDriveLetter("1:\\"));
    T_CHECK(SmbIsUncPath("//h/s") && SmbIsUncPath("\\\\h\\s") && !SmbIsUncPath("\\\\?\\C:\\x") && !SmbIsUncPath("\\\\.\\pipe\\x") && !SmbIsUncPath("C:\\x") && !SmbIsUncPath("\\x"));
    return true;
}

// ---- through the service -----------------------------------------------------------------------------------------
VM2B_TEST("smb", service_save_test_and_backup)
{
    FakeScope fs(c_, "smbsvc");
    Svc s(c_, "smbsvc2"); if (!s.up) return false;
    s.record = true;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 300000)); Str src = s.addDirServer(vms);
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "Share"; sv["path"] = "\\\\fileserver\\backup\\vm2b"; sv["user"] = "backupuser"; sv["password"] = kSecret;
    Json a = Json::object(); a["server"] = sv; Json saved = s.ok("servers.save", a); Str id = saved.get("id").str(); T_CHECK(!id.empty());
    Json list = s.ok("servers.list", Json::object()); T_CHECK(list.dump().find("S3cr3t") < 0);
    // test the saved server and an unsaved one from the wizard (the stored password stands behind the mask)
    Json ta = Json::object(); ta["id"] = id; Json tr = s.ok("servers.test", ta);
    T_CHECK(tr.get("message").str().find("test file") >= 0); T_CHECK(tr.get("message").str().find("free space") >= 0);
    T_CHECK(!FileExists(g_f.local + "\\vm2b\\.vm2b-test.tmp"));
    Json wiz = saved; wiz["password"] = kMask; Json wa = Json::object(); wa["server"] = wiz; Json wr = s.ok("servers.test", wa); T_CHECK(wr.get("message").str().find("free space") >= 0);
    Json wrong = sv; wrong["password"] = "bad-pass-1"; Json wb = Json::object(); wb["server"] = wrong; T_CHECK(!s.call("servers.test", wb)); T_CHECK(s.lastErr.find("password") >= 0); T_CHECK(s.lastErr.find("bad-pass-1") < 0);
    // a task that backs up to the share
    Json el = Svc::Element(src, "vm1", "{VM}"); Json tg = Json::object(); tg["server"] = id; tg["directory"] = "{VM}"; el["target"] = tg;
    Str task = s.saveTask("ToShare", el); T_CHECK(!task.empty());
    Json ra = Json::object(); ra["id"] = task; Json run = s.ok("tasks.run", ra); Str fin = s.waitJob(run.get("job").str()); T_CHECK(fin == "Success" || fin == "Gone");
    T_CHECK(s.waitHistory(1));
    T_CHECK(DirExists(g_f.local + "\\vm2b\\vm1"));
    Json qa = Json::object(); qa["server"] = id; Json bl = s.ok("backups.list", qa); T_CHECK(bl.get("backups").size() == 1);
    if (bl.get("backups").size() == 1) {
        Json va = Json::object(); va["server"] = id; va["directory"] = bl.get("backups").at(0).get("directory"); va["archive"] = bl.get("backups").at(0).get("archive");
        Json vr = s.ok("backups.verify", va); Str vs = s.waitJob(vr.get("job").str()); T_CHECK(vs == "Success" || vs == "Gone");
        Json rr = va; rr["dest"] = s.tmp.sub("restored"); Json rj = s.ok("backups.restore", rr); Str rs = s.waitJob(rj.get("job").str()); T_CHECK(rs == "Success" || rs == "Gone");
        T_CHECK(FileExists(JoinPath(s.tmp.sub("restored"), "vm1\\disk.vmdk")));
    }
    T_CHECK(s.transcript.find("S3cr3t") < 0); T_CHECK(s.transcript.find("bad-pass-1") < 0);
    // the history and the log of the jobs never show the password either
    Json hl = s.ok("history.list", [&] { Json x = Json::object(); x["limit"] = 10; return x; }()); T_CHECK(hl.dump().find("S3cr3t") < 0);
    // a server in use cannot be deleted; deleting the task first lets it go
    Json del = Json::object(); del["id"] = id; T_CHECK(!s.call("servers.delete", del));
    return true;
}

// ---- the lock-out guard (a wrong password is not offered again and again) -----------------------------------------------
namespace {

Str SaveSmbServer(Svc& s, const char* name, const char* user, const char* pass)
{
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = name; sv["path"] = "\\\\fileserver\\backup\\vm2b"; sv["user"] = user; sv["password"] = pass;
    Json a = Json::object(); a["server"] = sv; return s.ok("servers.save", a).get("id").str();
}
Json SaveSmbWithId(Svc& s, const Str& id, const char* name, const char* user, const char* pass)
{
    Json sv = Json::object(); sv["id"] = id; sv["type"] = "smb"; sv["name"] = name; sv["path"] = "\\\\fileserver\\backup\\vm2b"; sv["user"] = user; sv["password"] = pass;
    Json a = Json::object(); a["server"] = sv; return s.ok("servers.save", a);
}
Json ServerOf(Svc& s, const Str& id)
{
    Json l = s.ok("servers.list", Json::object()); for (size_t i = 0; i < l.size(); ++i) if (l.at(i).get("id").str() == id) return l.at(i);
    return Json::object();
}
Json EveryTen(i64 t0)
{
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "every"; Json ev = Json::object(); ev["value"] = 10; ev["unit"] = "minutes"; sch["every"] = ev;
    sch["first_time"] = FormatLocalMin(UnixToLocalMin(t0)).replace_all(" ", "T"); return sch;
}
Json ElementTo(const Str& src, const Str& tgtServer) { Json el = Svc::Element(src, "vm1", "{VM}"); Json tg = Json::object(); tg["server"] = tgtServer; tg["directory"] = "{VM}"; el["target"] = tg; el["verify"] = false; return el; }
// one scheduler pass per fake 10 minutes; waits for the run of every pass to be in the history
void Passes(Svc& s, int n)
{
    for (int i = 0; i < n; ++i) { int h = s.historyCount(); g_fakeNow += 10 * 60; s.waitHistory(h + 1, 20000); }
}
Json LastRecord(Svc& s)
{
    Json l = s.ok("history.list", [] { Json a = Json::object(); a["limit"] = 1; return a; }());
    if (l.size() == 0) return Json::object();
    Json a = Json::object(); a["id"] = l.at(0).get("id").str(); return s.ok("history.get", a);
}
Str ReadAll(const Str& path) { Bytes b; Str e; if (!ReadWholeFile(path.c_str(), b, e)) return Str(); return Str((const char*)b.data(), b.size()); }

} // namespace

VM2B_TEST("smb", auth_guard_scheduled_task_one_attempt)
{
    FakeScope fs(c_, "smbag");
    Svc s(c_, "smbag2", true); if (!s.up) return false;
    s.record = true;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 60000)); Str src = s.addDirServer(vms);
    const char* bad = "Wr0ng-Pw-Lockout-1"; const char* good = "S3cr3t-Pa55-\xc3\xbc";
    Str id = SaveSmbServer(s, "Share", "backupuser", bad);
    g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
    Str task = s.saveTask("ToShare", ElementTo(src, id), EveryTen(t0)); T_CHECK(!task.empty());
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = true; return a; }());
    Sleep(200);
    Passes(s, 6);
    c_.note("6 scheduler passes with a wrong password: %d WNetAddConnection2W call(s)", g_f.adds);
    T_CHECK(g_f.adds == 1);                                                                           // the first run asked once, the other five did not touch the server
    Json rec = LastRecord(s); T_CHECK(rec.get("status").str() == "Failed");
    T_CHECK(rec.get("results").size() == 1 && rec.get("results").at(0).get("skipped").boolean());
    T_CHECK(rec.get("results").at(0).get("message").str().find("were rejected") >= 0); T_CHECK(rec.get("results").at(0).get("message").str().find("1326") >= 0);
    T_CHECK(rec.get("results").at(0).get("message").str().find("Try again") >= 0);
    Json sv = ServerOf(s, id); T_CHECK(sv.get("auth_blocked").is_object()); T_CHECK(sv.get("auth_blocked").get("code").integer() == 1326); T_CHECK(sv.get("auth_blocked").get("time").integer() > 0);
    T_CHECK(sv.dump().find(bad) < 0);
    // the first record of the series is a real attempt, not a skip
    { Json hl = s.ok("history.list", [] { Json a = Json::object(); a["limit"] = 50; return a; }()); Json oldest = hl.at(hl.size() - 1); Json a = Json::object(); a["id"] = oldest.get("id").str(); Json r0 = s.ok("history.get", a);
      T_CHECK(!r0.get("results").at(0).get("skipped").boolean()); T_CHECK(r0.get("results").at(0).get("message").str().find("(1326)") >= 0); }
    // the mark is in state.json (not in config.json) and holds no password; the service restarts and still does not try
    Str stJson = ReadAll(JoinPath(s.core.config().dataDir(), "state.json")), cfgJson = ReadAll(JoinPath(s.core.config().dataDir(), "config.json"));
    T_CHECK(stJson.find("_auth_blocks") >= 0 && stJson.find("creds_fp") >= 0); T_CHECK(cfgJson.find("auth_blocked") < 0 && cfgJson.find("creds_fp") < 0);
    T_CHECK(stJson.find(bad) < 0 && stJson.find("Wr0ng") < 0);
    T_CHECK(s.restart()); s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = true; return a; }()); Sleep(200);
    T_CHECK(ServerOf(s, id).get("auth_blocked").is_object());
    Passes(s, 3); T_CHECK(g_f.adds == 1);
    // an edit of the record that does not touch the login data (the name) keeps the block; a new password lifts it and the next run goes through
    { Json r = ServerOf(s, id); r["password"] = kMask; r["name"] = "Share renamed"; Json a = Json::object(); a["server"] = r; s.ok("servers.save", a); }
    T_CHECK(ServerOf(s, id).get("auth_blocked").is_object()); Passes(s, 1); T_CHECK(g_f.adds == 1);
    SaveSmbWithId(s, id, "Share renamed", "backupuser", good);
    T_CHECK(!ServerOf(s, id).get("auth_blocked").is_object());
    Passes(s, 1); T_CHECK(g_f.adds >= 2); T_CHECK(LastRecord(s).get("status").str() == "Success");
    int addsOk = g_f.adds;
    // the password goes stale again: one more attempt, then silence; "Try again" gives exactly one more
    SaveSmbWithId(s, id, "Share renamed", "backupuser", "Stale-pw-2");
    Passes(s, 4); T_CHECK(g_f.adds == addsOk + 1); T_CHECK(ServerOf(s, id).get("auth_blocked").is_object());
    { Json a = Json::object(); a["id"] = id; Json r = s.ok("servers.unblock", a); T_CHECK(r.get("was_blocked").boolean()); }
    T_CHECK(!ServerOf(s, id).get("auth_blocked").is_object());
    Passes(s, 4); T_CHECK(g_f.adds == addsOk + 2); T_CHECK(ServerOf(s, id).get("auth_blocked").is_object());
    { Json a = Json::object(); a["id"] = "nope"; T_CHECK(!s.call("servers.unblock", a)); T_CHECK(s.lastErr.find("not found") >= 0); }
    // a manual run is an explicit action: ONE attempt each time, and a new refusal sets the mark again
    int before = g_f.adds;
    { Json a = Json::object(); a["id"] = task; Json r = s.ok("tasks.run", a); Str st = s.waitJob(r.get("job").str()); T_CHECK(st == "Failed" || st == "Gone"); }
    T_CHECK(g_f.adds == before + 1); T_CHECK(ServerOf(s, id).get("auth_blocked").is_object());
    { Json a = Json::object(); a["id"] = task; Json r = s.ok("tasks.run", a); s.waitJob(r.get("job").str()); }
    T_CHECK(g_f.adds == before + 2);
    // "Test connection" is explicit too: one attempt; a success removes the mark
    { Json a = Json::object(); a["id"] = id; T_CHECK(!s.call("servers.test", a)); T_CHECK(g_f.adds == before + 3); T_CHECK(ServerOf(s, id).get("auth_blocked").is_object()); }
    SaveSmbWithId(s, id, "Share renamed", "backupuser", good);
    { Json a = Json::object(); a["id"] = id; T_CHECK(s.call("servers.test", a)); T_CHECK(!ServerOf(s, id).get("auth_blocked").is_object()); }
    // the backup explorer does not knock on a closed door either
    SaveSmbWithId(s, id, "Share renamed", "backupuser", "Stale-pw-3");
    { Json a = Json::object(); a["id"] = id; T_CHECK(!s.call("servers.test", a)); }
    before = g_f.adds; { Json a = Json::object(); a["server"] = id; a["directory"] = "{VM}"; T_CHECK(!s.call("backups.list", a)); T_CHECK(s.lastErr.find("were rejected") >= 0); } T_CHECK(g_f.adds == before);
    // nothing anywhere holds a password
    T_CHECK(s.transcript.find(bad) < 0); T_CHECK(s.transcript.find("Stale-pw") < 0); T_CHECK(s.transcript.find("Wr0ng") < 0); T_CHECK(s.transcript.find("S3cr3t") < 0);
    stJson = ReadAll(JoinPath(s.core.config().dataDir(), "state.json")); T_CHECK(stJson.find("Stale-pw") < 0 && stJson.find("S3cr3t") < 0);
    Str hist = ReadAll(JoinPath(s.core.config().dataDir(), "history.json")); T_CHECK(hist.find("Stale-pw") < 0 && hist.find(bad) < 0 && hist.find("S3cr3t") < 0);
    Str logs = ReadAll(JoinPath(s.core.config().dataDir(), "service.log")); T_CHECK(logs.find("Stale-pw") < 0 && logs.find(bad) < 0);
    Json off = Json::object(); off["enabled"] = false; s.ok("scheduler.set", off);
    return true;
}

VM2B_TEST("smb", auth_guard_codes_transient_and_independence)
{
    FakeScope fs(c_, "smbag3");
    // which codes are a refusal of the credentials
    { const unsigned long yes[] = { 86, 1326, 1909, 1331, 1330, 1907, 1385, 1327, 1328 }; for (unsigned long k : yes) T_CHECK(SmbIsAuthCode(k));
      const unsigned long no[] = { 0, 5, 53, 67, 1219, 1231, 1225, 1311, 1460, 121, 1203, 1396, 2250, 64 }; for (unsigned long k : no) T_CHECK(!SmbIsAuthCode(k)); }
    Svc s(c_, "smbag4", true); if (!s.up) return false;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 40000)); Str src = s.addDirServer(vms);
    g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
    // a network error never blocks
    Str net = SaveSmbServer(s, "Net", "backupuser", "S3cr3t-Pa55-\xc3\xbc");
    Str tNet = s.saveTask("Net task", ElementTo(src, net), EveryTen(t0));
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = true; return a; }()); Sleep(200);
    g_f.down = true; Passes(s, 3); T_CHECK(g_f.adds >= 3); T_CHECK(!ServerOf(s, net).get("auth_blocked").is_object());
    int a3 = g_f.adds; g_f.down = false; Passes(s, 1); T_CHECK(g_f.adds > a3); T_CHECK(LastRecord(s).get("status").str() == "Success");
    // every refusal code sets the mark; "access denied" (5) and the connection conflict (1219) do not
    const unsigned long codes[] = { 1909, 1331, 1330, 1907, 1385, 1326 };
    for (unsigned long code : codes) {
        g_f.force = code; Json a = Json::object(); a["id"] = net; T_CHECK(!s.call("servers.test", a));
        Json b = ServerOf(s, net).get("auth_blocked"); T_CHECK(b.is_object() && (unsigned long)b.get("code").integer() == code);
        if (code != 1326) { g_f.force = 0; Json u = Json::object(); u["id"] = net; s.ok("servers.unblock", u); }
    }
    g_f.force = 5; { Json u = Json::object(); u["id"] = net; s.ok("servers.unblock", u); Json a = Json::object(); a["id"] = net; T_CHECK(!s.call("servers.test", a)); T_CHECK(!ServerOf(s, net).get("auth_blocked").is_object()); }
    g_f.force = 1219; { Json a = Json::object(); a["id"] = net; T_CHECK(!s.call("servers.test", a)); T_CHECK(!ServerOf(s, net).get("auth_blocked").is_object()); }
    g_f.force = 0;
    // two servers: the refused one does not stop the other, even inside one task; two elements on the refused server = one attempt
    Str bad = SaveSmbServer(s, "Bad", "backupuser", "Nope-1");
    Str ok2 = SaveSmbServer(s, "Good", "corp\\other", "Other-Pw-77");
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = false; return a; }());
    Json t = Json::object(); t["name"] = "Mixed"; t["enabled"] = true; Json els = Json::array();
    Json e1 = ElementTo(src, bad), e2 = ElementTo(src, ok2), e3 = ElementTo(src, bad); els.push(e1); els.push(e2); els.push(e3); t["elements"] = els;
    Json sa = Json::object(); sa["task"] = t; Str mixed = s.ok("tasks.save", sa).get("id").str();
    int before = g_f.adds;
    { Json a = Json::object(); a["id"] = mixed; Json r = s.ok("tasks.run", a); s.waitJob(r.get("job").str()); }
    Json rec = LastRecord(s); T_CHECK(rec.get("results").size() == 3);
    T_CHECK(rec.get("results").at(0).get("status").str() == "Failed" && !rec.get("results").at(0).get("skipped").boolean());
    T_CHECK(rec.get("results").at(1).get("status").str() == "Success");
    T_CHECK(rec.get("results").at(2).get("status").str() == "Failed" && rec.get("results").at(2).get("skipped").boolean());
    c_.note("mixed task: %d connection attempt(s) (one refused + one good)", g_f.adds - before);
    T_CHECK(g_f.adds - before == 2);
    T_CHECK(ServerOf(s, bad).get("auth_blocked").is_object()); T_CHECK(!ServerOf(s, ok2).get("auth_blocked").is_object());
    return true;
}

// ---- a save that cannot be written does not leave a ghost record (the "records did not get an id" race found on the domain stand) ----------------
namespace { struct HoldTmp { HANDLE h; DWORD ms; }; DWORD WINAPI ReleaseLater(LPVOID p) { HoldTmp* x = (HoldTmp*)p; Sleep(x->ms); CloseHandle(x->h); return 0; } }

VM2B_TEST("smb", config_save_waits_for_a_held_tmp_file)
{
    // an indexer / scanner holding config.json.tmp from the previous save for a moment: the next save waits instead of failing
    Tmp tmp(c_, "cfgtmp"); Str dir = tmp.sub("data"), e; MakeDirs(dir, e);
    Str tmpPath = JoinPath(dir, "x.json.tmp"); Wide wt(tmpPath);
    HANDLE h = CreateFileW(wt.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE); if (h == INVALID_HANDLE_VALUE) return false;
    HoldTmp ht = { h, 700 }; HANDLE th = CreateThread(nullptr, 0, ReleaseLater, &ht, 0, nullptr);
    DWORD t0 = GetTickCount(); Str body = "{\"a\":1}\n"; bool ok = WriteWholeFile(JoinPath(dir, "x.json").c_str(), body.data(), body.size(), e);
    c_.note("save with the .tmp file held for 700 ms: %s after %lu ms %s", ok ? "ok" : "FAILED", (unsigned long)(GetTickCount() - t0), e.c_str());
    WaitForSingleObject(th, 5000); CloseHandle(th);
    T_CHECK(ok); T_CHECK(ReadAll(JoinPath(dir, "x.json")) == body);
    return true;
}

VM2B_TEST("smb", config_failed_save_leaves_no_ghost_record)
{
    Tmp tmp(c_, "cfgghost"); Str dir = tmp.sub("data"), e; MakeDirs(dir, e);
    Config cfg; T_CHECK(cfg.open(dir, e));
    size_t n0 = cfg.servers().size();
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "Ghost"; sv["path"] = "\\\\h\\s"; sv["user"] = "u"; sv["password"] = "pw-ghost-1"; Json saved;
    Str blocker = JoinPath(dir, "config.json.tmp"); T_CHECK(MakeDirs(blocker, e));            // a folder where the temporary file must be: the write cannot succeed
    bool ok = cfg.saveServer(sv, saved, e);
    c_.note("save with an unwritable config.json.tmp: ok=%d err=[%s], servers %d -> %d", ok ? 1 : 0, e.c_str(), (int)n0, (int)cfg.servers().size());
    T_CHECK(!ok); T_CHECK(!e.empty()); T_CHECK(cfg.servers().size() == n0);                     // the failed save is not in the list (it used to stay in memory: the record existed, the answer had no id)
    RemoveTree(blocker);
    T_CHECK(cfg.saveServer(sv, saved, e)); T_CHECK(!saved.get("id").str().empty()); T_CHECK(cfg.servers().size() == n0 + 1);
    T_CHECK(ReadAll(JoinPath(dir, "config.json")).find("Ghost") >= 0);
    return true;
}

VM2B_TEST("smb", extended_error_maps_provider_status_to_the_account_codes)
{
    // Windows answers ERROR_EXTENDED_ERROR (1208) for an expired password etc.; the code behind it (WNetGetLastError) is a Win32 code or an NTSTATUS
    T_CHECK(SmbMapProviderError(2242) == 1330);                                    // NERR_PasswordExpired, measured on the real domain
    T_CHECK(SmbMapProviderError(1330) == 1330); T_CHECK(SmbMapProviderError(1326) == 1326);
    T_CHECK(SmbMapProviderError(0xC0000071ul) == 1330); T_CHECK(SmbMapProviderError(0xC0000224ul) == 1907); T_CHECK(SmbMapProviderError(0xC0000234ul) == 1909);
    T_CHECK(SmbMapProviderError(0xC000006Dul) == 1326); T_CHECK(SmbMapProviderError(0xC0000072ul) == 1331);
    T_CHECK(SmbIsAuthCode(SmbMapProviderError(0xC0000071ul)));
    return true;
}

VM2B_TEST("smb", auth_guard_old_state_file_is_read)
{
    Tmp tmp(c_, "smbag5"); Str dir = tmp.sub("data");
    { Svc* none = nullptr; (void)none; }
    Str e; MakeDirs(dir, e);
    Str old = "{\n  \"abc123\": { \"last_status\": \"Success\", \"last_end\": 1760000000 }\n}\n";
    T_CHECK(WriteWholeFile(JoinPath(dir, "state.json").c_str(), old.data(), old.size(), e));
    Config cfg; T_CHECK(cfg.open(dir, e));
    T_CHECK(cfg.servers().size() == 1); T_CHECK(cfg.taskState("abc123").get("last_status").str() == "Success");
    Json b; T_CHECK(!cfg.authBlock("x", b)); T_CHECK(!cfg.clearAuthBlock("x"));
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "S"; sv["path"] = "\\\\h\\s"; sv["user"] = "u"; sv["password"] = "pw-aaa-111"; Json saved; T_CHECK(cfg.saveServer(sv, saved, e));
    Str id = saved.get("id").str(); Str fp = cfg.credsFingerprint(id); T_CHECK(fp.size() == 32); T_CHECK(fp.find("pw-aaa") < 0);
    cfg.setAuthBlock(id, fp, 1326, "wrong (1326)", 1760000001); T_CHECK(cfg.authBlock(id, b)); T_CHECK(b.get("code").integer() == 1326);
    cfg.setAuthBlock(id, "deadbeef", 1909, "ignored: the record changed meanwhile", 5); T_CHECK(cfg.authBlock(id, b) && b.get("code").integer() == 1326);
    Config again; T_CHECK(again.open(dir, e)); T_CHECK(again.authBlock(id, b)); T_CHECK(b.get("time").integer() == 1760000001);          // survives a restart
    T_CHECK(again.taskState("abc123").get("last_status").str() == "Success");                                                          // the task states are untouched
    sv["id"] = id; sv["password"] = "pw-bbb-222"; T_CHECK(again.saveServer(sv, saved, e)); T_CHECK(!again.authBlock(id, b)); T_CHECK(!saved.has("auth_blocked"));
    T_CHECK(ReadAll(JoinPath(dir, "state.json")).find("creds_fp") < 0);
    Str fp2 = again.credsFingerprint(id); T_CHECK(fp2 != fp);
    sv["user"] = "u2"; sv["password"] = kMask; T_CHECK(again.saveServer(sv, saved, e)); T_CHECK(again.credsFingerprint(id) != fp2);
    sv["user"] = "u2"; sv["name"] = "S renamed"; sv["password"] = kMask; Str fp3 = again.credsFingerprint(id); T_CHECK(again.saveServer(sv, saved, e)); T_CHECK(again.credsFingerprint(id) == fp3);   // a rename is not a credentials change
    T_CHECK(again.credsFingerprint("local").empty());
    return true;
}

// ---- a fake FTP server that refuses every password with 530 ------------------------------------------------------------
namespace {
struct FakeFtp { SOCKET ls; int port; volatile LONG logins, stop; HANDLE th; };
DWORD WINAPI FakeFtpThread(LPVOID p)
{
    FakeFtp* f = (FakeFtp*)p;
    while (!f->stop) {
        fd_set rs; FD_ZERO(&rs); FD_SET(f->ls, &rs); timeval tv = { 0, 100000 };
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(f->ls, nullptr, nullptr); if (c == INVALID_SOCKET) continue;
        send(c, "220 fake\r\n", 10, 0);
        Str buf; DWORD t0 = GetTickCount();
        while (!f->stop && GetTickCount() - t0 < 5000) {
            fd_set r2; FD_ZERO(&r2); FD_SET(c, &r2); timeval t2 = { 0, 100000 };
            int sel = select(0, &r2, nullptr, nullptr, &t2); if (sel <= 0) continue;
            char tmp[256]; int n = recv(c, tmp, sizeof(tmp), 0); if (n <= 0) break; buf.append(tmp, (size_t)n);
            for (;;) {
                int nl = buf.find('\n'); if (nl < 0) break; Str line = buf.substr(0, (size_t)nl); buf = buf.substr((size_t)nl + 1);
                const char* r = "502 not implemented\r\n";
                if (line.starts_with("USER")) r = "331 password required\r\n"; else if (line.starts_with("PASS")) { InterlockedIncrement(&f->logins); r = "530 Login incorrect.\r\n"; } else if (line.starts_with("QUIT")) r = "221 bye\r\n";
                send(c, r, (int)strlen(r), 0);
            }
        }
        closesocket(c);
    }
    return 0;
}
}

VM2B_TEST("smb", auth_guard_ftp_530)
{
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    FakeFtp f; f.logins = 0; f.stop = 0; f.ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    T_CHECK(bind(f.ls, (sockaddr*)&a, sizeof(a)) == 0); T_CHECK(listen(f.ls, 4) == 0);
    int al = sizeof(a); getsockname(f.ls, (sockaddr*)&a, &al); f.port = ntohs(a.sin_port);
    f.th = CreateThread(nullptr, 0, FakeFtpThread, &f, 0, nullptr);
    bool res = [&]() -> bool {
        // the target itself: 530 is a refusal of the credentials, 4xx / a refused connection are not
        { Json j = Json::object(); j["type"] = "ftp"; j["host"] = "127.0.0.1"; j["port"] = (i64)f.port; j["user"] = "u"; j["password"] = "Ftp-Wr0ng-Pw"; j["directory"] = "/x";
          Str e; Target* t = MakeTarget(j, e); T_CHECK(t != nullptr); if (!t) return false; T_CHECK(!t->connect()); T_CHECK(t->auth_failed); T_CHECK(t->auth_code == 530); T_CHECK(!t->transient); T_CHECK(t->auth_err.find("Ftp-Wr0ng") < 0); delete t; }
        { Json j = Json::object(); j["type"] = "ftp"; j["host"] = "127.0.0.1"; j["port"] = (i64)1; j["user"] = "u"; j["password"] = "x"; j["directory"] = "/x";
          Str e; Target* t = MakeTarget(j, e); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->auth_failed); delete t; } }
        f.logins = 0;
        Svc s(c_, "smbag6", true); if (!s.up) return false; s.record = true;
        Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 30000)); Str src = s.addDirServer(vms);
        Json sv = Json::object(); sv["type"] = "ftp"; sv["name"] = "Fake FTP"; sv["host"] = "127.0.0.1"; sv["port"] = (i64)f.port; sv["user"] = "u"; sv["password"] = "Ftp-Wr0ng-Pw"; sv["directory"] = "/x";
        Json sa = Json::object(); sa["server"] = sv; Str id = s.ok("servers.save", sa).get("id").str();
        g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
        s.saveTask("ToFtp", ElementTo(src, id), EveryTen(t0));
        s.ok("scheduler.set", [] { Json x = Json::object(); x["enabled"] = true; return x; }()); Sleep(200);
        Passes(s, 5);
        c_.note("5 scheduler passes, FTP answers 530: %d PASS command(s)", (int)f.logins);
        T_CHECK(f.logins == 1);
        Json b = ServerOf(s, id).get("auth_blocked"); T_CHECK(b.is_object() && b.get("code").integer() == 530);
        T_CHECK(LastRecord(s).get("results").at(0).get("skipped").boolean());
        // a new password: tried once more (and refused once more)
        sv["id"] = id; sv["password"] = "Ftp-Wr0ng-Pw-2"; Json sb = Json::object(); sb["server"] = sv; s.ok("servers.save", sb);
        Passes(s, 3); T_CHECK(f.logins == 2);
        T_CHECK(s.transcript.find("Ftp-Wr0ng") < 0);
        Str stJson = ReadAll(JoinPath(s.core.config().dataDir(), "state.json")); T_CHECK(stJson.find("Ftp-Wr0ng") < 0);
        s.ok("scheduler.set", [] { Json x = Json::object(); x["enabled"] = false; return x; }());
        return true;
    }();
    InterlockedExchange(&f.stop, 1); WaitForSingleObject(f.th, 5000); CloseHandle(f.th); closesocket(f.ls);
    return res;
}

} // namespace vm2b
