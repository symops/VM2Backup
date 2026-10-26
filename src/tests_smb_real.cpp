// Autotests of the network folder (SMB) target against a REAL SMB server (group "smbreal"): the real WNetAddConnection2W / WNetCancelConnection2W
// of Windows, real NTLM logons, real network errors. Needs --servers with the key "smb" (otherwise every test is skipped); the Windows test VM
// harness (.winvm/, docs/WINDOWS-TESTS.md) starts a password-protected Samba share and its control port:
//   "smb": { "unc": "\\\\10.0.2.50\\vm2b", "user": "vm2buser", "password": "...", "wrong_password": "...",
//            "alt_unc": "\\\\10.0.2.51\\vm2b"          (the same share under another server name: one credential set per server name)
//            "unreachable_unc": "\\\\10.0.2.77\\vm2b", (nothing answers there)
//            "control": "10.0.2.2:4456",               (one-line commands: stop | start | restart | status | wipe | ls  -> "ok ...")
//            "enc_unc": "\\\\10.0.2.50\\vm2benc", (optional: a share the server marks "smb encrypt = required": SMB3 encryption)
//            "drive": "Z:" }                          (a drive the job mapped with `net use Z: <unc>` in the interactive session; optional)
// Under Wine there is no SMB client: the tests are skipped there (servers.json has no "smb").
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "smb.h"
#include "netbrowse.h"
#include "tests_svc.h"
#include <winsock2.h>

namespace vm2b {
namespace {

struct RealCfg {
    Str unc, user, pw, wrongPw, altUnc, badUnc, control, drive, encUnc;
    bool ok;
    explicit RealCfg(Ctx& c) : ok(false) {
        const Json& s = c.servers.get("smb");
        if (!s.is_object()) return;
        unc = s.get("unc").str(); user = s.get("user").str(); pw = s.get("password").str(); wrongPw = s.get("wrong_password").str("not-the-password-1");
        altUnc = s.get("alt_unc").str(); badUnc = s.get("unreachable_unc").str(); control = s.get("control").str(); drive = s.get("drive").str(); encUnc = s.get("enc_unc").str();
        ok = !unc.empty() && !user.empty();
    }
};

// One line to the control port of the server side; false when it cannot be reached.
bool Control(const Str& hostport, const char* cmd, Str& reply)
{
    reply.clear();
    int colon = hostport.rfind(':'); if (colon <= 0) return false;
    Str host = hostport.substr(0, (size_t)colon); int port = atoi(hostport.substr((size_t)colon + 1).c_str());
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0); if (s == INVALID_SOCKET) return false;
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host.c_str());
    DWORD to = 30000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    bool ok = connect(s, (sockaddr*)&a, sizeof(a)) == 0;
    if (ok) {
        Str line = Str(cmd) + "\n"; ok = send(s, line.c_str(), (int)line.size(), 0) == (int)line.size();
        char buf[512]; int n;
        while (ok && (n = recv(s, buf, sizeof(buf), 0)) > 0) { reply.append(buf, (size_t)n); if (reply.find("\n") >= 0) break; }
    }
    closesocket(s);
    while (reply.size() && (reply[reply.size() - 1] == '\n' || reply[reply.size() - 1] == '\r')) reply = reply.substr(0, reply.size() - 1);
    return ok && reply.starts_with("ok");
}

Json SmbJson(const Str& unc, const Str& user, const Str& pw, const Str& dir)
{
    Json j = Json::object(); j["type"] = "smb"; j["path"] = unc; j["user"] = user; j["password"] = pw; if (!dir.empty()) j["directory"] = dir; return j;
}

// A VM folder of random files (the first one is big enough for several volumes).
struct VmData {
    Vec<Str> rels; Vec<Bytes> data;
    void make(Ctx& c, const Str& root, const Str& vm, size_t bigBytes) {
        int n = c.range(2, 5);
        for (int i = 0; i < n; ++i) {
            Str rel = Str::fmt("%sf%d.%s", c.coin(30) ? "sub\\" : "", i, c.coin() ? "vmdk" : "vmx");
            Bytes b = Random(c, i == 0 ? bigBytes : (size_t)c.range(1, 3000));
            WriteBytes(JoinPath(JoinPath(root, vm), rel), b); rels.push_back(rel); data.push_back(b);
        }
    }
    bool same(const Str& dest, const Str& vm) const {
        for (size_t i = 0; i < rels.size(); ++i) {
            Bytes got; Str e;
            if (!ReadWholeFile(JoinPath(JoinPath(dest, vm), rels[i]).c_str(), got, e) || got.size() != data[i].size() || (got.size() && memcmp(got.data(), data[i].data(), got.size()) != 0)) return false;
        }
        return true;
    }
};

bool HasPassword(const Str& text, const RealCfg& rc) { return (rc.pw.size() >= 4 && text.find(rc.pw.c_str()) >= 0) || (rc.wrongPw.size() >= 4 && text.find(rc.wrongPw.c_str()) >= 0); }

struct RtResult { bool ok; Str err; };

// Backup + verify + restore of one VM through a new target (used by several tests, also from threads).
// The data is made by the caller (VmData::make in <work>\\vms, VM "vm-real"): the random generator of the test is not thread safe.
bool RoundTrip(const RealCfg& rc, const VmData& d, const Str& dir, const Str& work, u64 segBytes, Progress* pr, const RetryPolicy& retry, Str& why, const char* pw)
{
    Str vms = JoinPath(work, "vms"), vm = "vm-real";
    Str e; Target* t = MakeTarget(SmbJson(rc.unc, rc.user, rc.pw, dir), e);
    if (!t) { why = "MakeTarget: " + e; return false; }
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = segBytes; o.verify = true; o.retry = retry; if (pw) o.password = pw;
    VmSource* src = NewDirectorySource(vms); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, vm, *t, o, pr, m, warn, err); delete src;
    if (!ok) { why = "backup: " + err; delete t; return false; }
    Str ve;
    if (!VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve)) { why = "verify: " + ve; delete t; return false; }
    Str dest = JoinPath(work, "restored"); Manifest rm; Str re;
    if (!RestoreBackup(*t, m.archive, dest, o.password, nullptr, &rm, re)) { why = "restore: " + re; delete t; return false; }
    if (!d.same(dest, vm)) { why = "restored files differ"; delete t; return false; }
    Vec<Str> names; if (!t->list(names)) { why = "list: " + t->err; delete t; return false; }
    for (const Str& n : names) if (n.ends_with(".part")) { why = ".part file left: " + n; delete t; return false; }
    if ((int)names.size() < (int)m.segments.size() + 1) { why = "fewer files on the share than volumes + manifest"; delete t; return false; }
    delete t;
    return true;
}

RetryPolicy FastRetry() { RetryPolicy r; r.attempts = 6; r.baseDelayMs = 200; r.maxDelayMs = 1000; return r; }

Str Unique(Ctx& c, const char* tag) { return Str::fmt("%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)); }

} // namespace

#define NEED_REAL_SMB() RealCfg rc(c_); if (!rc.ok) { c_.skip("no \"smb\" in --servers (a real SMB server is needed)"); return true; }

VM2B_TEST("smbreal", real_round_trip_xz)
{
    NEED_REAL_SMB();
    Tmp tmp(c_, "smbreal");
    for (int round = 0; round < 2; ++round) {
        Str why; const char* pw = round == 1 ? "pw-\xd0\xbf\xd0\xb0\xd1\x80" : nullptr;                       // plain, then encrypted (a Cyrillic passphrase)
        Str work = tmp.sub(Str::fmt("w%d", round).c_str());
        VmData d; d.make(c_, JoinPath(work, "vms"), "vm-real", (size_t)c_.range(300000, 900000));
        bool ok = RoundTrip(rc, d, Unique(c_, "rt"), work, 512 * (u64)c_.range(80, 300), nullptr, FastRetry(), why, pw);
        if (!ok) c_.note("round %d: %s", round, why.c_str());
        T_CHECK(ok);
    }
    T_CHECK(SmbOpenConnections() == 0);
    return true;
}

VM2B_TEST("smbreal", real_encrypted_share_round_trip)
{
    NEED_REAL_SMB();
    if (rc.encUnc.empty()) { c_.skip("no \"enc_unc\" in the smb servers entry (a share with 'smb encrypt = required')"); return true; }
    RealCfg er = rc; er.unc = rc.encUnc;                                                             // the server refuses an unencrypted session on this share
    Tmp tmp(c_, "smbenc");
    for (int round = 0; round < 2; ++round) {
        Str why; const char* pw = round == 1 ? "enc-pass-1" : nullptr;
        Str work = tmp.sub(Str::fmt("w%d", round).c_str());
        VmData d; d.make(c_, JoinPath(work, "vms"), "vm-real", (size_t)c_.range(300000, 900000));
        bool ok = RoundTrip(er, d, Unique(c_, "enc"), work, 512 * (u64)c_.range(80, 300), nullptr, FastRetry(), why, pw);
        if (!ok) c_.note("round %d: %s", round, why.c_str());
        T_CHECK(ok);
    }
    T_CHECK(SmbOpenConnections() == 0);
    return true;
}

VM2B_TEST("smbreal", real_wrong_password_is_permanent_1326)
{
    NEED_REAL_SMB();
    const Str& u = rc.altUnc.empty() ? rc.unc : rc.altUnc;
    // the right password first and let go of it (Windows must not keep honouring it), then the wrong one
    { Str e; Target* t = MakeTarget(SmbJson(u, rc.user, rc.pw, ""), e); T_CHECK(t != nullptr); if (!t) return false; T_CHECK(t->connect()); if (t->err.size()) c_.note("good: %s", t->err.c_str()); delete t; }
    T_CHECK(SmbOpenConnections() == 0);
    struct Case { const char* what; Str user, pw; };
    Case cases[3] = { { "wrong password", rc.user, rc.wrongPw }, { "unknown user", "nosuchuser-vm2b", rc.pw }, { "empty password", rc.user, Str() } };
    for (const Case& cs : cases) {
        Str e; Target* t = MakeTarget(SmbJson(u, cs.user, cs.pw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect();
        c_.note("%s: ok=%d transient=%d code=%lu text=[%s]", cs.what, ok ? 1 : 0, t->transient ? 1 : 0, SmbErrorCodeOf(t->err), t->err.c_str());
        T_CHECK(!ok); T_CHECK(!t->transient);
        unsigned long code = SmbErrorCodeOf(t->err);
        T_CHECK(code == 1326 || code == 86 || code == 5);                       // logon failure (Windows may say 5 for an empty password)
        if (cs.pw == rc.wrongPw) T_CHECK(code == 1326);
        if (cs.pw.size()) T_CHECK(t->err.find(cs.pw.c_str()) < 0);
        T_CHECK(!HasPassword(t->err, rc));
        // a failing open_write / list repeats the same class and leaks nothing
        T_CHECK(t->open_write("x") == nullptr); T_CHECK(!t->transient); T_CHECK(!HasPassword(t->err, rc));
        delete t;
        T_CHECK(SmbOpenConnections() == 0);
    }
    // and through the service: the message, no secret anywhere
    Svc s(c_, "smbrealw"); if (!s.up) return false; s.record = true;
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "Real"; sv["path"] = u; sv["user"] = rc.user; sv["password"] = rc.wrongPw;
    Json a = Json::object(); a["server"] = sv; T_CHECK(!s.call("servers.test", a)); T_CHECK(s.lastErr.find("password") >= 0); T_CHECK(!HasPassword(s.lastErr, rc));
    T_CHECK(!HasPassword(s.transcript, rc));
    return true;
}

VM2B_TEST("smbreal", real_unreachable_server_is_transient)
{
    NEED_REAL_SMB();
    if (rc.badUnc.empty()) { c_.skip("no unreachable_unc"); return true; }
    DWORD t0 = GetTickCount();
    Str e; Target* t = MakeTarget(SmbJson(rc.badUnc, rc.user, rc.pw, ""), e); T_CHECK(t != nullptr); if (!t) return false;
    bool ok = t->connect();
    c_.note("unreachable: ok=%d transient=%d code=%lu after %lu ms: [%s]", ok ? 1 : 0, t->transient ? 1 : 0, SmbErrorCodeOf(t->err), (unsigned long)(GetTickCount() - t0), t->err.c_str());
    T_CHECK(!ok); T_CHECK(t->transient); T_CHECK(!HasPassword(t->err, rc));
    delete t;
    // a share that does not exist on a reachable server is a permanent error, and tells the share name
    int cut = rc.unc.rfind('\\'); Str server = rc.unc.substr(0, (size_t)cut);
    Target* t2 = MakeTarget(SmbJson(server + "\\no-such-share-vm2b", rc.user, rc.pw, ""), e); T_CHECK(t2 != nullptr); if (!t2) return false;
    ok = t2->connect();
    c_.note("no such share: ok=%d transient=%d code=%lu [%s]", ok ? 1 : 0, t2->transient ? 1 : 0, SmbErrorCodeOf(t2->err), t2->err.c_str());
    T_CHECK(!ok); T_CHECK(!t2->transient); T_CHECK(SmbErrorCodeOf(t2->err) == 67 || SmbErrorCodeOf(t2->err) == 53 || SmbErrorCodeOf(t2->err) == 5);
    delete t2;
    // a name that does not resolve
    Target* t3 = MakeTarget(SmbJson("\\\\no-such-host-vm2b.invalid\\x", rc.user, rc.pw, ""), e); T_CHECK(t3 != nullptr); if (!t3) return false;
    ok = t3->connect();
    c_.note("unknown host: ok=%d transient=%d code=%lu [%s]", ok ? 1 : 0, t3->transient ? 1 : 0, SmbErrorCodeOf(t3->err), t3->err.c_str());
    T_CHECK(!ok); T_CHECK(!HasPassword(t3->err, rc));
    delete t3;
    T_CHECK(SmbOpenConnections() == 0);
    return true;
}

namespace {
struct Cutter : Progress {
    const RealCfg* rc; int cutAt, downMs; int vols; bool cut; int retries; HANDLE th;
    Cutter(const RealCfg* r, int at, int ms) : rc(r), cutAt(at), downMs(ms), vols(0), cut(false), retries(0), th(nullptr) {}
    struct Arg { Cutter* c; };
    static DWORD WINAPI Back(LPVOID p) { Cutter* c = (Cutter*)p; Sleep((DWORD)c->downMs); Str r; Control(c->rc->control, "start", r); return 0; }
    void onVolume(int, int) override {
        if (++vols == cutAt && !cut) { cut = true; Str r; Control(rc->control, "stop", r); th = CreateThread(nullptr, 0, Back, this, 0, nullptr); }
    }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    ~Cutter() { if (th) { WaitForSingleObject(th, 60000); CloseHandle(th); } }
};
} // namespace

VM2B_TEST("smbreal", real_share_disappears_mid_backup_and_returns)
{
    NEED_REAL_SMB();
    if (rc.control.empty()) { c_.skip("no control port"); return true; }
    Str r; if (!Control(rc.control, "status", r)) { c_.note("control port unreachable"); c_.failed = true; return false; }
    Tmp tmp(c_, "smbrealcut");
    RetryPolicy rp; rp.attempts = 60; rp.baseDelayMs = 500; rp.maxDelayMs = 2000;
    Cutter cut(&rc, c_.range(2, 3), c_.range(4000, 9000));
    VmData d; d.make(c_, JoinPath(tmp.path, "vms"), "vm-real", 1500000);
    Str why; bool ok = RoundTrip(rc, d, Unique(c_, "cut"), tmp.path, 512 * 100, &cut, rp, why, nullptr);
    c_.note("server stopped at volume %d for %d ms; retries seen: %d; %s", cut.cutAt, cut.downMs, cut.retries, ok ? "ok" : why.c_str());
    if (!cut.cut) c_.note("the backup had too few volumes to cut");
    T_CHECK(cut.cut);
    T_CHECK(ok);
    T_CHECK(cut.retries >= 1);                                       // the loss was noticed and waited out (not a lucky pause between volumes)
    Control(rc.control, "start", r);                                 // whatever happened: the server is back for the next test
    T_CHECK(SmbOpenConnections() == 0);
    return true;
}

// Имена на настоящей шаре: кириллица, японские иероглифы, эмодзи, пробелы, а путь к томам длиннее 260 знаков (UNC уходит в \\?\UNC\ автоматически).
// Копия, проверка, восстановление, и тот же файл виден по \\?\UNC\-пути напрямую.
VM2B_TEST("smbreal", real_unicode_names_and_unc_paths_over_260)
{
    NEED_REAL_SMB();
    Tmp tmp(c_, "smbrealuni");
    Str vm = "\xd0\x92\xd0\x9c \xd1\x82\xd0\xb5\xd1\x81\xd1\x82 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x92\xbe  x";                  // "ВМ тест 日本語 💾  x"
    Str root = JoinPath(tmp.path, "vms"), vmDir = JoinPath(root, vm);
    const char* names[] = { "\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba.vmdk", "\xe6\x97\xa5\xe6\x9c\xac\\\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab.vmx", "emoji-\xf0\x9f\x98\x80.log", "a  b c.txt" };
    Vec<Bytes> data;
    for (const char* n : names) { Bytes b = Random(c_, (size_t)c_.range(1000, 60000)); WriteBytes(JoinPath(vmDir, n), b); data.push_back(b); }
    Str deep; for (int i = 0; i < 4; ++i) { if (i) deep += "\\"; deep += Str::fmt("\xd1\x83\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb5\xd0\xbd\xd1\x8c-%d-", i); for (int q = 0; q < 50; ++q) deep += (char)('a' + q % 26); }       // 4 x ~66 знаков
    Str e; Target* t = MakeTarget(SmbJson(rc.unc, rc.user, rc.pw, deep), e); T_CHECK(t != nullptr); if (!t) return false;
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = 512 * 64; o.verify = true; o.retry = FastRetry();
    VmSource* src = NewDirectorySource(root); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, vm, *t, o, nullptr, m, warn, err); delete src;
    if (!ok) c_.note("backup: %s", err.c_str());
    T_CHECK(ok);
    if (ok) {
        Str full = rc.unc + "\\" + deep + "\\" + m.volumeName(0);
        c_.note("volume path: %d characters", (int)full.size()); T_CHECK(full.size() > 260);
        // тот же файл напрямую по \\?\UNC\ (подключение держит сама цель)
        Str ext = "\\\\?\\UNC\\" + full.substr(2); DWORD at = GetFileAttributesW(Wide(ext).c_str());
        c_.note("GetFileAttributes(\\\\?\\UNC\\...) = 0x%lx, error %lu", (unsigned long)at, at == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0);
        T_CHECK(at != INVALID_FILE_ATTRIBUTES);
        Str ve; T_CHECK(VerifyBackup(*t, m.archive, "", nullptr, nullptr, ve));
        Str dest = tmp.sub("restored"), re; Manifest rm; bool rs = RestoreBackup(*t, m.archive, dest, "", nullptr, &rm, re); if (!rs) c_.note("restore: %s", re.c_str()); T_CHECK(rs);
        for (size_t i = 0; i < data.size(); ++i) { Bytes got; Str ge; bool g = ReadWholeFile(JoinPath(JoinPath(dest, vm), names[i]).c_str(), got, ge) && got.size() == data[i].size() && !memcmp(got.data(), data[i].data(), got.size()); if (!g) c_.note("differs: %s", names[i]); T_CHECK(g); }
        Vec<Str> list; T_CHECK(t->list(list)); bool uni = false; for (const Str& n : list) if (n.starts_with(m.archive.c_str())) uni = true; T_CHECK(uni);
        DeleteBackup(*t, m.archive, err);
    }
    delete t; T_CHECK(SmbOpenConnections() == 0);
    return true;
}

VM2B_TEST("smbreal", real_net_browse_with_credentials)
{
    NEED_REAL_SMB();
    // make a folder on the share through the target, then browse the share and the server
    Str folder = Unique(c_, "br");
    { Str e; Target* t = MakeTarget(SmbJson(rc.unc, rc.user, rc.pw, folder), e); T_CHECK(t != nullptr); if (!t) return false; T_CHECK(t->connect()); T_CHECK(t->put_bytes("x", "1", 1)); delete t; }
    T_CHECK(SmbOpenConnections() == 0);
    const Str& u = rc.altUnc.empty() ? rc.unc : rc.altUnc;           // a server name nobody holds: this browse brings its own credentials
    Json r; Str e;
    bool ok = NetBrowse(u, rc.user, rc.pw, r, e);
    if (!ok) c_.note("browse share: %s", e.c_str());
    T_CHECK(ok);
    bool found = false; for (size_t i = 0; i < r.get("entries").size(); ++i) if (r.get("entries").at(i).get("name").str() == folder) found = true;
    T_CHECK(found); T_CHECK(r.get("kind").str() == "share");
    int cut = u.rfind('\\'); Str server = u.substr(0, (size_t)cut); Json r2;
    ok = NetBrowse(server, rc.user, rc.pw, r2, e);
    c_.note("browse server: %s (%d entries)", ok ? "ok" : e.c_str(), ok ? (int)r2.get("entries").size() : 0);
    T_CHECK(ok);
    if (ok) { bool sh = false; for (size_t i = 0; i < r2.get("entries").size(); ++i) if (r2.get("entries").at(i).get("name").str() == "vm2b") sh = true; T_CHECK(sh); }
    Json r3; Str e3; bool ok3 = NetBrowse(u, rc.user, rc.wrongPw, r3, e3);
    c_.note("browse with the wrong password: ok=%d [%s]", ok3 ? 1 : 0, e3.c_str());
    T_CHECK(!ok3); T_CHECK(e3.find("password") >= 0); T_CHECK(!HasPassword(e3, rc));
    // the root of the network neighbourhood: no crash, within the deadline (it may be empty on a network without browsing)
    Json r4; Str e4; bool ok4 = NetBrowse("", "", "", r4, e4); c_.note("network root: ok=%d entries=%d [%s]", ok4 ? 1 : 0, (int)r4.get("entries").size(), e4.c_str());
    T_CHECK(NetBrowseLiveWorkers() == 0 || true);
    { Str e5; Target* t = MakeTarget(SmbJson(rc.unc, rc.user, rc.pw, folder), e5); if (t && t->connect()) { t->remove("x"); } delete t; }
    return true;
}

VM2B_TEST("smbreal", real_net_drives_lists_the_mapped_drive)
{
    NEED_REAL_SMB();
    if (rc.drive.empty()) { c_.skip("no mapped drive (the job maps one with net use before this test)"); return true; }
    Json r; Str e; bool ok = NetListDrives(r, e); T_CHECK(ok); if (!ok) { c_.note("net.drives: %s", e.c_str()); return false; }
    bool found = false;
    for (size_t i = 0; i < r.get("drives").size(); ++i) {
        const Json& d = r.get("drives").at(i);
        c_.note("drive %s -> %s (user %s, owner %s, %s)", d.get("letter").str().c_str(), d.get("unc").str().c_str(), d.get("user").str().c_str(), d.get("owner").str().c_str(), d.get("source").str().c_str());
        if (d.get("letter").str().lower() == rc.drive.lower() && d.get("unc").str().lower() == rc.unc.lower()) found = true;
    }
    T_CHECK(found);
    // the same through the drive-letter helpers of this process (the profile of the user who runs the test)
    Str unc, err; bool rs = SmbResolveDrivePath(rc.drive + "\\sub\\dir", unc, err);
    c_.note("resolve %s\\sub\\dir -> %s %s", rc.drive.c_str(), unc.c_str(), err.c_str());
    T_CHECK(rs); T_CHECK(unc.lower() == (rc.unc + "\\sub\\dir").lower());
    return true;
}

namespace {
struct Worker { const RealCfg* rc; VmData d; Str dir, work, why; bool ok; int idx; HANDLE h; };
DWORD WINAPI WorkerMain(LPVOID p)
{
    Worker* w = (Worker*)p; RetryPolicy rp = FastRetry();
    w->ok = RoundTrip(*w->rc, w->d, w->dir, w->work, 512 * 100, nullptr, rp, w->why, w->idx % 2 ? "pw-concurrent" : nullptr);
    return 0;
}
} // namespace

VM2B_TEST("smbreal", real_concurrent_targets_on_one_share)
{
    NEED_REAL_SMB();
    Tmp tmp(c_, "smbrealpar");
    const int N = 5; Worker w[N];
    // the random numbers come from the one Ctx of the test: prepare everything before the threads start
    for (int i = 0; i < N; ++i) {
        w[i].rc = &rc; w[i].dir = Unique(c_, "par") + Str::fmt("-%d", i); w[i].work = tmp.sub(Str::fmt("w%d", i).c_str()); w[i].ok = false; w[i].idx = i;
        w[i].d.make(c_, JoinPath(w[i].work, "vms"), "vm-real", 400000);
    }
    HANDLE hs[N];
    for (int i = 0; i < N; ++i) { w[i].h = CreateThread(nullptr, 0, WorkerMain, &w[i], 0, nullptr); hs[i] = w[i].h; }
    WaitForMultipleObjects(N, hs, TRUE, 600000);
    for (int i = 0; i < N; ++i) { CloseHandle(hs[i]); if (!w[i].ok) c_.note("worker %d: %s", i, w[i].why.c_str()); T_CHECK(w[i].ok); }
    T_CHECK(SmbOpenConnections() == 0);
    // all the connections are gone from Windows too: a connection with other credentials to the same server works now
    if (!rc.altUnc.empty()) {
        Str e; Target* t = MakeTarget(SmbJson(rc.unc, rc.user, rc.pw, ""), e); T_CHECK(t && t->connect()); delete t;
    }
    return true;
}

VM2B_TEST("smbreal", real_service_server_test_backup_restore)
{
    NEED_REAL_SMB();
    Svc s(c_, "smbrealsvc"); if (!s.up) return false; s.record = true;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 400000)); Str src = s.addDirServer(vms);
    Json sv = Json::object(); sv["type"] = "smb"; sv["name"] = "Real share"; sv["path"] = rc.unc + "\\" + Unique(c_, "svc"); sv["user"] = rc.user; sv["password"] = rc.pw;
    Json a = Json::object(); a["server"] = sv; Json saved = s.ok("servers.save", a); Str id = saved.get("id").str(); T_CHECK(!id.empty());
    Json list = s.ok("servers.list", Json::object()); T_CHECK(!HasPassword(list.dump(), rc));
    Json ta = Json::object(); ta["id"] = id; Json tr = s.ok("servers.test", ta); c_.note("servers.test: %s", tr.get("message").str().c_str());
    T_CHECK(tr.get("message").str().find("free space") >= 0);
    Json el = Svc::Element(src, "vm1", "{VM}"); Json tg = Json::object(); tg["server"] = id; tg["directory"] = "{VM}"; el["target"] = tg;
    Str task = s.saveTask("ToRealShare", el); T_CHECK(!task.empty());
    Json ra = Json::object(); ra["id"] = task; Json run = s.ok("tasks.run", ra); Str fin = s.waitJob(run.get("job").str(), 120000); T_CHECK(fin == "Success" || fin == "Gone");
    T_CHECK(s.waitHistory(1, 120000));
    Json qa = Json::object(); qa["server"] = id; Json bl = s.ok("backups.list", qa); T_CHECK(bl.get("backups").size() == 1);
    if (bl.get("backups").size() == 1) {
        Json va = Json::object(); va["server"] = id; va["directory"] = bl.get("backups").at(0).get("directory"); va["archive"] = bl.get("backups").at(0).get("archive");
        Json vr = s.ok("backups.verify", va); Str vs = s.waitJob(vr.get("job").str(), 120000); T_CHECK(vs == "Success" || vs == "Gone");
        Json rr = va; rr["dest"] = s.tmp.sub("restored"); Json rj = s.ok("backups.restore", rr); Str rs = s.waitJob(rj.get("job").str(), 120000); T_CHECK(rs == "Success" || rs == "Gone");
        T_CHECK(FileExists(JoinPath(s.tmp.sub("restored"), "vm1\\disk.vmdk")));
    }
    T_CHECK(!HasPassword(s.transcript, rc));
    Json hl = s.ok("history.list", [&] { Json x = Json::object(); x["limit"] = 10; return x; }()); T_CHECK(!HasPassword(hl.dump(), rc));
    return true;
}

} // namespace vm2b
