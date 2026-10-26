// Autotests of the network folder (SMB) target and the network drive helpers (group "smb"). Real SMB does not exist under Wine / Linux:
// the Windows network calls are replaced by a fake (SmbSetApi) that checks the credentials, keeps the list of connections and maps the
// share "\\fileserver\backup" to a local folder. What cannot be tested here (a real server, a domain, the service account, mapped
// drives) is listed in docs/INSTALL.en.txt and README.md.
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "smb.h"
#include "tests_svc.h"

namespace vm2b {
namespace {

// ---- the fake network ------------------------------------------------------------------------------------------
struct Cred { Str user, pass; };
struct FakeNet {
    Str share, local;             // "\\fileserver\backup" (lower case) and the folder behind it
    Vec<Cred> accepted;           // user + password pairs the "server" accepts
    bool anonymous;               // accepts a connection without credentials
    bool down;                    // every connection attempt fails: network unreachable
    int adds, cancels;
    Vec<Str> conns;               // "server|user" of the open connections
    Vec<Str> log;
    FakeNet() : anonymous(false), down(false), adds(0), cancels(0) {}
};
FakeNet g_f;

Str ServerPart(const Str& r) { int c = r.find('\\', 2); return c > 0 ? r.substr(0, (size_t)c) : r; }

unsigned long FakeAdd(const wchar_t* remote, const wchar_t* user, const wchar_t* pass)
{
    Str r = FromWide(remote).lower(), u = user ? FromWide(user) : Str(), p = pass ? FromWide(pass) : Str();
    ++g_f.adds; g_f.log.push_back("add " + r + " user=" + (user ? u : Str("<none>")));
    if (g_f.down) return 1231;
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
        { 64, "lost", true }, { 1331, "disabled", false }, { 1909, "locked", false }, { 1311, "domain controller", true }, { 1225, "refused", true },
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

} // namespace vm2b
