// Autotests of the SERVICE-LEVEL paths (everything goes through ServiceCore and its pipe, as the Windows program and the web site do):
//   group "svcesxi" - ESXi / vCenter through the service commands against the emulators of tools/testservers/esxi_mock.py
//                     (tools/run-esxi-tests.sh starts them and passes the addresses with --servers, like the "esxi" group);
//   group "svcweb"  - the administration web site hosted by the service (web.* routing, defaults, restart, the HTTPS API with roles and TOTP).
// The pieces of tests_esxi.cpp / tests_web.cpp that are needed here are small and local to this file (their helpers are file-private there).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "autotest.h"
#include "applog.h"
#include "crypto.h"
#include "engine.h"
#include "esxi.h"
#include "http.h"
#include "webauth.h"
#include "winutil.h"
#include "tests_svc.h"

namespace vm2b {

namespace {

// ---- the emulators (same servers json as tests_esxi.cpp) --------------------------------------------------------------------
struct Env {
    Str name, profile, kind, host, user, password, fp, tlsMin, root, tlsOnly, cert; int port; Vec<Str> dcs;
    explicit Env(const Json& j) {
        name = j.get("name").str(); profile = j.get("profile").str(); kind = j.get("kind").str(); host = j.get("host").str(); user = j.get("user").str();
        password = j.get("password").str(); fp = j.get("fingerprint").str(); tlsMin = j.get("tls_min").str("1.2"); root = j.get("root").str();
        tlsOnly = j.get("tls_only").str(); cert = j.get("cert").str(); port = (int)j.get("port").integer();
        const Json& d = j.get("dcs"); for (size_t i = 0; i < d.size(); ++i) dcs.push_back(d.at(i).str());
    }
    bool vcenter() const { return kind == "vcenter"; }
    // the server record the Add Server dialog saves (dlg_server.cpp collect()): verify + pinned fingerprint, tls_min, transport "api"; the tuning keys are for the test
    Json record() const {
        Json c = Json::object();
        c["type"] = kind; c["name"] = "Lab " + name; c["host"] = host; c["port"] = port; c["user"] = user; c["password"] = password;
        c["verify"] = true; c["fingerprint"] = fp; c["tls_min"] = tlsMin; c["transport"] = "api";
        c["task_timeout"] = 60; c["remove_timeout"] = 60; c["poll_ms"] = 20; c["timeout"] = 20;
        return c;
    }
};

bool Envs(Ctx& c, Vec<Env>& out)
{
    if (c.servers.is_null() || !c.servers.has("esxi")) { c.skip("no \"esxi\" servers (run tools/run-esxi-tests.sh)"); return false; }
    const Json& a = c.servers.get("esxi");
    for (size_t i = 0; i < a.size(); ++i) out.push_back(Env(a.at(i)));
    return !out.empty();
}

struct Ctl {
    HttpClient cl;
    static HttpOptions Opts(const Env& e) {
        HttpOptions o; o.host = e.host; o.port = e.port; o.tls = true; o.tlsOpt.fingerprint = e.fp; o.tlsOpt.tlsMin = e.tlsMin; o.tlsOpt.timeoutMs = 30000;
        return o;
    }
    explicit Ctl(const Env& e) : cl(Opts(e)) {}
    bool call(const char* cmd, const Json& a, Json& out) {
        Str body = a.dump(); HttpHeaders h; HttpResponse r; Bytes b;
        bool post = !a.is_null();
        if (!cl.requestBytes(post ? "POST" : "GET", Str("/__mock/") + cmd, h, post ? body.data() : nullptr, post ? body.size() : 0, r, b)) return false;
        Str e; Json::parse((const char*)b.data(), b.size(), out, e);
        return r.status == 200;
    }
    Json state() { Json o; call("state", Json(), o); return o; }
    void reset() { Json o; call("reset", Json::object(), o); }
    Json vmState(const Str& vm) { Json s = state(); return s.get("vms").get(vm.c_str()); }
    int leaked() { Json s = state(); return (int)s.get("leaked").size(); }
    int counter(const char* key) { Json s = state(); return (int)s.get("counters").get(key).integer(); }
    int events(const char* what) {                          // "create" / "remove" events with a VM2Backup- snapshot name
        Json s = state(); int n = 0; const Json& ev = s.get("events");
        for (size_t i = 0; i < ev.size(); ++i) if (ev.at(i).at(0).str() == what && ev.at(i).at(2).str().starts_with("VM2Backup-")) ++n;
        return n;
    }
    void deleteVm(const Str& name) { Json o, a = Json::object(); a["name"] = name; call("delete_vm", a, o); }
};

struct VmSpec { Str name; Json disks; Str power; Str dc; };
struct MockVm { Str name, dir, ds; bool ok; MockVm() : ok(false) {} };

MockVm MakeVm(Ctl& ctl, const Env& e, const VmSpec& s)
{
    Json a = Json::object();
    a["name"] = s.name; a["disks"] = s.disks; a["power"] = s.power; a["ds"] = "datastore1"; a["template"] = false; a["conn"] = "connected";
    if (!s.dc.empty()) a["dc"] = s.dc;
    a["seed"] = (i64)(s.name.size() * 31 + 7);
    Json out; MockVm m;
    m.ok = ctl.call("create_vm", a, out) && !out.has("error");
    m.name = s.name; m.ds = out.get("ds").str(); m.dir = JoinPath(JoinPath(e.root, m.ds), out.get("dir").str());
    return m;
}

Json Disks(Ctx& c, int n, int maxSize, int minSize)
{
    Json d = Json::array();
    for (int i = 0; i < n; ++i) { Json o = Json::object(); o["size"] = (i64)c.range(minSize, maxSize); o["thin"] = false; o["seed"] = (i64)c.range(1, 100000); d.push(o); }
    return d;
}

const char* const kNames[] = { "plain", "with space", "\xd0\xb2\xd0\xbc \xd1\x82\xd0\xb5\xd1\x81\xd1\x82", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "a&b+c=d", "caf\xc3\xa9 (copy) [1]", "x#hash" };
Str UniqueName(Ctx& c, const char* tag) { return Str::fmt("%s-%u-", tag, c.rnd(1000000)) + kNames[c.rnd(7)]; }

// ---- hashing a tree -----------------------------------------------------------------------------------------------------------
struct FI { Str rel; u64 size; Str sha; };

Str HashFile(const Str& path, u64& size)
{
    size = 0;
    HANDLE h = CreateFileW(FilePath(path), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Str();
    Sha256Stream s; Bytes buf; buf.resize(1 << 20);
    for (;;) { DWORD got = 0; if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) || got == 0) break; s.update(buf.data(), got); size += got; }
    CloseHandle(h);
    u8 d[32]; s.final(d);
    return HexEncode(d, 32);
}

void Walk(const Str& dir, const Str& rel, Vec<FI>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*")), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Str n = FromWide(fd.cFileName), r = rel.empty() ? n : rel + "/" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Walk(JoinPath(dir, n), r, out);
        else { FI f; f.rel = r; f.sha = HashFile(JoinPath(dir, n), f.size); out.push_back(f); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

const FI* FindFi(const Vec<FI>& v, const Str& rel) { for (const FI& f : v) if (f.rel == rel) return &f; return nullptr; }
bool Excluded(const Str& rel) { return rel.ends_with(".vswp") || rel.ends_with(".log") || rel.find(".lck") >= 0 || rel.ends_with(".vmx") || rel.ends_with(".vmsd"); }

int CountDirs(const Env& e, const char* ds)
{
    int n = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(JoinPath(e.root, ds), "*")), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") && fd.cFileName[0] != L'.') ++n; } while (FindNextFileW(h, &fd));
    FindClose(h); return n;
}
int TotalDirs(const Env& e) { return CountDirs(e, "datastore1") + CountDirs(e, "datastore2"); }

// ---- a VM on the mock and its service-level backup ----------------------------------------------------------------------------
struct Fx { VmSpec spec; MockVm m; Vec<FI> before; };

bool MakeFx(Ctx& c, Ctl& ctl, const Env& e, Fx& f, const char* tag, int maxDisks, int maxSize, int minSize, const char* power)
{
    f.spec.name = UniqueName(c, tag); f.spec.disks = Disks(c, c.range(1, maxDisks), maxSize, minSize); f.spec.power = power;
    if (e.vcenter()) f.spec.dc = e.dcs[c.rnd((u32)e.dcs.size())];
    f.m = MakeVm(ctl, e, f.spec);
    if (!f.m.ok) return false;
    Walk(f.m.dir, "", f.before);
    return true;
}

// every file of the original (but swap, logs, .vmx, .vmsd) is in the restored VM folder byte for byte, and the folder has a .vmx
bool CheckRestored(Ctx& c, const Env& e, const Fx& f, const Str& newName, const Str& ds)
{
    bool ok = true;
    Vec<FI> after; Walk(JoinPath(JoinPath(e.root, ds), newName), "", after);
    for (const FI& b : f.before) {
        if (Excluded(b.rel)) continue;
        const FI* a = FindFi(after, b.rel);
        if (!a) { c.note("not restored: %s", b.rel.c_str()); ok = false; continue; }
        if (a->sha != b.sha || a->size != b.size) { c.note("restored file differs: %s", b.rel.c_str()); ok = false; }
    }
    bool vmx = false; for (const FI& a : after) if (a.rel.ends_with(".vmx")) vmx = true;
    if (!vmx) { c.note("no .vmx in the restored VM folder"); ok = false; }
    return ok;
}

// ---- the service side ---------------------------------------------------------------------------------------------------------------
Json Obj() { return Json::object(); }

Json JobRow(Svc& s, const Str& id)
{
    Json jobs = s.ok("jobs.list", Obj());
    for (size_t i = 0; i < jobs.size(); ++i) if (jobs.at(i).get("id").str() == id) return jobs.at(i);
    return Json();
}

// the history record of a job (waits for it: the job's status changes a moment before the record is written)
bool HistoryOf(Svc& s, const Str& jobId, Json& rec, int ms = 30000)
{
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        Json a = Obj(); a["limit"] = 1000; Json hl = s.ok("history.list", a);
        for (size_t i = 0; i < hl.size(); ++i) if (hl.at(i).get("id").str() == jobId) { Json g = Obj(); g["id"] = jobId; rec = s.ok("history.get", g); return true; }
        Sleep(30);
    }
    return false;
}

// starts an operation job (verify / restore / restore_esxi); finish = true waits for it
bool StartOp(Svc& s, const char* cmd, const Json& a, Str& jobId)
{
    Json r; if (!s.call(cmd, a, &r)) return false;
    jobId = r.get("job").str(); return !jobId.empty();
}

Str FirstResultMessage(const Json& rec) { return rec.get("results").at(0).get("message").str(); }

Json RestoreArgs(const Str& dir, const Str& archive, const Str& pw, const Str& esxiServer, const Json& options)
{
    Json a = Obj(); a["server"] = "local"; a["directory"] = dir; a["archive"] = archive; if (!pw.empty()) a["password"] = pw;     // the keys of the dialog (dlg_backup.cpp)
    a["esxi_server"] = esxiServer; a["options"] = options; return a;
}

Json GuiOptions(const Str& name, const Str& dc, const Str& ds, const Str& folder, const Str& pool, const Str& host, bool powerOn)
{
    Json op = Obj(); op["vm_name"] = name; op["datacenter"] = dc; op["datastore"] = ds; op["folder"] = folder; op["pool"] = pool; op["host"] = host; op["power_on"] = powerOn;
    return op;
}

bool HasBytes(const Bytes& b, const Str& needle)
{
    if (needle.empty() || b.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= b.size(); ++i) if (b[i] == (u8)needle[0] && memcmp(b.data() + i, needle.data(), needle.size()) == 0) return true;
    return false;
}

// searches every file below `dir` for the secrets; returns the first path that holds one ("" = none)
Str FindSecretInTree(const Str& dir, const Vec<Str>& secrets)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*")), &fd);
    if (h == INVALID_HANDLE_VALUE) return Str();
    Str hit;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Str p = JoinPath(dir, FromWide(fd.cFileName));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { hit = FindSecretInTree(p, secrets); if (!hit.empty()) break; continue; }
        Bytes b; Str e; if (!ReadWholeFile(p.c_str(), b, e, (size_t)64 << 20)) continue;
        for (const Str& s : secrets) if (HasBytes(b, s)) { hit = p + " (" + s.substr(0, 6) + "...)"; break; }
        if (!hit.empty()) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return hit;
}

Str AsciiPrefix(const Str& s) { Str r; for (size_t i = 0; i < s.size() && (u8)s[i] < 0x80; ++i) r += s[i]; return r; }     // the mock password is "pw-<n>-<non-ASCII>": JSON may escape the tail

const wchar_t kLogName[] = L"vm2b-svctest";
Str LogPath() { return JoinPath(JoinPath(ServiceCore::DefaultDataDir(), "logs"), FromWide(kLogName) + ".log"); }
void OpenLogOnce() { static bool done = false; if (!done) { done = true; AppLogOpen(kLogName); } }      // the service writes its log through Log(): the autotest process opens one

} // namespace

// ============================================================================================================================
// ESXi / vCenter through the service
// ============================================================================================================================
VM2B_TEST("svcesxi", esxi_through_service_all_profiles)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    OpenLogOnce();
    Svc s(c_, "svcx1"); if (!s.up) return false;
    s.record = true;
    Str archPw = Str::fmt("arch-pw-%u", c_.rnd(1000000));
    Vec<Str> secrets; secrets.push_back(AsciiPrefix(all[0].password)); secrets.push_back(archPw);
    T_CHECK(secrets[0].size() >= 5);
    Str store = s.tmp.sub("store");
    Str dirSrv; { Str rootDir = s.tmp.sub("plainvms"); WriteBytes(JoinPath(rootDir, "pvm\\a.bin"), Random(c_, 1000)); dirSrv = s.addDirServer(rootDir); }
    int round = c_.range(0, 5), histRows = 0;
    for (const Env& e : all) {
        ++round; int variant = round % 4;
        Ctl ctl(e); ctl.reset();
        c_.note("%s (variant %d)", e.name.c_str(), variant);
        // 1. servers.save: the password is protected, never comes back; the wizard's "mask" keeps it
        Json sa = Obj(); sa["server"] = e.record(); Json saved = s.ok("servers.save", sa);
        Str srv = saved.get("id").str(); T_CHECK(!srv.empty()); T_CHECK(saved.get("has_password").boolean()); T_CHECK(!saved.has("password"));
        T_CHECK(saved.get("type").str() == e.kind && saved.get("host").str() == e.host && saved.get("fingerprint").str() == e.fp);
        Json lst = s.ok("servers.list", Obj()); T_CHECK(lst.dump().find(secrets[0].c_str()) < 0);
        { Json rv; Json cfgBytes; (void)cfgBytes; Json revealed; T_CHECK(s.core.config().server(srv, true, revealed)); T_CHECK(revealed.get("password").str() == e.password); }
        // 2. the VM and servers.vms / servers.test (by id, and as the wizard sends an unsaved server with the mask)
        Fx f; T_CHECK(MakeFx(c_, ctl, e, f, "SV", 2, 300000, 20000, variant % 2 ? "poweredOn" : "poweredOff")); if (!f.m.ok) return false;
        Json va = Obj(); va["id"] = srv; Json vl = s.ok("servers.vms", va);
        bool seen = false; for (size_t i = 0; i < vl.get("vms").size(); ++i) if (vl.get("vms").at(i).get("name").str() == f.spec.name) { seen = true; T_CHECK(vl.get("vms").at(i).get("state").str() == (f.spec.power == "poweredOn" ? "running" : "stopped")); }
        T_CHECK(seen);
        { Json wz = Obj(); Json rec = saved; rec["password"] = kMask; wz["server"] = rec; Json r2 = s.ok("servers.vms", wz); T_CHECK(r2.get("vms").size() == vl.get("vms").size()); }
        { Json r3 = s.ok("servers.test", va); T_CHECK(r3.get("vms").size() == vl.get("vms").size()); T_CHECK(r3.get("message").str().find("virtual machine") >= 0); }
        { Json bad = e.record(); bad["name"] = "bad pw " + e.name; bad["password"] = "wrong-" + e.password; Json ba = Obj(); ba["server"] = bad; Json r4;
          T_CHECK(!s.call("servers.vms", ba, &r4)); T_CHECK(s.lastErr.find("login failed") >= 0); }
        // 3. a task with an ESXi source, the snapshot options and the datacenter key as the GUI writes them (dlg_backup.cpp onOk)
        bool quiesce = c_.coin(), memory = e.vcenter() ? c_.coin(30) : c_.coin(30);
        Json el = Svc::Element(srv, f.spec.name.c_str(), JoinPath(store, "{VM}"), "xz", c_.range(1, 3), c_.coin(60) ? archPw.c_str() : nullptr);
        bool encrypted = el.get("encrypt").get("enabled").boolean();
        { Json sn = Obj(); sn["quiesce"] = quiesce; sn["memory"] = memory; el["snapshot"] = sn; }
        if (e.vcenter() && c_.coin(70)) el["source"]["datacenter"] = f.spec.dc;
        el["allow_live"] = true; el["resume"] = true; el["speed_mbit"] = 0; el["on_error_next"] = true; el["segment_mib"] = 1;
        Str taskName = "ESXi " + e.name;
        Str taskId = s.saveTask(taskName.c_str(), el); T_CHECK(!taskId.empty());
        Json tl = s.ok("tasks.list", Obj()); T_CHECK(tl.dump().find(archPw.c_str()) < 0);
        { Json t = Obj(); for (size_t i = 0; i < tl.size(); ++i) if (tl.at(i).get("id").str() == taskId) t = tl.at(i);
          T_CHECK(t.get("elements").at(0).get("snapshot").get("quiesce").boolean() == quiesce && t.get("elements").at(0).get("snapshot").get("memory").boolean() == memory); }
        Json ra = Obj(); ra["id"] = taskName; Json run = s.ok("tasks.run", ra);
        Str jid = run.get("job").str(); T_CHECK(!jid.empty());
        Str fin = s.waitJob(jid, 90000); T_CHECK(fin == "Success" || fin == "Gone");
        if (fin != "Success" && fin != "Gone") c_.note("backup job ended as '%s'", fin.c_str());
        Json rec; T_CHECK(HistoryOf(s, jid, rec)); ++histRows; if (c_.failed) return false;
        T_CHECK(rec.get("kind").str() == "task" && rec.get("status").str() == "Success");
        T_CHECK(rec.get("results").size() == 1 && rec.get("results").at(0).get("vm").str() == f.spec.name && rec.get("results").at(0).get("raw").integer() > 20000);
        Str archive = rec.get("results").at(0).get("archive").str(); T_CHECK(!archive.empty());
        T_CHECK(rec.get("results").at(0).get("encrypted").boolean() == encrypted);
        T_CHECK(ctl.leaked() == 0); T_CHECK(f.spec.power != "poweredOn" || (ctl.events("create") == 1 && ctl.events("remove") == 1));
        if (f.spec.power == "poweredOn") {                                       // the snapshot request carried exactly the task's options
            Json lg; ctl.call("log", Json(), lg); bool ok = false;
            for (size_t i = 0; i < lg.get("log").size(); ++i) { const Json& l = lg.get("log").at(i); if (l.get("what").str() == "CreateSnapshot") { Json x; Str pe; Json::parse(l.get("extra").str(), x, pe); ok = x.get("memory").boolean() == memory && x.get("quiesce").boolean() == quiesce; } }
            T_CHECK(ok);
        }
        // 4. Backup Explorer: list and verify
        Json qa = Obj(); qa["server"] = "local"; Json bl = s.ok("backups.list", qa);
        Json bk; for (size_t i = 0; i < bl.get("backups").size(); ++i) if (bl.get("backups").at(i).get("archive").str() == archive) bk = bl.get("backups").at(i);
        T_CHECK(!bk.is_null()); T_CHECK(bk.get("vm").str() == f.spec.name); T_CHECK(bk.get("encrypted").boolean() == encrypted);
        Str bdir = bk.get("directory").str();
        Str pw = encrypted ? archPw : Str();
        { Json v = Obj(); v["server"] = "local"; v["directory"] = bdir; v["archive"] = archive; if (encrypted) v["password"] = pw;
          Str vj; T_CHECK(StartOp(s, "backups.verify", v, vj)); Str vs = s.waitJob(vj); T_CHECK(vs == "Success" || vs == "Gone"); Json vr; T_CHECK(HistoryOf(s, vj, vr)); ++histRows; T_CHECK(vr.get("kind").str() == "verify" && vr.get("status").str() == "Success");
          if (encrypted) { Json w = v; w["password"] = "nope"; Str wj; T_CHECK(StartOp(s, "backups.verify", w, wj)); s.waitJob(wj); Json wr; T_CHECK(HistoryOf(s, wj, wr)); ++histRows; T_CHECK(wr.get("status").str() == "Failed"); } }
        // 5. esxi.restore_options: what the restore dialog loads
        Json oa = Obj(); oa["server"] = srv; if (e.vcenter()) oa["datacenter"] = f.spec.dc; Json opts = s.ok("esxi.restore_options", oa);
        T_CHECK(opts.get("kind").str() == e.kind); T_CHECK(opts.get("datastores").size() >= 2); T_CHECK(opts.get("pools").size() >= 2); T_CHECK(opts.get("hosts").size() >= 1);
        T_CHECK(opts.get("datacenter").str() == (e.vcenter() ? f.spec.dc : Str("ha-datacenter")));
        { bool vmListed = false; for (size_t i = 0; i < opts.get("vms").size(); ++i) if (opts.get("vms").at(i).str() == f.spec.name) vmListed = true; T_CHECK(vmListed); }
        { Json bo = Obj(); bo["server"] = "no-such-server"; T_CHECK(!s.call("esxi.restore_options", bo)); T_CHECK(s.lastErr.find("not found") >= 0);
          Json bo2 = Obj(); bo2["server"] = srv; if (e.vcenter()) bo2["datacenter"] = "NoSuchDC"; if (e.vcenter()) { T_CHECK(!s.call("esxi.restore_options", bo2)); T_CHECK(!s.lastErr.empty()); } }
        Str dc = opts.get("datacenter").str(), poolId, folderId, hostId;
        for (size_t i = 0; i < opts.get("pools").size(); ++i) if (opts.get("pools").at(i).get("path").str().ends_with("/Resources/Production")) poolId = opts.get("pools").at(i).get("id").str();
        for (size_t i = 0; i < opts.get("folders").size(); ++i) if (opts.get("folders").at(i).get("path").str() == "Servers/Linux") folderId = opts.get("folders").at(i).get("id").str();
        if (opts.get("hosts").size()) hostId = opts.get("hosts").at(0).get("id").str();
        // 6. restore to the host: the options of the dialog, in the four ways the dialog's choices can combine
        Str newName = f.spec.name + " restored"; Str ds = "datastore1"; bool powerOn = false; Str useFolder, usePool, useHost;
        if (variant == 1) { ds = "datastore2"; powerOn = true; }
        if (variant == 2) { usePool = poolId; if (e.vcenter()) { useFolder = folderId; useHost = hostId; } }
        if (variant == 3) newName = "R" + Str::fmt("%u", c_.rnd(100000)) + " " + kNames[c_.rnd(7)];
        int regBefore = ctl.counter("reg:RegisterVM"), dirs0 = TotalDirs(e);
        Json op = GuiOptions(newName, dc, variant == 1 ? ds : Str(), useFolder, usePool, useHost, powerOn);
        Json rjob; Str rj; T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, pw, srv, op), rj));
        Str rs = s.waitJob(rj, 90000); T_CHECK(rs == "Success" || rs == "Gone");
        { Json row = JobRow(s, rj); if (!row.is_null()) T_CHECK(row.get("kind").str() == "restore_esxi"); }
        Json rr; T_CHECK(HistoryOf(s, rj, rr)); ++histRows; if (c_.failed) return false;
        T_CHECK(rr.get("kind").str() == "restore_esxi"); if (rr.get("status").str() != "Success") c_.note("restore: %s %s", rr.get("status").str().c_str(), FirstResultMessage(rr).c_str());
        T_CHECK(rr.get("status").str() == "Success");
        T_CHECK(rr.get("results").at(0).get("restored_vm").str() == newName);
        { Str vmx = rr.get("results").at(0).get("vmx").str(); T_CHECK(vmx.ends_with(".vmx")); T_CHECK(vmx.find(newName.c_str()) >= 0); T_CHECK(vmx.find(ds.c_str()) >= 0); }
        T_CHECK(CheckRestored(c_, e, f, newName, ds));
        { Json v = ctl.vmState(newName); T_CHECK(!v.is_null()); T_CHECK(v.get("ds").str() == ds); T_CHECK(v.get("snapshots").size() == 0); T_CHECK(v.get("power").str() == (powerOn ? "poweredOn" : "poweredOff")); }
        T_CHECK(ctl.counter("reg:RegisterVM") == regBefore + 1); T_CHECK(ctl.leaked() == 0);
        // 7. refusals: nothing is registered, nothing is left on the datastores, the job says why
        int regAfter = ctl.counter("reg:RegisterVM"); dirs0 = TotalDirs(e);
        struct Case { const char* what; Json args; const char* need; };
        Vec<Case> cases;
        { Case k; k.what = "existing VM name"; k.args = RestoreArgs(bdir, archive, pw, srv, GuiOptions(f.spec.name, dc, "", "", "", "", false)); k.need = "already exist"; cases.push_back(k); }
        { Case k; k.what = "the name of the restored VM"; k.args = RestoreArgs(bdir, archive, pw, srv, GuiOptions(newName, dc, "", "", "", "", false)); k.need = "already exist"; cases.push_back(k); }
        { Case k; k.what = "wrong esxi_server id"; k.args = RestoreArgs(bdir, archive, pw, "no-such-id", GuiOptions("zz" + Str::fmt("%u", c_.rnd(1000)), dc, "", "", "", "", false)); k.need = "not found"; cases.push_back(k); }
        { Case k; k.what = "a directory server as the ESXi server"; k.args = RestoreArgs(bdir, archive, pw, dirSrv, GuiOptions("yy" + Str::fmt("%u", c_.rnd(1000)), dc, "", "", "", "", false)); k.need = ""; cases.push_back(k); }
        { Case k; k.what = "bad datastore"; k.args = RestoreArgs(bdir, archive, pw, srv, GuiOptions("xx" + Str::fmt("%u", c_.rnd(1000)), dc, "no-such-datastore", "", "", "", false)); k.need = "datastore"; cases.push_back(k); }
        if (e.vcenter()) { Case k; k.what = "bad folder"; k.args = RestoreArgs(bdir, archive, pw, srv, GuiOptions("ww" + Str::fmt("%u", c_.rnd(1000)), dc, "", "No/Such/Folder", "", "", false)); k.need = "folder"; cases.push_back(k); }
        if (encrypted) { Case k; k.what = "wrong archive password"; k.args = RestoreArgs(bdir, archive, "wrong", srv, GuiOptions("vv" + Str::fmt("%u", c_.rnd(1000)), dc, "", "", "", "", false)); k.need = "wrong password"; cases.push_back(k); }
        for (const Case& k : cases) {
            Str j; if (!StartOp(s, "backups.restore_esxi", k.args, j)) { c_.note("%s: refused at the call: %s", k.what, s.lastErr.c_str()); T_CHECK(false); continue; }
            Str st = s.waitJob(j, 60000); T_CHECK(st == "Failed" || st == "Gone");
            Json hr; T_CHECK(HistoryOf(s, j, hr)); ++histRows;
            T_CHECK(hr.get("status").str() == "Failed"); Str msg = FirstResultMessage(hr);
            if (hr.get("status").str() != "Failed" || (k.need[0] && msg.find(k.need) < 0)) c_.note("%s: status %s, message '%s'", k.what, hr.get("status").str().c_str(), msg.c_str());
            T_CHECK(!msg.empty()); T_CHECK(k.need[0] == 0 || msg.find(k.need) >= 0);
            T_CHECK(msg.find(secrets[0].c_str()) < 0 && msg.find(archPw.c_str()) < 0);
        }
        { Json none = Obj(); none["server"] = "local"; none["directory"] = bdir; none["archive"] = archive; none["options"] = GuiOptions("q", dc, "", "", "", "", false); T_CHECK(!s.call("backups.restore_esxi", none)); T_CHECK(s.lastErr.find("choose") >= 0); }   // no esxi_server at all
        T_CHECK(ctl.counter("reg:RegisterVM") == regAfter); T_CHECK(TotalDirs(e) == dirs0);
        if (TotalDirs(e) != dirs0) c_.note("%d folders, %d before", TotalDirs(e), dirs0);
        T_CHECK(ctl.leaked() == 0);
        ctl.deleteVm(f.spec.name); ctl.deleteVm(newName);
        // the save of the server record does not disturb the saved task, and a server in use cannot be deleted
        Json del = Obj(); del["id"] = srv; T_CHECK(!s.call("servers.delete", del)); T_CHECK(s.lastErr.find("used by") >= 0);
        if (c_.failed) return false;
    }
    // 8. the history holds one row per job; nothing in it, in any answer, in the data folder or in the log is a secret
    Json hl = s.ok("history.list", [] { Json a = Obj(); a["limit"] = 1000; return a; }());
    T_CHECK((int)hl.size() == histRows); if ((int)hl.size() != histRows) c_.note("history has %d rows, %d jobs ran", (int)hl.size(), histRows);
    for (size_t i = 0; i < secrets.size(); ++i) T_CHECK(s.transcript.find(secrets[i].c_str()) < 0);
    T_CHECK(s.transcript.size() > 1000);
    Str hit = FindSecretInTree(s.opt.dataDir, secrets); if (!hit.empty()) c_.note("secret found in %s", hit.c_str()); T_CHECK(hit.empty());
    { Bytes lb; Str le; if (ReadWholeFile(LogPath().c_str(), lb, le)) { for (const Str& x : secrets) T_CHECK(!HasBytes(lb, x)); T_CHECK(lb.size() > 20); } else c_.note("the service log %s could not be read", LogPath().c_str()); }
    Bytes cfg; Str ce; T_CHECK(ReadWholeFile(JoinPath(s.opt.dataDir, "config.json").c_str(), cfg, ce)); T_CHECK(HasBytes(cfg, "dpapi:"));
    return true;
}

VM2B_TEST("svcesxi", restore_esxi_speed_limit_setting)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    Vec<Env> es; for (const Env& e : all) if (e.tlsOnly.empty() && e.cert.empty()) es.push_back(e);
    const Env& e = es[c_.rnd((u32)es.size())];
    Svc s(c_, "svcx2"); if (!s.up) return false;
    Ctl ctl(e); ctl.reset();
    Fx f; T_CHECK(MakeFx(c_, ctl, e, f, "SL", 3, 400000, 150000, "poweredOff")); if (!f.m.ok) return false;
    Json sa = Obj(); sa["server"] = e.record(); Str srv = s.ok("servers.save", sa).get("id").str();
    Str taskId = s.saveTask("Limit", [&] { Json el = Svc::Element(srv, f.spec.name.c_str(), JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1); el["verify"] = false; return el; }());
    Json ra = Obj(); ra["id"] = "Limit"; Str jid = s.ok("tasks.run", ra).get("job").str(); Str fin = s.waitJob(jid, 60000); T_CHECK(fin == "Success" || fin == "Gone");
    Json rec; T_CHECK(HistoryOf(s, jid, rec)); if (c_.failed) return false;
    Json r0 = rec.get("results").at(0); Str archive = r0.get("archive").str(); i64 raw = r0.get("raw").integer(); T_CHECK(raw > 100000);
    Json bl = s.ok("backups.list", [] { Json a = Obj(); a["server"] = "local"; return a; }()); T_CHECK(bl.get("backups").size() == 1); Str bdir = bl.get("backups").at(0).get("directory").str();
    Str dc = e.vcenter() ? f.spec.dc : Str("ha-datacenter");
    // the limit counts the bytes that go to the host: about 3 s for this VM
    double mbit = (double)raw * 8.0 / 1e6 / 3.0; if (mbit < 0.05) mbit = 0.05;
    Json lim = Obj(); lim["speed_limit_mbit"] = mbit; s.ok("settings.set", lim);
    T_CHECK(s.ok("settings.get", Obj()).get("speed_limit_mbit").num() > mbit * 0.99);
    Str n1 = f.spec.name + " limited"; DWORD t0 = GetTickCount(); Str j1;
    T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, GuiOptions(n1, dc, "", "", "", "", false)), j1));
    double seen = 0; for (int i = 0; i < 100 && seen == 0; ++i) { Json row = JobRow(s, j1); if (!row.is_null() && row.get("limit_mbit").num() > 0) seen = row.get("limit_mbit").num(); else Sleep(20); }
    T_CHECK(seen > mbit * 0.95 && seen < mbit * 1.05);
    Str st = s.waitJob(j1, 90000); T_CHECK(st == "Success" || st == "Gone"); double sec1 = (GetTickCount() - t0) / 1000.0;
    Json r1; T_CHECK(HistoryOf(s, j1, r1)); T_CHECK(r1.get("status").str() == "Success"); T_CHECK(CheckRestored(c_, e, f, n1, "datastore1"));
    c_.note("%s: %lld raw bytes at %.2f Mbit/s took %.1f s", e.name.c_str(), (long long)raw, mbit, sec1);
    T_CHECK(sec1 >= 1.8);                                              // without the limit the mock takes well under a second
    // (the service gives the SAME limiter to the archive reads and to the upload, so the two legs share the limit: about twice the time for an uncompressed archive on a local target)
    T_CHECK(sec1 < 30.0);
    // the limit removed: the next restore is clearly faster
    Json off = Obj(); off["speed_limit_mbit"] = 0; s.ok("settings.set", off);
    Str n2 = f.spec.name + " free"; t0 = GetTickCount(); Str j2;
    T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, GuiOptions(n2, dc, "", "", "", "", false)), j2));
    st = s.waitJob(j2, 90000); T_CHECK(st == "Success" || st == "Gone"); double sec2 = (GetTickCount() - t0) / 1000.0;
    c_.note("without the limit: %.1f s", sec2); T_CHECK(sec2 < sec1);
    Json r2; T_CHECK(HistoryOf(s, j2, r2)); T_CHECK(r2.get("status").str() == "Success");
    // the same limit set while a restore runs is followed by the running job (the scheduler tick re-reads it)
    Json slow = Obj(); slow["speed_limit_mbit"] = mbit; s.ok("settings.set", slow);
    Str n3 = f.spec.name + " retuned"; Str j3; T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, GuiOptions(n3, dc, "", "", "", "", false)), j3));
    Sleep(300); Json off2 = Obj(); off2["speed_limit_mbit"] = 0; s.ok("settings.set", off2);
    bool freed = false; for (int i = 0; i < 100 && !freed; ++i) { Json row = JobRow(s, j3); freed = row.is_null() || row.get("limit_mbit").num() == 0; if (!freed) Sleep(20); }
    T_CHECK(freed);
    st = s.waitJob(j3, 90000); T_CHECK(st == "Success" || st == "Gone");
    ctl.deleteVm(f.spec.name); ctl.deleteVm(n1); ctl.deleteVm(n2); ctl.deleteVm(n3);
    T_CHECK(ctl.leaked() == 0);
    return true;
}

VM2B_TEST("svcesxi", restore_esxi_abort_and_service_restart)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    Vec<Env> es; for (const Env& e : all) if (e.tlsOnly.empty() && e.cert.empty()) es.push_back(e);
    const Env& e = es[c_.rnd((u32)es.size())];
    Svc s(c_, "svcx3"); if (!s.up) return false;
    Ctl ctl(e); ctl.reset();
    Fx f; T_CHECK(MakeFx(c_, ctl, e, f, "RT", 4, 400000, 300000, "poweredOff")); if (!f.m.ok) return false;
    Json sa = Obj(); sa["server"] = e.record(); Str srv = s.ok("servers.save", sa).get("id").str();
    s.saveTask("Src", [&] { Json el = Svc::Element(srv, f.spec.name.c_str(), JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1); el["verify"] = false; return el; }());
    Json ra = Obj(); ra["id"] = "Src"; Str jid = s.ok("tasks.run", ra).get("job").str(); Str fin = s.waitJob(jid, 60000); T_CHECK(fin == "Success" || fin == "Gone");
    Json rec; T_CHECK(HistoryOf(s, jid, rec)); if (c_.failed) return false;
    Str archive = rec.get("results").at(0).get("archive").str(); i64 raw = rec.get("results").at(0).get("raw").integer();
    Json bl = s.ok("backups.list", [] { Json a = Obj(); a["server"] = "local"; return a; }()); Str bdir = bl.get("backups").at(0).get("directory").str();
    Str dc = e.vcenter() ? f.spec.dc : Str("ha-datacenter");
    int histBase = s.historyCount(), dirs0 = TotalDirs(e);
    double mbit = (double)raw * 8.0 / 1e6 / 7.0; if (mbit < 0.05) mbit = 0.05;                    // the upload would take about 7 s
    Json lim = Obj(); lim["speed_limit_mbit"] = mbit; s.ok("settings.set", lim);
    Str stateDir = JoinPath(s.opt.dataDir, "restore-state");

    // ---- a. cancel (jobs.abort): the job ends "Abort", the host keeps nothing, no state is left
    Str nameA = f.spec.name + " cancelled"; Str ja; T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, GuiOptions(nameA, dc, "", "", "", "", false)), ja));
    Sleep(1200);
    T_CHECK(JobRow(s, ja).get("status").str() == "Running");
    { Json ab = Obj(); ab["id"] = ja; s.ok("jobs.abort", ab); }
    Str sa1 = s.waitJob(ja, 60000); T_CHECK(sa1 == "Abort" || sa1 == "Gone"); if (sa1 != "Abort" && sa1 != "Gone") c_.note("abort ended as '%s'", sa1.c_str());
    Json ar; T_CHECK(HistoryOf(s, ja, ar)); T_CHECK(ar.get("status").str() == "Abort");
    T_CHECK(ctl.vmState(nameA).is_null()); T_CHECK(TotalDirs(e) == dirs0); if (TotalDirs(e) != dirs0) c_.note("after the cancel: %d folders, %d before", TotalDirs(e), dirs0);
    { Vec<FI> st; Walk(stateDir, "", st); T_CHECK(st.empty()); }
    int reg0 = ctl.counter("reg:RegisterVM"); histBase = s.historyCount();

    // ---- b. the service restarts in the middle of a restore: the job pauses (no history row, nothing registered, the uploaded files stay)
    Str nameB = f.spec.name + " interrupted"; Json opB = GuiOptions(nameB, dc, "", "", "", "", false);
    Str jb; T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, opB), jb));
    Sleep(2500);
    T_CHECK(JobRow(s, jb).get("status").str() == "Running");
    int dirsDuring = TotalDirs(e);
    T_CHECK(s.restart()); if (c_.failed) return false;
    T_CHECK(ctl.vmState(nameB).is_null()); T_CHECK(ctl.counter("reg:RegisterVM") == reg0);                      // never registered half-way
    T_CHECK(s.historyCount() == histBase);                                                                       // a pause is not a result
    T_CHECK(s.ok("jobs.list", Obj()).size() == 0);                                                               // a restore is not a task: the new service does not run it again by itself
    int dirsPaused = TotalDirs(e); c_.note("folders: %d before, %d during the upload, %d after the restart", dirs0, dirsDuring, dirsPaused);
    T_CHECK(dirsPaused >= dirs0);
    // regression: the service makes the state folder itself (it once did not: the state file could not be written, the half-uploaded folder blocked every later try)
    if (dirsPaused > dirs0) { Vec<FI> st; Walk(stateDir, "", st); if (st.size() != 1) c_.note("%d files in %s after the restart", (int)st.size(), stateDir.c_str()); T_CHECK(st.size() == 1 && st[0].rel.starts_with("vm2b-restore-esxi-")); }
    Sleep(800); T_CHECK(ctl.vmState(nameB).is_null()); T_CHECK(s.ok("jobs.list", Obj()).size() == 0);
    // the user starts the same restore again (the settings, including the limit, survived the restart); it continues and registers the VM exactly once
    Json ls = s.ok("settings.get", Obj()); T_CHECK(ls.get("speed_limit_mbit").num() > mbit * 0.99);
    Json off = Obj(); off["speed_limit_mbit"] = 0; s.ok("settings.set", off);
    Str jb2; T_CHECK(StartOp(s, "backups.restore_esxi", RestoreArgs(bdir, archive, "", srv, opB), jb2));
    Str sb = s.waitJob(jb2, 90000); T_CHECK(sb == "Success" || sb == "Gone");
    Json br; T_CHECK(HistoryOf(s, jb2, br));
    if (c_.failed) { c_.note("second run: %s '%s' / %s", sb.c_str(), br.get("status").str().c_str(), FirstResultMessage(br).c_str()); return false; }
    if (br.get("status").str() != "Success") c_.note("second run: %s %s", br.get("status").str().c_str(), FirstResultMessage(br).c_str());
    T_CHECK(br.get("kind").str() == "restore_esxi" && br.get("status").str() == "Success"); T_CHECK(br.get("results").at(0).get("restored_vm").str() == nameB);
    { Json row = JobRow(s, jb2); if (dirsPaused > dirs0 && !row.is_null()) T_CHECK(row.get("message").str().starts_with("continuing")); }       // it continued from the journal
    T_CHECK(ctl.counter("reg:RegisterVM") == reg0 + 1);
    { Json st = ctl.state(); int n = 0; const Json& vms = st.get("vms"); for (size_t i = 0; i < vms.size(); ++i) if (vms.key_at(i) == nameB) ++n; T_CHECK(n == 1); }
    T_CHECK(CheckRestored(c_, e, f, nameB, "datastore1"));
    T_CHECK(TotalDirs(e) == dirs0 + 1); T_CHECK(s.historyCount() == histBase + 1);
    { Vec<FI> st; Walk(stateDir, "", st); for (const FI& q : st) c_.note("left in the state dir: %s", q.rel.c_str()); T_CHECK(st.empty()); }
    T_CHECK(ctl.leaked() == 0);
    ctl.deleteVm(f.spec.name); ctl.deleteVm(nameB);
    return true;
}

VM2B_TEST("svcesxi", esxi_task_survives_host_power_loss_and_service_restart)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    Vec<Env> es; for (const Env& e : all) if (e.tlsOnly.empty() && e.cert.empty()) es.push_back(e);
    const Env& e = es[c_.rnd((u32)es.size())];
    Svc s(c_, "svcx4"); if (!s.up) return false;
    Ctl ctl(e); ctl.reset();
    Fx f; T_CHECK(MakeFx(c_, ctl, e, f, "PW", 3, 400000, 200000, c_.coin() ? "poweredOn" : "poweredOff")); if (!f.m.ok) return false;
    Json sa = Obj(); sa["server"] = e.record(); Str srv = s.ok("servers.save", sa).get("id").str();
    s.saveTask("Pw", [&] { Json el = Svc::Element(srv, f.spec.name.c_str(), JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1); el["verify"] = false; el["resume"] = true; el["segment_mib"] = 1; return el; }());
    int rounds = c_.range(2, 3);
    for (int round = 0; round < rounds; ++round) {
        int n = c_.range(1, 70);
        { Json fl = Obj(); fl["die_after"] = n; Json o; Ctl* cp = &ctl; cp->call("faults", fl, o); }
        Json ra = Obj(); ra["id"] = "Pw"; Str jid = s.ok("tasks.run", ra).get("job").str(); T_CHECK(!jid.empty());
        // wait for the outage (or the end of the job when the budget was larger than the whole run)
        bool dead = false; DWORD t0 = GetTickCount(); Str fin;
        while ((int)(GetTickCount() - t0) < 60000) {
            if (ctl.state().get("dead").boolean()) { dead = true; break; }
            Json row = JobRow(s, jid); if (row.is_null() || (row.get("status").str() != "Running" && row.get("status").str() != "Waiting")) break;
            Sleep(20);
        }
        c_.note("%s round %d: power lost after %d events: %s", e.name.c_str(), round, n, dead ? "yes" : "no (job finished first)");
        if (dead) { Sleep(c_.range(0, 400)); T_CHECK(s.restart()); if (c_.failed) return false; }       // the service dies with the host (no clean end of the job on the host side)
        else { fin = s.waitJob(jid, 60000); T_CHECK(fin == "Success" || fin == "Warning" || fin == "Gone"); }
        Json o; ctl.call("revive", Obj(), o);
    }
    // power is back for good: the next run must succeed, whatever the earlier runs left (snapshots, half-written volumes)
    ctl.reset();
    Json ra = Obj(); ra["id"] = "Pw"; Str jid = s.ok("tasks.run", ra).get("job").str(); T_CHECK(!jid.empty());
    Str fin = s.waitJob(jid, 120000); if (fin != "Success" && fin != "Warning" && fin != "Gone") { Json row = JobRow(s, jid); c_.note("final job: '%s' %s", fin.c_str(), row.dump().c_str()); } T_CHECK(fin == "Success" || fin == "Warning" || fin == "Gone");
    Json rec; T_CHECK(HistoryOf(s, jid, rec)); if (c_.failed) return false;
    if (rec.get("status").str() != "Success" && rec.get("status").str() != "Warning") c_.note("final run: %s %s", rec.get("status").str().c_str(), FirstResultMessage(rec).c_str());
    T_CHECK(rec.get("status").str() == "Success" || rec.get("status").str() == "Warning");
    T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.state().get("vms").get(f.spec.name.c_str()).get("snapshots").size() == 0);
    ctl.deleteVm(f.spec.name);
    return true;
}

// ============================================================================================================================
// the web site hosted by the service
// ============================================================================================================================
namespace {

bool TcpOpen(const char* ip, int port)
{
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET so = socket(AF_INET, SOCK_STREAM, 0); if (so == INVALID_SOCKET) return false;
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = inet_addr(ip); a.sin_port = htons((u_short)port);
    bool ok = connect(so, (sockaddr*)&a, sizeof(a)) == 0; closesocket(so); return ok;
}

bool WaitClosed(const char* ip, int port, int ms = 3000) { DWORD t0 = GetTickCount(); while ((int)(GetTickCount() - t0) < ms) { if (!TcpOpen(ip, port)) return true; Sleep(30); } return false; }
bool WaitOpen(const char* ip, int port, int ms = 5000) { DWORD t0 = GetTickCount(); while ((int)(GetTickCount() - t0) < ms) { if (TcpOpen(ip, port)) return true; Sleep(30); } return false; }

int FreePort(Ctx& c)
{
    for (int i = 0; i < 60; ++i) { int p = c.range(24000, 24299); if (!TcpOpen("127.0.0.1", p) && !TcpOpen("127.0.0.2", p)) return p; }
    return 24000;
}

// a browser-like client of the site: TLS pinned to the fingerprint, cookie jar (the session cookie), the CSRF token of the login
struct WebCli {
    HttpClient cl; Str csrf, err; Str role;
    WebCli(int port, const Str& fp) : cl(Opts(port, fp)) {}
    static HttpOptions Opts(int port, const Str& fp) {
        HttpOptions o; o.host = "127.0.0.1"; o.port = port; o.tls = true; o.tlsOpt.fingerprint = fp; o.tlsOpt.timeoutMs = 30000; return o;
    }
    // returns the HTTP status (0 = no answer); the body is parsed into `res` when it is JSON
    int send(const char* method, const Str& path, const Str& body, Json* res = nullptr, bool withCsrf = true, const char* origin = nullptr) {
        HttpHeaders h; if (Str(method) == "POST") h.add("Content-Type", "application/json");
        if (withCsrf && !csrf.empty()) h.add("X-CSRF-Token", csrf);
        if (origin) h.add("Origin", origin);
        HttpResponse r; Bytes b;
        if (!cl.requestBytes(method, path, h, body.empty() ? nullptr : body.data(), body.size(), r, b)) { err = cl.err; return 0; }
        if (res) { Str e; *res = Json(); Json::parse((const char*)b.data(), b.size(), *res, e); }
        return r.status;
    }
    int post(const char* cmd, const Json& a, Json* res = nullptr, bool withCsrf = true) { return send("POST", Str("/api/") + cmd, a.dump(), res, withCsrf); }
    // one attempt, no waiting (429 = the throttle after failed attempts of this address / user)
    int loginRaw(const char* user, const char* pw, const Str& code = Str(), const Str& recovery = Str(), Json* res = nullptr) {
        Json a = Obj(); a["user"] = user; a["password"] = pw; if (!code.empty()) a["code"] = code; if (!recovery.empty()) a["recovery"] = recovery;
        Json r; int st = post("login", a, &r, false);
        if (st == 200) { csrf = r.get("csrf").str(); role = r.get("role").str(); }
        if (res) *res = r;
        return st;
    }
    // waits out the throttle (a failed attempt blocks the address for 1, 2, 4 ... s) and returns the real answer
    int login(const char* user, const char* pw, const Str& code = Str(), const Str& recovery = Str(), Json* res = nullptr) {
        int st = 0;
        for (int i = 0; i < 6; ++i) { Json r; st = loginRaw(user, pw, code, recovery, &r); if (res) *res = r; if (st != 429) break; Sleep((DWORD)(r.get("retry_after").integer(1) * 1000 + 150)); }
        return st;
    }
};

Str StrongPw(const char* tag) { return Str("Zebra-Quartz-") + tag + "-77"; }

Json WebUser(const char* name, const char* role, const Str& pw, const char* twofa)
{
    Json u = Obj(); u["name"] = name; u["role"] = role; u["password"] = pw; u["twofa"] = twofa; return u;
}

Json Cmd(Svc& s, const char* cmd) { return s.ok(cmd, Obj()); }

Str Fingerprint(Svc& s) { return Cmd(s, "web.status").get("cert").get("fingerprint").str(); }

bool EnableSite(Svc& s, int port, bool require2fa)
{
    Json a = Obj(); a["enabled"] = true; a["port"] = port; Json b = Json::array(); b.push("127.0.0.1"); a["bind"] = b; a["require_2fa_admins"] = require2fa;
    Json r; return s.call("web.settings.set", a, &r);
}

int g_shutdowns = 0;
void CountShutdown() { ++g_shutdowns; }

} // namespace

VM2B_TEST("svcweb", web_commands_default_on_loopback_with_default_admin)
{
    SetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", nullptr);            // the product default: the autotest process switches it off for the other tests
    Svc s(c_, "svcw1");
    SetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", L"0");
    if (!s.up) return false;
    s.record = true;
    int port = FreePort(c_);
    // a fresh service: the site is ON, but only on 127.0.0.1, with the default user admin / admin
    Json st = Cmd(s, "web.status");
    T_CHECK(st.get("enabled").boolean()); T_CHECK(st.get("users").integer() == 1); T_CHECK(st.get("sessions").integer() == 0);
    T_CHECK(st.get("bind").size() == 1 && st.get("bind").at(0).str() == "127.0.0.1"); T_CHECK(st.get("port").integer() == 8443);
    T_CHECK(!TcpOpen("127.0.0.1", port));
    if (st.get("listening").boolean()) {                                                           // (port 8443 may be taken by something else on the machine: then the entry carries the error)
        T_CHECK(st.get("entries").size() == 1 && st.get("entries").at(0).get("addresses").size() == 1);
        Str dfp = st.get("cert").get("fingerprint").str(); T_CHECK(dfp.starts_with("sha256:"));    // the certificate was made on the first start
        WebCli w(8443, dfp); Json me; int rc = w.send("GET", "/api/me", "", &me); T_CHECK(rc == 200);
        T_CHECK(!me.get("authenticated").boolean() && !me.get("no_users").boolean());               // no "no users" text on a fresh install
        T_CHECK(w.login("admin", "Anything-Goes-12345") == 401);                                       // a wrong password does not log in
        T_CHECK(w.login("admin", "admin") == 200 && w.role == "admin"); T_CHECK(w.post("settings.get", Obj()) == 200);   // admin/admin: full admin rights
    } else c_.note("the default site does not listen on 8443: %s", st.dump().c_str());
    Json sg = Cmd(s, "web.settings.get"); const Json& d = sg.get("settings");
    T_CHECK(d.get("enabled").boolean()); T_CHECK(d.get("bind").size() == 1 && d.get("bind").at(0).str() == "127.0.0.1"); T_CHECK(d.get("port").integer() == 8443);
    T_CHECK(!d.get("require_2fa_admins").boolean()); T_CHECK(!d.get("trust_proxy").boolean()); T_CHECK(sg.get("warnings").size() == 0);
    // unknown web.* commands are routed to the site and refused there; the other services' commands do not mention the site
    T_CHECK(!s.call("web.bogus", Obj())); T_CHECK(s.lastErr.find("unknown command") >= 0 && s.lastErr.find("web.bogus") >= 0);
    T_CHECK(!s.call("web", Obj())); T_CHECK(s.lastErr.find("unknown command") >= 0);
    Json il = Cmd(s, "web.interfaces.list"); T_CHECK(il.get("interfaces").is_array());
    { Json ul = Cmd(s, "web.users.list").get("users"); T_CHECK(ul.size() == 1 && ul.at(0).get("name").str() == "admin" && ul.at(0).get("role").str() == "admin"); }
    // settings are validated: an empty list while enabled, a bad address
    { Json a = Obj(); a["enabled"] = true; a["bind"] = Json::array(); T_CHECK(!s.call("web.settings.set", a)); }
    { Json a = Obj(); a["bind"] = "999.1.1.1"; T_CHECK(!s.call("web.settings.set", a)); }
    T_CHECK(Cmd(s, "web.status").get("enabled").boolean());
    // users, a certificate
    Str pw = StrongPw("w1");
    Json us = s.ok("web.users.save", WebUser("root", "admin", pw, "optional")); T_CHECK(us.get("users").size() == 2 && us.get("users").at(1).get("name").str() == "root");
    T_CHECK(us.dump().find("hash") < 0 && us.dump().find("salt") < 0 && us.dump().find(pw.c_str()) < 0);
    T_CHECK(!s.call("web.users.save", WebUser("bobby", "viewer", "short", "off"))); T_CHECK(!s.lastErr.empty());
    T_CHECK(!s.call("web.users.save", WebUser("bobby", "viewer", "bobby-bobby-12", "off")));            // the name inside the password
    Json names = Obj(); { Json n = Json::array(); n.push("svc.test.local"); names["names"] = n; }
    Json cg = s.ok("web.cert.generate", names); Str fp = cg.get("fingerprint").str(); T_CHECK(fp.starts_with("sha256:"));
    Json cs = Cmd(s, "web.status"); T_CHECK(cs.get("cert").get("fingerprint").str() == fp); T_CHECK(cs.get("cert").get("self_signed").boolean()); T_CHECK(cs.dump().find("svc.test.local") >= 0);
    T_CHECK(!s.call("web.cert.generate", [] { Json a = Obj(); Json n = Json::array(); n.push("bad name\r\n"); a["names"] = n; return a; }()));
    // the site still sits on its default port: our test port is not open yet although a user and a certificate exist
    T_CHECK(!TcpOpen("127.0.0.1", port));
    // moving it to the test port: loopback only
    T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    st = Cmd(s, "web.status");
    T_CHECK(st.get("enabled").boolean() && st.get("listening").boolean()); T_CHECK(st.get("port").integer() == port); T_CHECK(st.get("address").str() == "127.0.0.1");
    T_CHECK(st.get("entries").size() == 1); const Json& ent = st.get("entries").at(0);
    T_CHECK(ent.get("entry").str() == "127.0.0.1" && ent.get("listening").boolean() && ent.get("addresses").size() == 1);
    T_CHECK(ent.get("addresses").at(0).get("address").str() == "127.0.0.1" && ent.get("addresses").at(0).get("listening").boolean());
    T_CHECK(st.get("cert").get("fingerprint").str() == fp);                                       // the generated certificate is the one in use
    T_CHECK(WaitOpen("127.0.0.1", port)); T_CHECK(!TcpOpen("127.0.0.2", port));                   // the other loopback address is not bound
    if (TcpOpen("127.0.0.2", port)) c_.note("127.0.0.2 answered: the listener is wider than 127.0.0.1");
    Json sg2 = Cmd(s, "web.settings.get"); T_CHECK(sg2.get("settings").get("enabled").boolean() && sg2.get("warnings").size() == 0);     // loopback: no warnings
    { WebCli w(port, fp); Json me; int rc = w.send("GET", "/api/me", "", &me); T_CHECK(rc == 200); if (rc != 200) c_.note("GET /api/me: %d %s", rc, w.err.c_str());
      T_CHECK(!me.get("authenticated").boolean() && !me.get("no_users").boolean()); T_CHECK(w.cl.peerFingerprint() == fp); T_CHECK(w.cl.tlsVersion() == "TLS 1.2" || w.cl.tlsVersion() == "TLS 1.3");
      WebCli bad(port, "sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"); T_CHECK(bad.send("GET", "/api/me", "") == 0); T_CHECK(bad.cl.certError); }
    // switching it off closes the port; the settings stay
    { Json a = Obj(); a["enabled"] = false; s.ok("web.settings.set", a); }
    T_CHECK(WaitClosed("127.0.0.1", port)); st = Cmd(s, "web.status"); T_CHECK(!st.get("enabled").boolean() && !st.get("listening").boolean());
    T_CHECK(Cmd(s, "web.settings.get").get("settings").get("port").integer() == port);
    { Json a = Obj(); a["enabled"] = true; s.ok("web.settings.set", a); }                         // on again: same port, same certificate
    T_CHECK(WaitOpen("127.0.0.1", port)); T_CHECK(Fingerprint(s) == fp);
    // nothing of the web secrets in the answers; the audit log knows what happened and holds no password
    T_CHECK(s.transcript.find(pw.c_str()) < 0);
    Json au = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 200; return a; }()); Str text = au.get("text").str();
    T_CHECK(text.find("settings_changed") >= 0 && text.find("cert_generated") >= 0 && text.find("user_saved") >= 0); T_CHECK(text.find(pw.c_str()) < 0);
    return true;
}

VM2B_TEST("svcweb", web_default_admin_flow_over_https)
{
    SetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", nullptr);
    Svc s(c_, "svcw1b");
    SetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", L"0");
    if (!s.up) return false;
    int port = FreePort(c_);
    { Json a = Obj(); a["port"] = port; T_CHECK(s.call("web.settings.set", a)); } if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    { WebCli w(port, fp); T_CHECK(w.login("admin", "admin") == 200 && w.role == "admin"); T_CHECK(w.post("state", Obj()) == 200); T_CHECK(w.post("servers.list", Obj()) == 200);
      T_CHECK(w.post("service.shutdown", Obj()) == 403); T_CHECK(w.post("web.status", Obj()) == 200); }                      // the admin reaches web.*, never service.shutdown
    // binding every address works with the default password (no guard)
    { Json a = Obj(); a["bind"] = "0.0.0.0"; a["port"] = port; T_CHECK(s.call("web.settings.set", a)); T_CHECK(Cmd(s, "web.status").get("listening").boolean()); }
    { Json a = Obj(); a["bind"] = "127.0.0.1"; a["port"] = port; T_CHECK(s.call("web.settings.set", a)); }
    // the password is changed through web.users.save: the old one stops working
    Str pw = StrongPw("w1b");
    s.ok("web.users.save", [&] { Json u = Obj(); u["name"] = "admin"; u["password"] = pw; return u; }());
    { WebCli w(port, fp); T_CHECK(w.login("admin", "admin") == 401); T_CHECK(w.login("admin", pw.c_str()) == 200); }
    // deleting every user does not bring the default administrator back, not even after a restart
    s.ok("web.users.delete", [] { Json u = Obj(); u["name"] = "admin"; return u; }());
    T_CHECK(s.restart()); if (c_.failed) return false;
    T_CHECK(Cmd(s, "web.users.list").get("users").size() == 0);
    Json au = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 300; return a; }()); Str text = au.get("text").str();
    T_CHECK(text.find("default_admin_created") >= 0); T_CHECK(text.find(pw.c_str()) < 0);
    return true;
}

VM2B_TEST("svcweb", web_settings_survive_service_restart)
{
    Svc s(c_, "svcw2"); if (!s.up) return false;
    int port = FreePort(c_);
    Str pw = StrongPw("w2");
    s.ok("web.users.save", WebUser("root", "admin", pw, "off"));
    s.ok("web.users.save", WebUser("eye", "viewer", StrongPw("w2v"), "off"));
    T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(fp.starts_with("sha256:")); T_CHECK(WaitOpen("127.0.0.1", port));
    { WebCli w(port, fp); T_CHECK(w.login("root", pw.c_str()) == 200); T_CHECK(w.role == "admin"); T_CHECK(w.post("state", Obj()) == 200); }
    T_CHECK(Cmd(s, "web.status").get("sessions").integer() >= 1);
    // restart the service: same settings, the site listens again with the same certificate and users; the old session is gone
    T_CHECK(s.restart()); if (c_.failed) return false;
    Json sg = Cmd(s, "web.settings.get").get("settings");
    T_CHECK(sg.get("enabled").boolean()); T_CHECK(sg.get("port").integer() == port); T_CHECK(sg.get("bind").size() == 1 && sg.get("bind").at(0).str() == "127.0.0.1"); T_CHECK(!sg.get("require_2fa_admins").boolean());
    bool up = WaitOpen("127.0.0.1", port, 10000); T_CHECK(up);
    if (!up) { c_.note("status: %s", Cmd(s, "web.status").dump().c_str()); return false; }
    Json st = Cmd(s, "web.status"); T_CHECK(st.get("listening").boolean() && st.get("entries").size() == 1 && st.get("entries").at(0).get("addresses").at(0).get("address").str() == "127.0.0.1");
    T_CHECK(st.get("cert").get("fingerprint").str() == fp); T_CHECK(st.get("users").integer() == 2); T_CHECK(st.get("sessions").integer() == 0);
    T_CHECK(!TcpOpen("127.0.0.2", port));
    { WebCli w(port, fp); T_CHECK(w.login("root", pw.c_str()) == 200); T_CHECK(w.post("jobs.list", Obj()) == 200); WebCli v(port, fp); T_CHECK(v.login("eye", StrongPw("w2v").c_str()) == 200 && v.role == "viewer"); }
    // switched off: it stays off after a restart (a fresh start is off by default, a stored "off" is kept)
    { Json a = Obj(); a["enabled"] = false; s.ok("web.settings.set", a); }
    T_CHECK(WaitClosed("127.0.0.1", port));
    T_CHECK(s.restart()); if (c_.failed) return false;
    T_CHECK(!Cmd(s, "web.settings.get").get("settings").get("enabled").boolean()); T_CHECK(!Cmd(s, "web.status").get("listening").boolean());
    Sleep(300); T_CHECK(!TcpOpen("127.0.0.1", port));
    T_CHECK(Cmd(s, "web.users.list").get("users").size() == 2);
    return true;
}

VM2B_TEST("svcweb", web_https_api_roles_and_refusals)
{
    g_shutdowns = 0;
    Svc s(c_, "svcw3", false, false, CountShutdown); if (!s.up) return false;
    s.record = true;
    int port = FreePort(c_);
    Str pwA = StrongPw("adm"), pwO = StrongPw("opr"), pwV = StrongPw("vwr");
    s.ok("web.users.save", WebUser("root", "admin", pwA, "off"));
    s.ok("web.users.save", WebUser("oper", "operator", pwO, "off"));
    s.ok("web.users.save", WebUser("eye", "viewer", pwV, "off"));
    T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    // the service has a task and a finished backup to work with
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), Random(c_, 60000)); Str srv = s.addDirServer(root);
    Str store = s.tmp.sub("store");
    Str taskId = s.saveTask("Web task", Svc::Element(srv, "vm1", JoinPath(store, "{VM}"), "xz", 1));
    Json ra = Obj(); ra["id"] = "Web task"; Str j0 = s.ok("tasks.run", ra).get("job").str(); T_CHECK(s.waitJob(j0) != ""); T_CHECK(s.waitHistory(1));
    Json bl = s.ok("backups.list", [] { Json a = Obj(); a["server"] = "local"; return a; }()); T_CHECK(bl.get("backups").size() == 1);
    Str archive = bl.get("backups").at(0).get("archive").str(), bdir = bl.get("backups").at(0).get("directory").str();
    int servers0 = (int)s.ok("servers.list", Obj()).size(), hist0 = s.historyCount(), jobs0 = (int)s.ok("jobs.list", Obj()).size();      // finished jobs stay listed for a minute
    Json restoreArgs = Obj(); restoreArgs["server"] = "local"; restoreArgs["directory"] = bdir; restoreArgs["archive"] = archive; restoreArgs["dest"] = s.tmp.sub("restored-web");
    Json newServer = Obj(); { Json x = Obj(); x["type"] = "directory"; x["name"] = "From the web"; x["path"] = root; newServer["server"] = x; }
    Json runArgs = Obj(); runArgs["id"] = "Web task";
    Json esxiRestore = Obj(); esxiRestore["server"] = "local"; esxiRestore["directory"] = bdir; esxiRestore["archive"] = archive; esxiRestore["esxi_server"] = srv; esxiRestore["options"] = GuiOptions("x", "dc", "", "", "", "", false);
    Json esxiOpts = Obj(); esxiOpts["server"] = srv;
    Json setArgs = Obj(); setArgs["max_parallel"] = 3;

    // not signed in: nothing is possible
    { WebCli w(port, fp); Json r; T_CHECK(w.post("state", Obj(), &r) == 401); T_CHECK(w.post("web.status", Obj()) == 401); T_CHECK(w.post("service.shutdown", Obj()) == 401); T_CHECK(w.post("tasks.run", runArgs) == 401); }
    WebCli adm(port, fp), opr(port, fp), vwr(port, fp);
    T_CHECK(adm.login("root", pwA.c_str()) == 200 && adm.role == "admin"); T_CHECK(opr.login("oper", pwO.c_str()) == 200 && opr.role == "operator"); T_CHECK(vwr.login("eye", pwV.c_str()) == 200 && vwr.role == "viewer");
    { WebCli w(port, fp); T_CHECK(w.login("root", "wrong-password-1") == 401);
      Json r; T_CHECK(w.loginRaw("root", pwA.c_str(), "", "", &r) == 429); T_CHECK(r.get("retry_after").integer() >= 1);       // the throttle holds even the right password for a moment
      T_CHECK(w.login("nobody", pwA.c_str()) == 401); T_CHECK(w.login("root", pwA.c_str()) == 200); }
    if (c_.failed) return false;

    // the viewer reads, and nothing else
    T_CHECK(vwr.post("state", Obj()) == 200); T_CHECK(vwr.post("tasks.list", Obj()) == 200); T_CHECK(vwr.post("history.list", Obj()) == 200);
    { Json r; T_CHECK(vwr.post("servers.list", Obj(), &r) == 200); T_CHECK(r.get("ok").boolean() && r.get("result").size() == (size_t)servers0); T_CHECK(r.dump().find("password") < 0); }
    T_CHECK(vwr.post("tasks.run", runArgs) == 403); T_CHECK(vwr.post("backup.run", Obj()) == 403); T_CHECK(vwr.post("jobs.abort", Obj()) == 403); T_CHECK(vwr.post("backups.verify", restoreArgs) == 403);
    T_CHECK(vwr.post("backups.restore", restoreArgs) == 403); T_CHECK(vwr.post("backups.restore_esxi", esxiRestore) == 403); T_CHECK(vwr.post("esxi.restore_options", esxiOpts) == 403);
    T_CHECK(vwr.post("servers.save", newServer) == 403); T_CHECK(vwr.post("tasks.save", Obj()) == 403); T_CHECK(vwr.post("settings.set", setArgs) == 403); T_CHECK(vwr.post("history.clear", Obj()) == 403);
    T_CHECK(vwr.post("backups.delete", restoreArgs) == 403); T_CHECK(vwr.post("scheduler.set", Obj()) == 403);
    // ... and none of those had an effect
    Sleep(200);
    T_CHECK((int)s.ok("jobs.list", Obj()).size() == jobs0); T_CHECK(s.historyCount() == hist0); T_CHECK((int)s.ok("servers.list", Obj()).size() == servers0);
    T_CHECK(!FileExists(restoreArgs.get("dest").str())); T_CHECK(s.ok("settings.get", Obj()).get("max_parallel").integer() != 3);
    // the operator runs and verifies, but does not restore or change the configuration
    T_CHECK(opr.post("tasks.list", Obj()) == 200);
    T_CHECK(opr.post("servers.save", newServer) == 403); T_CHECK(opr.post("backups.restore", restoreArgs) == 403); T_CHECK(opr.post("backups.restore_esxi", esxiRestore) == 403); T_CHECK(opr.post("settings.set", setArgs) == 403);
    T_CHECK(opr.post("tasks.delete", runArgs) == 403); T_CHECK(opr.post("history.clear", Obj()) == 403);
    T_CHECK(!FileExists(restoreArgs.get("dest").str())); T_CHECK((int)s.ok("servers.list", Obj()).size() == servers0);
    { Json r; T_CHECK(opr.post("tasks.run", runArgs, &r) == 200); T_CHECK(r.get("ok").boolean()); Str jid = r.get("result").get("job").str(); T_CHECK(!jid.empty()); s.waitJob(jid); T_CHECK(s.waitHistory(hist0 + 1)); hist0 = s.historyCount(); }
    { Json va = restoreArgs; va.remove("dest"); Json r; T_CHECK(opr.post("backups.verify", va, &r) == 200); T_CHECK(r.get("ok").boolean()); s.waitJob(r.get("result").get("job").str()); T_CHECK(s.waitHistory(hist0 + 1)); hist0 = s.historyCount(); }
    // the administrator does everything the site offers: servers, tasks, runs, restores, settings
    { Json r; T_CHECK(adm.post("servers.save", newServer, &r) == 200); T_CHECK(r.get("ok").boolean()); T_CHECK((int)s.ok("servers.list", Obj()).size() == servers0 + 1); }
    { Json r; T_CHECK(adm.post("backups.restore", restoreArgs, &r) == 200); T_CHECK(r.get("ok").boolean()); s.waitJob(r.get("result").get("job").str()); T_CHECK(s.waitHistory(hist0 + 1));
      Bytes got, orig; Str e; T_CHECK(ReadWholeFile(JoinPath(restoreArgs.get("dest").str(), "vm1\\disk.vmdk").c_str(), got, e)); T_CHECK(got.size() == 60000); }
    { Json r; T_CHECK(adm.post("settings.set", setArgs, &r) == 200); T_CHECK(r.get("ok").boolean()); T_CHECK(s.ok("settings.get", Obj()).get("max_parallel").integer() == 3); }
    { Json r; T_CHECK(adm.post("tasks.run", runArgs, &r) == 200); T_CHECK(r.get("ok").boolean()); s.waitJob(r.get("result").get("job").str()); }
    // the site never offers the service's own administration, to anybody: web.*, service.shutdown, unknown commands
    const char* never[] = { "service.shutdown", "nonsense", "web.bogus" };
    for (const char* cmd : never) {
        Json r; int hs = adm.post(cmd, Obj(), &r); if (hs != 403) c_.note("%s answered %d to the administrator", cmd, hs);
        T_CHECK(hs == 403); T_CHECK(!r.get("ok").boolean() || Str(cmd) == "web.bogus"); T_CHECK(opr.post(cmd, Obj()) == 403); T_CHECK(vwr.post(cmd, Obj()) == 403);
    }
    // web.* is the site's own administration: the admin may call it, the operator and the viewer may not (the same list, every command)
    const char* webCmds[] = { "web.status", "web.settings.get", "web.interfaces.list", "web.users.list", "web.audit.tail", "web.settings.set", "web.users.save", "web.users.delete", "web.cert.generate", "web.cert.import", "web.users.totp_enroll", "web.users.totp_confirm", "web.users.totp_disable" };
    for (const char* cmd : webCmds) { T_CHECK(opr.post(cmd, Obj()) == 403); T_CHECK(vwr.post(cmd, Obj()) == 403); T_CHECK(opr.post(cmd, Obj(), nullptr, false) == 403); }
    { Json r; T_CHECK(adm.post("web.status", Obj(), &r) == 200 && r.get("ok").boolean() && r.get("result").get("listening").boolean()); T_CHECK(adm.post("web.settings.get", Obj(), &r) == 200 && r.get("ok").boolean());
      T_CHECK(adm.post("web.users.list", Obj(), &r) == 200 && r.get("result").get("users").size() == 3 && r.dump().find("hash") < 0 && r.dump().find("salt") < 0); T_CHECK(adm.post("web.interfaces.list", Obj(), &r) == 200);
      T_CHECK(adm.post("web.status", Obj(), nullptr, false) == 403); }               // CSRF is required for every POST
    T_CHECK(g_shutdowns == 0);                                                                                     // the service is still there
    { Json st = Cmd(s, "web.status"); T_CHECK(st.get("enabled").boolean() && st.get("listening").boolean()); T_CHECK(st.get("users").integer() == 3); }
    T_CHECK(s.call("state", Obj()));
    // request hygiene: a session without the CSRF token, with a wrong one, a cross-origin request, a wrong content type, GET on a command
    { Json r; T_CHECK(adm.post("tasks.list", Obj(), &r, false) == 403); WebCli wrong(port, fp); T_CHECK(wrong.login("root", pwA.c_str()) == 200); wrong.csrf = "0000"; T_CHECK(wrong.post("tasks.list", Obj()) == 403);
      T_CHECK(adm.send("POST", "/api/tasks.list", "{}", nullptr, true, "https://evil.example.com") == 403); T_CHECK(adm.send("GET", "/api/tasks.list", "") == 405);
      T_CHECK(adm.post("tasks.list", Obj()) == 200); }
    // sign out: the session is dead
    { WebCli w(port, fp); T_CHECK(w.login("eye", pwV.c_str()) == 200); T_CHECK(w.post("tasks.list", Obj()) == 200); T_CHECK(w.post("logout", Obj()) == 200); T_CHECK(w.post("tasks.list", Obj()) == 401); }
    // the audit trail on the service side, and nothing secret anywhere
    Str text = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 500; return a; }()).get("text").str();
    T_CHECK(text.find("login_ok") >= 0 && text.find("login_failed") >= 0 && text.find("forbidden") >= 0 && text.find("csrf_refused") >= 0 && text.find("csrf_origin_refused") >= 0);
    T_CHECK(text.find("evil") < 0 || text.find("Evil-Password") < 0);
    Vec<Str> secrets; secrets.push_back(pwA); secrets.push_back(pwO); secrets.push_back(pwV); secrets.push_back("Evil-Password-12345");
    for (const Str& x : secrets) T_CHECK(text.find(x.c_str()) < 0 && s.transcript.find(x.c_str()) < 0);
    { Str hit = FindSecretInTree(s.opt.dataDir, secrets); if (!hit.empty()) c_.note("secret found in %s", hit.c_str()); T_CHECK(hit.empty()); }
    return true;
}

VM2B_TEST("svcweb", web_totp_login_flow_through_service)
{
    Svc s(c_, "svcw4"); if (!s.up) return false;
    s.record = true;
    int port = FreePort(c_);
    Str pw = StrongPw("totp");
    s.ok("web.users.save", WebUser("root", "admin", pw, "optional"));
    T_CHECK(EnableSite(s, port, true)); if (c_.failed) return false;                       // require_2fa_admins: the default
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), Random(c_, 30000)); Str srv = s.addDirServer(root);
    s.saveTask("T", Svc::Element(srv, "vm1", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1));
    // an administrator without TOTP cannot sign in, and the message says why
    { WebCli w(port, fp); Json r; T_CHECK(w.login("root", pw.c_str(), "", "", &r) == 401); T_CHECK(r.get("error").str().find("Two-factor") >= 0); T_CHECK(w.post("state", Obj()) == 401); }
    // enrollment over the pipe (the local administrator): secret shown once, confirmed with a code from the authenticator, recovery codes shown once
    Json en = s.ok("web.users.totp_enroll", [] { Json a = Obj(); a["name"] = "root"; return a; }());
    Str b32 = en.get("secret").str(); T_CHECK(b32.size() == 32); T_CHECK(en.get("uri").str().starts_with("otpauth://totp/VM2Backup:root?secret="));
    Bytes sec; T_CHECK(WebAuth::Base32Decode(b32, sec) && sec.size() == 20);
    { Json a = Obj(); a["name"] = "root"; a["code"] = "000000"; T_CHECK(!s.call("web.users.totp_confirm", a)); }
    Str code0 = WebAuth::Totp(sec, (u64)UnixNow());
    Json cf = s.ok("web.users.totp_confirm", [&] { Json a = Obj(); a["name"] = "root"; a["code"] = code0; return a; }());
    T_CHECK(cf.get("recovery_codes").size() == 10); Str rc0 = cf.get("recovery_codes").at(0).str(), rc1 = cf.get("recovery_codes").at(1).str(); T_CHECK(rc0.size() == 11 && rc0[5] == '-');
    T_CHECK(!s.call("web.users.totp_enroll", [] { Json a = Obj(); a["name"] = "root"; return a; }()));      // already enabled
    Json ul = s.ok("web.users.list", Obj()); T_CHECK(ul.dump().find(b32.c_str()) < 0 && ul.dump().find(rc0.c_str()) < 0); T_CHECK(ul.get("users").at(0).get("totp_enabled").boolean());
    // sign-in over HTTPS: password only -> "need 2FA"; a wrong code; a code of the neighbouring step (the code of this step was used by the confirmation); the replay of it
    WebCli w(port, fp);
    { Json r; T_CHECK(w.login("root", pw.c_str(), "", "", &r) == 202); T_CHECK(r.get("need_2fa").boolean()); T_CHECK(w.post("state", Obj()) == 401); }
    T_CHECK(w.login("root", pw.c_str(), "000000") == 401);
    Str code1 = WebAuth::Totp(sec, (u64)UnixNow() + 30);
    { Json r; int hs = w.login("root", pw.c_str(), code1, "", &r); if (hs != 200) c_.note("login with the code: %d %s", hs, r.dump().c_str()); T_CHECK(hs == 200); T_CHECK(r.get("role").str() == "admin"); }
    T_CHECK(w.post("tasks.list", Obj()) == 200); T_CHECK(w.post("web.status", Obj()) == 200);
    { Json r; T_CHECK(w.post("tasks.run", [] { Json a = Obj(); a["id"] = "T"; return a; }(), &r) == 200); T_CHECK(r.get("ok").boolean()); s.waitJob(r.get("result").get("job").str()); T_CHECK(s.waitHistory(1)); }
    { WebCli r2(port, fp); T_CHECK(r2.login("root", pw.c_str(), code1) == 401); }                  // a code is good once
    // recovery codes: once each, case-insensitive
    { WebCli r3(port, fp); T_CHECK(r3.login("root", pw.c_str(), "", rc0) == 200); T_CHECK(r3.post("state", Obj()) == 200); WebCli r4(port, fp); T_CHECK(r4.login("root", pw.c_str(), "", rc0) == 401);
      WebCli r5(port, fp); T_CHECK(r5.login("root", pw.c_str(), "", rc1.lower()) == 200); }
    T_CHECK(s.ok("web.users.list", Obj()).get("users").at(0).get("recovery_left").integer() == 8);
    // a wrong password never gets to the second factor
    { WebCli r6(port, fp); Json r; T_CHECK(r6.login("root", "wrong-password-12", "", "", &r) == 401); T_CHECK(!r.get("need_2fa").boolean()); }
    // the second factor is off again (local administrator), and the policy "admins need 2FA" refuses the sign-in
    s.ok("web.users.totp_disable", [] { Json a = Obj(); a["name"] = "root"; return a; }());
    { WebCli r7(port, fp); T_CHECK(r7.login("root", pw.c_str()) == 401); }
    { Json a = Obj(); a["require_2fa_admins"] = false; s.ok("web.settings.set", a); }
    { WebCli r8(port, fp); T_CHECK(r8.login("root", pw.c_str()) == 200); }
    // nothing secret in the audit trail or the answers
    Str text = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 500; return a; }()).get("text").str();
    T_CHECK(text.find("login_ok") >= 0 && text.find("recovery_code_used") >= 0);
    T_CHECK(text.find(pw.c_str()) < 0 && text.find(b32.c_str()) < 0 && text.find(rc0.c_str()) < 0 && text.find(code1.c_str()) < 0);
    T_CHECK(s.transcript.find(pw.c_str()) < 0);
    Bytes webjson; Str we; T_CHECK(ReadWholeFile(JoinPath(s.opt.dataDir, "web.json").c_str(), webjson, we)); T_CHECK(!HasBytes(webjson, b32)); T_CHECK(!HasBytes(webjson, pw));
    return true;
}

// Restore to ESXi / vCenter from the web site: admin only, through the same service commands the Windows program used, against the mock.
VM2B_TEST("svcweb", web_admin_restores_to_esxi_over_https)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    OpenLogOnce();
    Svc s(c_, "svcwx"); if (!s.up) return false;
    Str store = s.tmp.sub("store"), pwA = StrongPw("wxa"), pwO = StrongPw("wxo"), pwV = StrongPw("wxv");
    s.ok("web.users.save", WebUser("root", "admin", pwA, "off")); s.ok("web.users.save", WebUser("oper", "operator", pwO, "off")); s.ok("web.users.save", WebUser("eye", "viewer", pwV, "off"));
    int port = FreePort(c_); T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    WebCli adm(port, fp), opr(port, fp), vwr(port, fp);
    T_CHECK(adm.login("root", pwA.c_str()) == 200); T_CHECK(opr.login("oper", pwO.c_str()) == 200); T_CHECK(vwr.login("eye", pwV.c_str()) == 200);
    bool seen[2] = { false, false };
    for (const Env& e : all) {
        if (seen[e.vcenter() ? 1 : 0]) continue; seen[e.vcenter() ? 1 : 0] = true;
        c_.note("%s", e.name.c_str());
        Ctl ctl(e); ctl.reset();
        Json sa = Obj(); sa["server"] = e.record(); Str srv = s.ok("servers.save", sa).get("id").str(); T_CHECK(!srv.empty());
        Fx f; T_CHECK(MakeFx(c_, ctl, e, f, "WX", 1, 200000, 20000, "poweredOff")); if (!f.m.ok) return false;
        Json el = Svc::Element(srv, f.spec.name.c_str(), JoinPath(store, "{VM}"), "xz", 1, nullptr);
        Str taskName = "WebEsxi " + e.name; Str taskId = s.saveTask(taskName.c_str(), el); T_CHECK(!taskId.empty());
        Json ra = Obj(); ra["id"] = taskName; Str jid = s.ok("tasks.run", ra).get("job").str(); Str fin = s.waitJob(jid, 90000); T_CHECK(fin == "Success" || fin == "Gone");
        Json rec; T_CHECK(HistoryOf(s, jid, rec)); if (c_.failed) return false;
        Str archive = rec.get("results").at(0).get("archive").str(); T_CHECK(!archive.empty());
        Json qa = Obj(); qa["server"] = "local"; Json bl = s.ok("backups.list", qa); Str bdir;
        for (size_t i = 0; i < bl.get("backups").size(); ++i) if (bl.get("backups").at(i).get("archive").str() == archive) bdir = bl.get("backups").at(i).get("directory").str();
        T_CHECK(!bdir.empty());
        // the restore options: admin yes, operator / viewer no, a missing CSRF token no
        Json oa = Obj(); oa["server"] = srv; if (e.vcenter()) oa["datacenter"] = f.spec.dc;
        T_CHECK(opr.post("esxi.restore_options", oa) == 403); T_CHECK(vwr.post("esxi.restore_options", oa) == 403); T_CHECK(adm.post("esxi.restore_options", oa, nullptr, false) == 403);
        Json opts; T_CHECK(adm.post("esxi.restore_options", oa, &opts) == 200); T_CHECK(opts.get("ok").boolean());
        const Json& od = opts.get("result"); T_CHECK(od.get("datastores").size() >= 2 && od.get("pools").size() >= 1); Str dc = od.get("datacenter").str(); T_CHECK(!dc.empty());
        T_CHECK(opts.dump().find(AsciiPrefix(e.password).c_str()) < 0);
        // the restore itself
        Str newName = f.spec.name + " from web"; int regBefore = ctl.counter("reg:RegisterVM");
        Json ra2 = RestoreArgs(bdir, archive, "", srv, GuiOptions(newName, dc, "datastore1", "", "", "", false));
        T_CHECK(opr.post("backups.restore_esxi", ra2) == 403); T_CHECK(vwr.post("backups.restore_esxi", ra2) == 403); T_CHECK(adm.post("backups.restore_esxi", ra2, nullptr, false) == 403);
        T_CHECK(ctl.counter("reg:RegisterVM") == regBefore);
        Json rr; T_CHECK(adm.post("backups.restore_esxi", ra2, &rr) == 200); T_CHECK(rr.get("ok").boolean());
        Str rj = rr.get("result").get("job").str(); T_CHECK(!rj.empty());
        { WebCli w(port, fp); T_CHECK(w.login("root", pwA.c_str()) == 200); Json jl; T_CHECK(w.post("jobs.list", Obj(), &jl) == 200); }
        Str st = s.waitJob(rj, 90000); T_CHECK(st == "Success" || st == "Gone");
        Json hr; T_CHECK(HistoryOf(s, rj, hr)); T_CHECK(hr.get("kind").str() == "restore_esxi" && hr.get("status").str() == "Success");
        { Json g = Obj(); g["id"] = rj; Json hg; T_CHECK(adm.post("history.get", g, &hg) == 200); const Json& row = hg.get("result").get("results").at(0);
          T_CHECK(row.get("restored_vm").str() == newName && row.get("vmx").str().ends_with(".vmx")); }
        T_CHECK(ctl.counter("reg:RegisterVM") == regBefore + 1); T_CHECK(!ctl.vmState(newName).is_null()); T_CHECK(CheckRestored(c_, e, f, newName, "datastore1"));
        // an existing name is refused by the service, through the web as well
        { Json again = RestoreArgs(bdir, archive, "", srv, GuiOptions(newName, dc, "", "", "", "", false)); Json r2; T_CHECK(adm.post("backups.restore_esxi", again, &r2) == 200);
          Str j2 = r2.get("result").get("job").str(); T_CHECK(!j2.empty()); s.waitJob(j2, 60000); Json h2; T_CHECK(HistoryOf(s, j2, h2)); T_CHECK(h2.get("status").str() == "Failed"); }
        T_CHECK(ctl.counter("reg:RegisterVM") == regBefore + 1);
        ctl.deleteVm(f.spec.name); ctl.deleteVm(newName);
        if (c_.failed) return false;
    }
    Str text = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 500; return a; }()).get("text").str();
    T_CHECK(text.find("backups.restore_esxi") >= 0 && text.find("forbidden") >= 0);
    return true;
}

// The site administers itself over HTTPS (admin role): the lock-out protections, the deferred listener restart, the acting user in the audit log.
VM2B_TEST("svcweb", web_self_administration_over_https)
{
    Svc s(c_, "svcw5"); if (!s.up) return false;
    s.record = true;
    int port = FreePort(c_), port2 = FreePort(c_); if (port2 == port) port2 = FreePort(c_);
    Str pwA = StrongPw("sa1"), pwB = StrongPw("sa2"), pwO = StrongPw("sa3");
    s.ok("web.users.save", WebUser("root", "admin", pwA, "optional")); s.ok("web.users.save", WebUser("oper", "operator", pwO, "off"));
    T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    WebCli adm(port, fp), opr(port, fp);
    T_CHECK(adm.login("root", pwA.c_str()) == 200); T_CHECK(opr.login("oper", pwO.c_str()) == 200);
    auto set = [&](WebCli& w, const Json& a, Json* r = nullptr) { Json x; int hs = w.post("web.settings.set", a, &x); if (r) *r = x; return hs == 200 && x.get("ok").boolean(); };
    auto errOf = [&](WebCli& w, const char* cmd, const Json& a) { Json x; w.post(cmd, a, &x); return x.get("ok").boolean() ? Str() : (x.get("error").str().empty() ? Str("?") : x.get("error").str()); };
    // 1. the site cannot switch itself off; the local channel still can
    { Json a = Obj(); a["enabled"] = false; Str e = errOf(adm, "web.settings.set", a); T_CHECK(e.find("switched off") >= 0); T_CHECK(Cmd(s, "web.status").get("enabled").boolean()); T_CHECK(adm.post("state", Obj()) == 200); }
    // 2. the bind list must keep a loopback entry
    { Json a = Obj(); Json b = Json::array(); b.push("0.0.0.0"); a["bind"] = b; T_CHECK(errOf(adm, "web.settings.set", a).find("loopback") >= 0);
      Json a2 = Obj(); Json b2 = Json::array(); b2.push("all"); a2["bind"] = b2; T_CHECK(errOf(adm, "web.settings.set", a2).find("loopback") >= 0);
      Json a3 = Obj(); a3["bind"] = Json::array(); T_CHECK(!errOf(adm, "web.settings.set", a3).empty());
      Json a4 = Obj(); Json b4 = Json::array(); b4.push("999.1.1.1"); b4.push("127.0.0.1"); a4["bind"] = b4; T_CHECK(!errOf(adm, "web.settings.set", a4).empty());
      T_CHECK(Cmd(s, "web.settings.get").get("settings").get("bind").size() == 1); }
    // 3. an address that cannot be bound is refused with the reason, nothing changes (a port in use is detected the same way; Wine lets a second REUSEADDR socket in, so it is not tested here)
    {   Json a = Obj(); Json b = Json::array(); b.push("127.0.0.1"); b.push("203.0.113.77"); a["bind"] = b; a["port"] = port2;
        Str e = errOf(adm, "web.settings.set", a); T_CHECK(e.find("cannot listen on 203.0.113.77") >= 0 && e.find("does not exist") >= 0);
        T_CHECK(Cmd(s, "web.settings.get").get("settings").get("port").integer() == port); T_CHECK(Cmd(s, "web.settings.get").get("settings").get("bind").size() == 1);
        T_CHECK(adm.post("state", Obj()) == 200); T_CHECK(WaitOpen("127.0.0.1", port)); T_CHECK(!TcpOpen("127.0.0.1", port2));
        Json a2 = Obj(); a2["port"] = 70000; T_CHECK(!errOf(adm, "web.settings.set", a2).empty()); }
    // 4. a good change: answered first (on the old connection), applied after; the sessions survive the restart
    { Json a = Obj(); a["port"] = port2; Json b = Json::array(); b.push("127.0.0.1"); b.push("127.0.0.2"); a["bind"] = b; a["session_minutes"] = 30;
      Json r; T_CHECK(set(adm, a, &r)); T_CHECK(r.get("result").get("settings").get("port").integer() == port2);
      T_CHECK(WaitOpen("127.0.0.1", port2, 15000)); T_CHECK(WaitOpen("127.0.0.2", port2, 5000)); T_CHECK(WaitClosed("127.0.0.1", port, 10000));
      Json st = Cmd(s, "web.status"); T_CHECK(st.get("port").integer() == port2 && st.get("listening").boolean() && st.get("sessions").integer() >= 2);
      T_CHECK(Cmd(s, "web.settings.get").get("settings").get("session_minutes").integer() == 30); }
    WebCli adm2(port2, fp), opr2(port2, fp); T_CHECK(adm2.login("root", pwA.c_str()) == 200);
    // 5. users: no self-deletion, no self-demotion, the password policy, a new admin
    T_CHECK(errOf(adm2, "web.users.delete", [] { Json a = Obj(); a["name"] = "root"; return a; }()).find("your own account") >= 0);
    T_CHECK(errOf(adm2, "web.users.save", [] { Json a = Obj(); a["name"] = "ROOT"; a["role"] = "viewer"; return a; }()).find("your own role") >= 0);
    T_CHECK(errOf(adm2, "web.users.save", WebUser("bobby", "viewer", "short", "off")).size() > 3); T_CHECK(errOf(adm2, "web.users.save", WebUser("bobby", "viewer", "bobby-bobby-12", "off")).size() > 3);
    T_CHECK(errOf(adm2, "web.users.save", WebUser("boss", "admin", pwB, "off")).empty());
    { Json ul; T_CHECK(adm2.post("web.users.list", Obj(), &ul) == 200); T_CHECK(ul.get("result").get("users").size() == 3); T_CHECK(ul.dump().find(pwB.c_str()) < 0); }
    WebCli boss(port2, fp); T_CHECK(boss.login("boss", pwB.c_str()) == 200);
    T_CHECK(opr2.login("oper", pwO.c_str()) == 200);
    T_CHECK(errOf(boss, "web.users.save", [] { Json a = Obj(); a["name"] = "oper"; a["role"] = "viewer"; return a; }()).empty());                 // another user's role: fine; its sessions end
    T_CHECK(opr2.post("state", Obj()) == 401);                                                                                                      // its sessions ended with the role change
    T_CHECK(errOf(boss, "web.users.delete", [] { Json a = Obj(); a["name"] = "oper"; return a; }()).empty());
    T_CHECK(errOf(boss, "web.users.delete", [] { Json a = Obj(); a["name"] = "ghost"; return a; }()).size() > 0);
    // 6. 2FA: enrol yourself (the session stays), requiring it needs your own 2FA, you cannot drop yours while it is required
    { Json a = Obj(); a["require_2fa_admins"] = true; T_CHECK(errOf(boss, "web.settings.set", a).find("two-factor") >= 0); }
    Json en; T_CHECK(boss.post("web.users.totp_enroll", [] { Json a = Obj(); a["name"] = "boss"; return a; }(), &en) == 200 && en.get("ok").boolean());
    Str b32 = en.get("result").get("secret").str(); Bytes sec; T_CHECK(WebAuth::Base32Decode(b32, sec) && sec.size() == 20);
    T_CHECK(en.get("result").get("uri").str().starts_with("otpauth://totp/VM2Backup:boss?secret="));
    Json cf; T_CHECK(boss.post("web.users.totp_confirm", [&] { Json a = Obj(); a["name"] = "boss"; a["code"] = WebAuth::Totp(sec, (u64)UnixNow()); return a; }(), &cf) == 200 && cf.get("ok").boolean());
    T_CHECK(cf.get("result").get("recovery_codes").size() == 10); T_CHECK(boss.post("state", Obj()) == 200);                                          // the own session survives the enrolment
    { Json a = Obj(); a["require_2fa_admins"] = true; T_CHECK(set(boss, a)); }
    T_CHECK(errOf(boss, "web.users.totp_disable", [] { Json a = Obj(); a["name"] = "boss"; return a; }()).find("required for administrators") >= 0);
    { WebCli w(port2, fp); Json r; T_CHECK(w.login("root", pwA.c_str(), "", "", &r) == 401); T_CHECK(r.get("error").str().find("Two-factor") >= 0); }    // root has no 2FA: refused with the reason
    { Json a = Obj(); a["require_2fa_admins"] = false; T_CHECK(set(boss, a)); }
    T_CHECK(errOf(boss, "web.users.totp_disable", [] { Json a = Obj(); a["name"] = "boss"; return a; }()).empty());
    T_CHECK(boss.post("state", Obj()) == 401); T_CHECK(boss.login("boss", pwB.c_str()) == 200);          // a 2FA change ends the user's sessions, own included
    // 7. changing your own password ends your sessions
    { Str e9 = errOf(boss, "web.users.save", [&] { Json a = Obj(); a["name"] = "boss"; a["password"] = StrongPw("sa9"); return a; }()); if (!e9.empty()) c_.note("own password change: %s", e9.c_str()); T_CHECK(e9.empty()); } T_CHECK(boss.post("state", Obj()) == 401);
    // 8. a new self-signed certificate: the listener restarts after the answer, the fingerprint changes
    { Json a = Obj(); Json n = Json::array(); n.push("site.test.local"); a["names"] = n; Json r; T_CHECK(adm2.post("web.cert.generate", a, &r) == 200 && r.get("ok").boolean());
      Str nfp = r.get("result").get("fingerprint").str(); T_CHECK(nfp.starts_with("sha256:") && nfp != fp);
      bool seen = false; for (int i = 0; i < 100 && !seen; ++i) { Sleep(100); seen = Fingerprint(s) == nfp; } T_CHECK(seen);
      bool up = false; for (int i = 0; i < 100 && !up; ++i) { Sleep(100); WebCli w(port2, nfp); Json m; up = w.send("GET", "/api/me", "", &m) == 200; } T_CHECK(up); fp = nfp; }
    // 9. a certificate cannot be read from a path on the server through the site, an upload must be a real file
    WebCli adm3(port2, fp); T_CHECK(adm3.login("boss", StrongPw("sa9").c_str()) == 200);
    { Json a = Obj(); a["pfx_path"] = "C:\\Windows\\win.ini"; a["password"] = "x"; T_CHECK(errOf(adm3, "web.cert.import", a).find("upload") >= 0); }
    { Json a = Obj(); a["pfx_base64"] = "AAAA"; a["password"] = "x"; T_CHECK(!errOf(adm3, "web.cert.import", a).empty()); }
    { Json a = Obj(); { Str big; big.resize(1500000, 'A'); a["pfx_base64"] = big; } a["password"] = "x"; T_CHECK(errOf(adm3, "web.cert.import", a).find("too large") >= 0); }
    T_CHECK(!FileExists(JoinPath(s.opt.dataDir, "web-upload.pfx.tmp")));
    // 10. the audit log names the acting user and holds no secret
    Str text = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 600; return a; }()).get("text").str();
    T_CHECK(text.find("settings_changed\troot") >= 0 && text.find("settings_changed\tboss") >= 0); T_CHECK(text.find("user_deleted\tboss") >= 0); T_CHECK(text.find("totp_confirmed\tboss") >= 0); T_CHECK(text.find("cert_generated\troot") >= 0);
    T_CHECK(text.find(pwA.c_str()) < 0 && text.find(pwB.c_str()) < 0 && text.find(b32.c_str()) < 0);
    T_CHECK(s.transcript.find(b32.c_str()) < 0);
    return true;
}

// net.drives / net.browse over the site: admin only, CSRF needed, the browse password never in an answer or the audit log.
VM2B_TEST("svcweb", web_network_commands_admin_only)
{
    Svc s(c_, "svcw6"); if (!s.up) return false;
    Str pwA = StrongPw("nw1"), pwO = StrongPw("nw2"), pwV = StrongPw("nw3");
    s.ok("web.users.save", WebUser("root", "admin", pwA, "off")); s.ok("web.users.save", WebUser("oper", "operator", pwO, "off")); s.ok("web.users.save", WebUser("eye", "viewer", pwV, "off"));
    int port = FreePort(c_); T_CHECK(EnableSite(s, port, false)); if (c_.failed) return false;
    Str fp = Fingerprint(s); T_CHECK(WaitOpen("127.0.0.1", port));
    WebCli adm(port, fp), opr(port, fp), vwr(port, fp);
    T_CHECK(adm.login("root", pwA.c_str()) == 200); T_CHECK(opr.login("oper", pwO.c_str()) == 200); T_CHECK(vwr.login("eye", pwV.c_str()) == 200);
    const Str secret = "Br0wse-Web-Secret-77";
    Json bad = Obj(); bad["path"] = "\\\\?\\C:\\"; bad["user"] = "corp\\admin"; bad["password"] = secret;              // refused before any network access
    Json drv = Obj();
    WebCli* lows[2] = { &opr, &vwr };
    for (WebCli* w : lows) { T_CHECK(w->post("net.drives", drv) == 403); T_CHECK(w->post("net.browse", bad) == 403); }
    T_CHECK(adm.post("net.drives", drv, nullptr, false) == 403); T_CHECK(adm.post("net.browse", bad, nullptr, false) == 403);
    { Json r; T_CHECK(adm.post("net.drives", drv, &r) == 200); T_CHECK(r.get("ok").boolean() && r.get("result").get("drives").is_array()); T_CHECK(r.dump().find("password") < 0); }
    { Json r; T_CHECK(adm.post("net.browse", bad, &r) == 200); T_CHECK(!r.get("ok").boolean() && !r.get("error").str().empty()); T_CHECK(r.dump().find(secret.c_str()) < 0); }
    Str text = s.ok("web.audit.tail", [] { Json a = Obj(); a["lines"] = 200; return a; }()).get("text").str();
    T_CHECK(text.find(secret.c_str()) < 0 && text.find("forbidden") >= 0); T_CHECK(s.transcript.find(secret.c_str()) < 0);
    return true;
}

} // namespace vm2b
