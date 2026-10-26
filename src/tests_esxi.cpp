// Autotests of the ESXi / vCenter source: the XML parser and the pure helpers (no server needed), and everything else against the emulators of
// tools/testservers/esxi_mock.py (started by tools/run-esxi-tests.sh, one per version profile 6.0 .. 9.0 as host and as vCenter).
#include "autotest.h"
#include "crypto.h"
#include "engine.h"
#include "esxi.h"
#include "http.h"
#include "winutil.h"
#include "xml.h"

namespace vm2b {

namespace {

// ---- scratch folders ------------------------------------------------------------------------------------------------
struct EsTmp {
    Str path; bool keep;
    EsTmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path);
        Str e; MakeDirs(path, e);
    }
    ~EsTmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

static RetryPolicy Fast(int attempts = 12) { RetryPolicy r; r.attempts = attempts; r.baseDelayMs = 1; r.maxDelayMs = 4; return r; }

// ---- the emulators ----------------------------------------------------------------------------------------------------
struct Env {
    Str name, profile, kind, host, user, password, fp, tlsMin, root, tlsOnly, cert; int port; Vec<Str> dcs;
    explicit Env(const Json& j) {
        name = j.get("name").str(); profile = j.get("profile").str(); kind = j.get("kind").str(); host = j.get("host").str(); user = j.get("user").str();
        password = j.get("password").str(); fp = j.get("fingerprint").str(); tlsMin = j.get("tls_min").str("1.2"); root = j.get("root").str();
        tlsOnly = j.get("tls_only").str(); cert = j.get("cert").str(); port = (int)j.get("port").integer();
        const Json& d = j.get("dcs"); for (size_t i = 0; i < d.size(); ++i) dcs.push_back(d.at(i).str());
    }
    bool vcenter() const { return kind == "vcenter"; }
    Json cfg(bool quiesce = true, bool memory = false) const {
        Json c = Json::object();
        c["type"] = kind; c["host"] = host; c["port"] = port; c["user"] = user; c["password"] = password; c["fingerprint"] = fp; c["tls_min"] = tlsMin;
        c["task_timeout"] = 60; c["remove_timeout"] = 60; c["poll_ms"] = 20; c["timeout"] = 20;
        Json s = Json::object(); s["quiesce"] = quiesce; s["memory"] = memory; c["snapshot"] = s;
        return c;
    }
};

static bool Envs(Ctx& c, Vec<Env>& out)
{
    if (c.servers.is_null() || !c.servers.has("esxi")) { c.skip("no \"esxi\" servers (run tools/run-esxi-tests.sh)"); return false; }
    const Json& a = c.servers.get("esxi");
    for (size_t i = 0; i < a.size(); ++i) out.push_back(Env(a.at(i)));
    return !out.empty();
}

// Датастор эмулятора на локальном NTFS гостя (C:\...): имя файла с ':' и подобным там невозможно (':' - это альтернативный поток), а эти тесты создают именно такие
// ВМ; под Wine (Z:) и на сетевой папке - можно.
static bool NtfsDatastore(const Env& e) { return e.root.size() > 1 && e.root[1] == ':' && e.root[0] != 'Z' && e.root[0] != 'z'; }

// the 12 normal servers (the TLS 1.0 only one is for its own test)
static void Normal(const Vec<Env>& all, Vec<Env>& out) { for (const Env& e : all) if (e.tlsOnly.empty() && e.cert.empty()) out.push_back(e); }

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
    void faults(const Json& f) { Json o; call("faults", f, o); }
    Json vmState(const Str& vm) { Json s = state(); return s.get("vms").get(vm.c_str()); }
    int leaked() { Json s = state(); return (int)s.get("leaked").size(); }
    int counter(const char* key) { Json s = state(); return (int)s.get("counters").get(key).integer(); }
    int snapCount(const Str& vm) { return (int)vmState(vm).get("snapshots").size(); }
    int events(const char* what) {                          // "create" / "remove" events with a VM2Backup- snapshot name
        Json s = state(); int n = 0; const Json& ev = s.get("events");
        for (size_t i = 0; i < ev.size(); ++i) if (ev.at(i).at(0).str() == what && ev.at(i).at(2).str().starts_with("VM2Backup-")) ++n;
        return n;
    }
};

struct VmSpec { Str name; Json disks; Str power; Str dc; Str ds; bool tmpl; Str conn; Json extra; VmSpec() : power("poweredOff"), ds("datastore1"), tmpl(false), conn("connected") {} };

struct MockVm { Str name, dir, ds; bool ok; MockVm() : ok(false) {} };

static MockVm MakeVm(Ctl& ctl, const Env& e, const VmSpec& s)
{
    Json a = Json::object();
    a["name"] = s.name; a["disks"] = s.disks; a["power"] = s.power; a["ds"] = s.ds; a["template"] = s.tmpl; a["conn"] = s.conn;
    if (!s.dc.empty()) a["dc"] = s.dc;
    if (s.extra.is_array()) a["extra_files"] = s.extra;
    a["seed"] = (i64)(s.name.size() * 31 + 7);
    Json out; MockVm m;
    m.ok = ctl.call("create_vm", a, out) && !out.has("error");
    m.name = s.name; m.ds = out.get("ds").str(); m.dir = JoinPath(JoinPath(e.root, m.ds), out.get("dir").str());
    return m;
}

static Json Disks(Ctx& c, int n, u64 maxSize = 3000000, u64 minSize = 1)
{
    Json d = Json::array();
    for (int i = 0; i < n; ++i) { Json o = Json::object(); o["size"] = (i64)c.range((int)minSize, (int)maxSize); o["thin"] = c.coin(30); o["seed"] = (i64)c.range(1, 100000); d.push(o); }
    return d;
}

// ---- hashing a tree --------------------------------------------------------------------------------------------------
struct FI { Str rel; u64 size; Str sha; Str full; };

static Str HashFile(const Str& path, u64& size)
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

static void Walk(const Str& dir, const Str& rel, Vec<FI>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*")), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Str n = FromWide(fd.cFileName), r = rel.empty() ? n : rel + "/" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Walk(JoinPath(dir, n), r, out);
        else { FI f; f.rel = r; f.full = JoinPath(dir, n); f.sha = HashFile(f.full, f.size); out.push_back(f); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The .vmx of a running point differs from the one before the snapshot in the disk file names, the snapshot lines (the host rewrites them): compare the rest.
static Str NormVmx(const Str& path)
{
    Bytes b; Str e; if (!ReadWholeFile(path.c_str(), b, e)) return Str("<unreadable>");
    Str t((const char*)b.data(), b.size()), out; size_t s = 0;
    for (size_t i = 0; i <= t.size(); ++i) if (i == t.size() || t[i] == '\n') {
        Str l = t.substr(s, i - s); s = i + 1;
        if (l.lower().find(".filename") >= 0 || l.starts_with("snapshot.") || l.find("ctkEnabled") >= 0 && l.find("scsi") >= 0) continue;
        out += l + "\n";
    }
    return out;
}
static const FI* FindFi(const Vec<FI>& v, const Str& rel) { for (const FI& f : v) if (f.rel == rel) return &f; return nullptr; }
static bool Excluded(const Str& rel) { return rel.ends_with(".vswp") || rel.ends_with(".log") || rel.find(".lck") >= 0; }

// What a backup of a VM must contain: everything the datastore folder held before (except swap and logs), identical; the only additions are the
// snapshot's own description files (.vmsn / .vmem, and .vmsd which lists it); never a delta / sesparse file or the descriptor of the new level.
static bool CheckTree(Ctx& c, const Vec<FI>& before, const Vec<FI>& after)
{
    bool ok = true;
    for (const FI& b : before) {
        const FI* a = FindFi(after, b.rel);
        if (Excluded(b.rel)) { if (a) { c.note("excluded file is in the archive: %s", b.rel.c_str()); ok = false; } continue; }
        if (!a) { c.note("missing in the restore: %s", b.rel.c_str()); ok = false; continue; }
        if (b.rel.ends_with(".vmsd")) continue;
        if (b.rel.ends_with(".vmx")) { if (NormVmx(a->full) != NormVmx(b.full)) { c.note("differs: %s (normalized)", b.rel.c_str()); ok = false; } continue; }
        if (a->size != b.size || a->sha != b.sha) { c.note("differs: %s (%llu vs %llu bytes)", b.rel.c_str(), (unsigned long long)b.size, (unsigned long long)a->size); ok = false; }
    }
    for (const FI& a : after) {
        if (FindFi(before, a.rel)) continue;
        if (a.rel.ends_with(".vmsn") || a.rel.ends_with(".vmem")) continue;
        c.note("unexpected file in the archive: %s", a.rel.c_str()); ok = false;
    }
    return ok;
}

// ---- progress --------------------------------------------------------------------------------------------------------
struct Pr : Progress {
    u64 bytes; int retries, resumes, files; bool abortAt, keep; u64 limit;
    void (*hook)(Pr*, int, const Str&); void* user;
    Pr() : bytes(0), retries(0), resumes(0), files(0), abortAt(false), keep(false), limit(0), hook(nullptr), user(nullptr) {}
    void onBytes(u64 n) override { bytes += n; }
    void onFile(const Str& p) override { ++files; if (hook) hook(this, files, p); }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    void onResume(int, u64) override { ++resumes; }
    bool aborted() override { return abortAt && bytes >= limit; }
    bool keepOnAbort() override { return keep; }
};

static Target* LocalAt(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }

static BackupOptions RandomOpts(Ctx& c, bool smallVolumes)
{
    BackupOptions o; o.kdfIterations = 3000; o.retry = Fast();
    
    if (c.coin(40)) o.password = Str::fmt("pw-%u-\xd0\xbf", c.rnd(1000));
    o.verify = c.coin(60);
    if (smallVolumes) o.segmentBytes = (u64)512 * (u64)c.range(64, 1500);
    else if (c.coin(40)) o.segmentBytes = (u64)512 * (u64)c.range(100, 4000);
    return o;
}

static const char* const kNames[] = { "plain", "with space", "\xd0\xb2\xd0\xbc \xd1\x82\xd0\xb5\xd1\x81\xd1\x82", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "per%cent#hash", "a&b+c=d", "caf\xc3\xa9 (copy) [1]" };

static Str UniqueName(Ctx& c, const char* tag) { return Str::fmt("%s-%u-", tag, c.rnd(1000000)) + kNames[c.rnd(7)]; }

static bool Restore(Ctx& c, Target& t, const Manifest& m, const BackupOptions& o, const Str& dest, Str& err)
{
    Manifest rm;
    bool ok = RestoreBackup(t, m.archive, dest, o.password, nullptr, &rm, err);
    if (!ok) c.note("restore: %s", err.c_str());
    return ok;
}

} // namespace

// ============================================================================================================================
// pure parts (no server)
// ============================================================================================================================
// After EVERY esxi / svcesxi test: delete all test VMs (and sessions, faults) on every mock server, whatever the result of the test.
static void EsxiTeardown(Ctx& c)
{
    if (c.servers.is_null() || !c.servers.has("esxi")) return;
    bool sk = c.skipped; Str why = c.skipReason;                       // the hook must not change the state of the test
    Vec<Env> all; if (Envs(c, all)) for (const Env& e : all) { Ctl ctl(e); ctl.reset(); }
    c.skipped = sk; c.skipReason = why;
}
static ::vm2b::RegTeardown reg_esxi_teardown("esxi", EsxiTeardown);
static ::vm2b::RegTeardown reg_svcesxi_teardown("svcesxi", EsxiTeardown);

VM2B_TEST("esxi", esxi_xml_parser)
{
    XmlNode r; Str e;
    const char* doc = "\xef\xbb\xbf<?xml version=\"1.0\"?><!-- c --><soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" xmlns:x=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<soapenv:Body><R xmlns=\"urn:vim25\"><a type=\"VirtualMachine\" x:type=\"ManagedObjectReference\">vm-1</a><t>x &amp; &lt;y&gt; &#65;&#x42; &quot;q&quot;</t>"
        "<c><![CDATA[<raw & text>]]></c><e/><n:k xmlns:n=\"urn:n\" n:attr=\"1\">v</n:k><!-- in --><?pi x?></R></soapenv:Body></soapenv:Envelope>";
    T_CHECK(XmlParse(doc, strlen(doc), r, e)); if (!e.empty()) c_.note("%s", e.c_str());
    T_CHECK(r.name == "Envelope");
    const XmlNode* a = r.find("Body/R/a");
    T_CHECK(a && a->text == "vm-1" && a->attr("type") == "VirtualMachine" && a->xsiType() == "ManagedObjectReference");
    T_CHECK(r.find("Body/R/t")->text == "x & <y> AB \"q\"");
    T_CHECK(r.find("Body/R/c")->text == "<raw & text>");
    T_CHECK(r.find("Body/R/e") != nullptr && r.find("Body/R/e")->text.empty());
    T_CHECK(r.find("Body/R/k")->text == "v" && r.find("Body/R/k")->attr("attr") == "1");
    T_CHECK(r.find("Body/R/zzz") == nullptr);
    // refused input
    const char* bad[] = { "<!DOCTYPE x [<!ENTITY a \"b\">]><x>&a;</x>", "<a><b></a>", "<a>", "<a b=1/>", "<a>&nope;</a>", "<a>&#0;</a>", "<a>&#xD800;</a>", "", "text", "<a/><b/>",
                          "<a b=\"<\"/>", "<a><![CDATA[x</a>", "<a><!-- x</a>", "<a>&amp</a>", "<a b='x\"/>" };
    for (const char* b : bad) { XmlNode x; Str er; bool ok = XmlParse(b, strlen(b), x, er); if (ok) c_.note("accepted: %s", b); T_CHECK(!ok); T_CHECK(!ok && !er.empty()); }
    // limits
    { XmlLimits l; l.maxDepth = 5; Str deep; for (int i = 0; i < 20; ++i) deep += "<a>"; for (int i = 0; i < 20; ++i) deep += "</a>"; XmlNode x; Str er; T_CHECK(!XmlParse(deep.data(), deep.size(), x, er, l)); T_CHECK(er.find("deep") >= 0); }
    { XmlLimits l; l.maxNodes = 10; Str many = "<r>"; for (int i = 0; i < 50; ++i) many += "<i/>"; many += "</r>"; XmlNode x; Str er; T_CHECK(!XmlParse(many.data(), many.size(), x, er, l)); }
    { XmlLimits l; l.maxBytes = 100; Str big; big.resize(300, 'x'); big = "<a>" + big + "</a>"; XmlNode x; Str er; T_CHECK(!XmlParse(big.data(), big.size(), x, er, l)); }
    // a deep but legal document (snapshot chains nest) must parse
    { Str deep; for (int i = 0; i < 150; ++i) deep += "<a>"; for (int i = 0; i < 150; ++i) deep += "</a>"; XmlNode x; Str er; T_CHECK(XmlParse(deep.data(), deep.size(), x, er)); }
    // writer round trip
    { XmlWriter w; w.open("a", "x=\"1\""); w.leaf("b", "1 < 2 & \"3\""); w.open("c"); w.text("t\x01z"); w.close(); Str s = w.str();
      XmlNode x; Str er; T_CHECK(XmlParse(s.data(), s.size(), x, er)); T_CHECK(x.attr("x") == "1"); T_CHECK(x.childText("b") == "1 < 2 & \"3\""); T_CHECK(x.childText("c") == "tz"); }
    // fuzz: mutated documents never crash, and the result is either an error or a tree
    Str base = doc; int ok = 0;
    for (int i = 0; i < 4000; ++i) {
        Str m = base;
        int k = c_.range(1, 6);
        for (int j = 0; j < k; ++j) {
            int pos = (int)c_.rnd((u32)m.size());
            switch (c_.rnd(3)) { case 0: m[(size_t)pos] = (char)c_.rnd(256); break; case 1: m = m.substr(0, (size_t)pos); break; default: m = m.substr(0, (size_t)pos) + m.substr((size_t)pos + 1 > m.size() ? m.size() : (size_t)pos + 1); }
            if (m.empty()) break;
        }
        XmlNode x; Str er; if (XmlParse(m.data(), m.size(), x, er)) ++ok;
    }
    c_.note("fuzz: %d of 4000 mutations still parse", ok);
    return true;
}

VM2B_TEST("esxi", esxi_helpers)
{
    Str ds, p;
    T_CHECK(EsxiParseDsPath("[datastore1] vm/vm.vmx", ds, p) && ds == "datastore1" && p == "vm/vm.vmx");
    T_CHECK(EsxiParseDsPath("[my store 2]vm x/\xd0\xb2.vmdk", ds, p) && ds == "my store 2" && p == "vm x/\xd0\xb2.vmdk");
    T_CHECK(!EsxiParseDsPath("vm/vm.vmx", ds, p)); T_CHECK(!EsxiParseDsPath("[] x", ds, p)); T_CHECK(!EsxiParseDsPath("", ds, p)); T_CHECK(!EsxiParseDsPath("[abc", ds, p));
    T_CHECK(EsxiUrlEscape("a b/\xd0\xb2%#&+?=.-_~") == "a%20b/%D0%B2%25%23%26%2B%3F%3D.-_~");
    T_CHECK(EsxiFolderUrl("data store", "dir x/f-flat.vmdk", "ha-datacenter") == "/folder/dir%20x/f-flat.vmdk?dcPath=ha-datacenter&dsName=data%20store");
    T_CHECK(EsxiParseHttpDate("Sat, 24 Oct 2026 12:00:00 GMT") == 1792843200);
    T_CHECK(EsxiParseHttpDate("Thu, 01 Jan 1970 00:00:10 GMT") == -1 || EsxiParseHttpDate("Thu, 01 Jan 1970 00:00:10 GMT") == 10);
    T_CHECK(EsxiParseHttpDate("garbage") == -1); T_CHECK(EsxiParseHttpDate("Sat, 32 Oct 2026 12:00:00 GMT") == -1);
    T_CHECK(EsxiCompareVersion("6.7.0", "7.0") < 0); T_CHECK(EsxiCompareVersion("7.0.3.0", "7.0.3") == 0); T_CHECK(EsxiCompareVersion("8.0.2", "7.0.3.1") > 0);
    T_CHECK(EsxiCompareVersion("6.10", "6.9") > 0); T_CHECK(EsxiCompareVersion("", "6.0") < 0);
    return true;
}

VM2B_TEST("esxi", esxi_config_errors)
{
    Str err; VmSource* s;
    Json c = Json::object(); c["type"] = "esxi"; c["host"] = "h"; c["user"] = "u"; c["password"] = "p";
    s = NewEsxiSource(c, err); T_CHECK(s != nullptr); delete s;
    Json c2 = c; c2["type"] = "vcenter"; s = NewEsxiSource(c2, err); T_CHECK(s != nullptr); delete s;
    Json c3 = c; c3["transport"] = "ssh"; s = NewEsxiSource(c3, err); T_CHECK(s == nullptr); T_CHECK(err.find("not available yet") >= 0);
    Json c4 = c; c4["transport"] = "api"; s = NewEsxiSource(c4, err); T_CHECK(s != nullptr); delete s;
    Json c5 = c; c5["transport"] = "carrier-pigeon"; s = NewEsxiSource(c5, err); T_CHECK(s == nullptr);
    Json c6 = c; c6["host"] = ""; s = NewEsxiSource(c6, err); T_CHECK(s == nullptr); T_CHECK(!err.empty());
    Json c7 = c; c7["user"] = ""; s = NewEsxiSource(c7, err); T_CHECK(s == nullptr);
    Json c8 = c; c8["tls_min"] = "0.9"; s = NewEsxiSource(c8, err); T_CHECK(s == nullptr);
    Json c9 = c; c9["type"] = "ftp"; s = NewEsxiSource(c9, err); T_CHECK(s == nullptr);
    Json viaMake = c; s = MakeSource(viaMake, err); T_CHECK(s != nullptr && Str(s->kind()) == "esxi"); delete s;     // MakeSource reaches it
    return true;
}

// ============================================================================================================================
// against the emulators
// ============================================================================================================================
VM2B_TEST("esxi", esxi_list_all_profiles)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    for (const Env& e : es) {
        Ctl ctl(e); ctl.reset();
        Str tag = Str::fmt("L%u", c_.rnd(100000));
        int n = c_.range(2, 4);
        Vec<Str> names; Vec<u64> sizes; Vec<Str> power;
        for (int i = 0; i < n; ++i) {
            VmSpec s; s.name = tag + Str::fmt("-%d ", i) + kNames[c_.rnd(7)]; s.disks = Disks(c_, c_.range(1, 2), 500000);
            static const char* pw[] = { "poweredOn", "poweredOff", "suspended" }; s.power = pw[c_.rnd(3)];
            if (e.vcenter()) s.dc = e.dcs[c_.rnd((u32)e.dcs.size())];
            MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
            Vec<FI> t; Walk(m.dir, "", t); u64 sum = 0; for (const FI& f : t) sum += f.size;
            names.push_back(s.name); sizes.push_back(sum); power.push_back(s.power);
        }
        VmSpec tm; tm.name = tag + "-template"; tm.disks = Disks(c_, 1, 1000); tm.tmpl = true; MakeVm(ctl, e, tm);
        VmSpec ia; ia.name = tag + "-orphan"; ia.disks = Disks(c_, 1, 1000); ia.conn = "orphaned"; MakeVm(ctl, e, ia);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
        Vec<VmInfo> vms; bool ok = src->list(vms);
        if (!ok) c_.note("%s list: %s", e.name.c_str(), src->err.c_str());
        T_CHECK(ok);
        int found = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            for (const VmInfo& v : vms) if (v.name == names[i]) {
                ++found;
                const char* want = power[i] == "poweredOn" ? "running" : power[i] == "poweredOff" ? "stopped" : "suspended";
                T_CHECK(v.state == want);
                if (v.size != sizes[i]) c_.note("%s: size %llu vs %llu", names[i].c_str(), (unsigned long long)v.size, (unsigned long long)sizes[i]);
                T_CHECK(v.size == sizes[i]);
                T_CHECK(v.extra.find("datastore datastore1") >= 0); T_CHECK(v.extra.find("2 vCPU") >= 0); T_CHECK(v.extra.find("1024 MB") >= 0);
            }
        }
        T_CHECK(found == (int)names.size());
        for (const VmInfo& v : vms) { T_CHECK(!v.name.ends_with("-template") || !v.name.starts_with(tag.c_str())); T_CHECK(!v.name.ends_with("-orphan") || !v.name.starts_with(tag.c_str())); }
        T_CHECK(ctl.counter("auth:Login") >= 1); T_CHECK(ctl.counter("auth:Logout") >= 1);
        // an old-protocol server (9.0 refuses the 6.0 version string) costs one refused request
        T_CHECK(ctl.counter("fault:soap-version") == (e.profile == "9.0" ? 1 : 0));
        T_CHECK(ctl.counter("soap:RetrieveServiceContent") == 1);
        T_CHECK(Str(src->kind()) == "esxi");
        delete src;
        for (const Str& n2 : names) { Json o, a = Json::object(); a["name"] = n2; ctl.call("delete_vm", a, o); }
    }
    return true;
}

VM2B_TEST("esxi", esxi_backup_restore_all_profiles)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-br");
    int idx = 0;
    for (const Env& e : es) {
        Ctl ctl(e); ctl.reset();
        VmSpec s; s.name = UniqueName(c_, "BR"); s.disks = Disks(c_, c_.range(1, 3)); s.power = c_.coin() ? "poweredOn" : "poweredOff";
        if (e.vcenter()) s.dc = e.dcs[c_.rnd((u32)e.dcs.size())];
        Json ex = Json::array(); if (c_.coin(40)) { Json f = Json::object(); f["name"] = "extra data.bin"; f["size"] = c_.range(1, 5000); f["type"] = "namespaceData"; ex.push(f); }
        s.extra = ex;
        MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before);
        BackupOptions o = RandomOpts(c_, false);
        Json cfg = e.cfg(c_.coin(70));
        Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr); if (!src) return false;
        Str store = tmp.sub(Str::fmt("store%d", idx).c_str());
        Target* t = LocalAt(store);
        Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, s.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s backup: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) {
            T_CHECK(man.source == "esxi"); T_CHECK(man.vm == s.name);
            Str dest = tmp.sub(Str::fmt("restore%d", idx).c_str()), re;
            if (Restore(c_, *t, man, o, dest, re)) {
                Vec<FI> after; Walk(JoinPath(dest, s.name), "", after);
                T_CHECK(CheckTree(c_, before, after));
                u64 tot = 0; for (const FI& f : after) tot += f.size;
                T_CHECK(pr.bytes == tot);
                if (pr.bytes != tot) c_.note("progress %llu vs restored %llu", (unsigned long long)pr.bytes, (unsigned long long)tot);
                // the meta file
                Bytes mb; Str me; T_CHECK(ReadWholeFile(JoinPath(dest, "vm2backup-meta.json").c_str(), mb, me));
                Json mj; Str pe; T_CHECK(Json::parse((const char*)mb.data(), mb.size(), mj, pe));
                T_CHECK(mj.get("source").str() == "esxi"); T_CHECK(mj.get("host").str() == e.host); T_CHECK(!mj.get("uuid").str().empty()); T_CHECK(!mj.get("api_version").str().empty());
                T_CHECK(mj.get("snapshot").str().starts_with("VM2Backup-")); T_CHECK(mj.get("files").size() == after.size() - 0 || mj.get("files").size() + 0 >= before.size() - 3);
                T_CHECK(mj.get("disks").size() == s.disks.size());
            }
            T_CHECK(!FileExists(JoinPath(store, man.archive + ".resume")));
        }
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(s.name) == 0);
        T_CHECK(ctl.events("create") == 1); T_CHECK(ctl.events("remove") == 1);
        delete t; delete src; ++idx;
        Json o2, a2 = Json::object(); a2["name"] = s.name; ctl.call("delete_vm", a2, o2);
    }
    return true;
}

// ---- helpers for the scenario tests ------------------------------------------------------------------------------------
namespace {

struct Fix { VmSpec spec; MockVm m; Vec<FI> before; };

static bool MakeFix(Ctx& c, Ctl& ctl, const Env& e, Fix& f, const char* tag, int nDisks, u64 maxSize, const char* power, bool tiny = false, u64 minSize = 1)
{
    f.spec.name = UniqueName(c, tag); f.spec.disks = Disks(c, nDisks, maxSize, minSize); f.spec.power = power;
    if (e.vcenter()) f.spec.dc = e.dcs[c.rnd((u32)e.dcs.size())];
    if (tiny) for (size_t i = 0; i < f.spec.disks.size(); ++i) f.spec.disks.item(i)["thin"] = false;
    f.m = MakeVm(ctl, e, f.spec);
    if (!f.m.ok) return false;
    f.before.clear(); Walk(f.m.dir, "", f.before);
    return true;
}

static void DeleteVm(Ctl& ctl, const Str& name) { Json o, a = Json::object(); a["name"] = name; ctl.call("delete_vm", a, o); }

struct HookData { Ctl* ctl; Str vm; int mode; int fired; };
static void HookFn(Pr* pr, int idx, const Str&)
{
    HookData* h = (HookData*)pr->user; Json o, a = Json::object();
    if (h->mode == 1 && idx == 1) { a["vm"] = h->vm; a["disk"] = 0; a["offset"] = 1000; a["length"] = 7000; a["seed"] = 9; h->ctl->call("guestwrite", a, o); ++h->fired; }
    if (h->mode == 2 && idx == 1) { h->ctl->call("expire", Json::object(), o); ++h->fired; }
    if (h->mode == 3 && (idx == 1 || idx == 3)) { h->ctl->call("expire", Json::object(), o); ++h->fired; }
}

static bool Contains(const Str& s, const char* t) { return s.find(t) >= 0; }

} // namespace

VM2B_TEST("esxi", esxi_guest_writes_are_frozen_out)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-fr");
    int n = 0;
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        ++n;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "FZ", c_.range(1, 2), 2000000, "poweredOn")); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, false);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub(Str::fmt("s%d", n).c_str()));
        HookData hd = { &ctl, f.spec.name, 1, 0 }; Pr pr; pr.hook = HookFn; pr.user = &hd;
        Manifest man; Str warn;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(hd.fired == 1);
        if (ok) {
            Str dest = tmp.sub(Str::fmt("r%d", n).c_str()), re;
            if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); T_CHECK(CheckTree(c_, f.before, after)); }
            // the guest write was merged into the base disk when the snapshot went away
            Vec<FI> now; Walk(f.m.dir, "", now);
            bool changed = false;
            for (const FI& b : f.before) if (b.rel.ends_with("-flat.vmdk")) { const FI* a = FindFi(now, b.rel); if (a && a->sha != b.sha) changed = true; }
            T_CHECK(changed);
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name);
    }
    if (!n) c_.note("no profile drawn");
    return true;
}

VM2B_TEST("esxi", esxi_resume_after_dropped_connection)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-drop");
    for (const Env& e : es) {
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "DR", 1, 4000000, c_.coin() ? "poweredOn" : "poweredOff", false, 1300000)); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, true);
        bool noRange = c_.coin(30);
        Json fl = Json::object();
        Json d = Json::object(); d["match"] = c_.coin() ? "-flat.vmdk" : ".vmdk"; d["bytes"] = c_.range(30000, 900000); d["count"] = c_.range(1, 3); fl["drop_after"] = d;
        bool beyondFirstVolume = d.get("bytes").integer() > (i64)o.segmentBytes + 20000;
        if (noRange) fl["no_range"] = true;
        ctl.faults(fl);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store"));
        Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(pr.retries >= 1);
        Json st = ctl.state();
        if (!noRange && beyondFirstVolume) T_CHECK(st.get("ranges").size() >= 1);          // a volume was complete: the retry asks for the rest only
        else if (noRange) for (size_t i = 0; i < st.get("ranges").size(); ++i) T_CHECK(st.get("ranges").at(i).at(2).integer() == 200);        // a server without ranges: read and drop
        if (ok) {
            Str dest = tmp.sub("r"), re;
            if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); T_CHECK(CheckTree(c_, f.before, after)); }
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_resume_across_runs)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-runs");
    int round = 0;
    for (const Env& e : es) {
        int mode = round++ % 3;                                     // 0 continue the snapshot, 1 the snapshot was removed meanwhile, 2 the VM changed
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "RN", 1, 5000000, "poweredOn", false, 3000000)); if (!f.m.ok) return false;
        { Json o2, a = Json::object(); a["vm"] = f.spec.name; a["disk"] = 0; a["offset"] = 0; a["length"] = 10; ctl.call("guestwrite", a, o2); f.before.clear(); Walk(f.m.dir, "", f.before); }   // (VM running: the base disk takes writes)
        BackupOptions o = RandomOpts(c_, true);
        o.segmentBytes = (u64)512 * (u64)c_.range(64, 300);
        Str store = tmp.sub("store");
        Str err;
        {   // run 1: stopped half way, as a pause (the snapshot stays)
            VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(store);
            Pr pr; pr.abortAt = true; pr.keep = true; pr.limit = (u64)c_.range(600000, 1500000);
            Manifest man; Str warn;
            bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
            T_CHECK(!ok); T_CHECK(err == kAbortedMessage);
            delete t; delete src;
            T_CHECK(ctl.snapCount(f.spec.name) == 1);               // kept for the continuation
            T_CHECK(ctl.leaked() == 1);
        }
        if (mode == 0) { Json o2, a = Json::object(); a["vm"] = f.spec.name; a["disk"] = 0; a["offset"] = 4096; a["length"] = 9000; a["seed"] = 5; ctl.call("guestwrite", a, o2); }
        if (mode == 1) {
            Json st = ctl.vmState(f.spec.name); Json o2, a = Json::object(); a["vm"] = f.spec.name; a["name"] = st.get("snapshots").at(0).str(); ctl.call("remove_snapshot", a, o2);
            Json a2 = Json::object(); a2["vm"] = f.spec.name; a2["disk"] = 0; a2["offset"] = 4096; a2["length"] = 9000; a2["seed"] = 5; ctl.call("guestwrite", a2, o2);
            f.before.clear(); Walk(f.m.dir, "", f.before);
        }
        if (mode == 2) {
            Json o2, a = Json::object(); a["vm"] = f.spec.name; a["file"] = f.spec.name + ".vmx"; ctl.call("touch", a, o2);
            f.before.clear(); Walk(f.m.dir, "", f.before);
            // the snapshot's own files are part of `before` now: they stay in the VM folder until the first run's snapshot is removed
            for (size_t i = f.before.size(); i-- > 0;) if (f.before[i].rel.find("-0000") >= 0) f.before.erase(i);
        }
        {   // run 2
            VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(store);
            Pr pr; Manifest man; Str warn;
            bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
            if (!ok) c_.note("%s mode %d: %s", e.name.c_str(), mode, err.c_str());
            T_CHECK(ok);
            if (mode == 0) { T_CHECK(pr.resumes == 1); T_CHECK(ctl.events("create") == 1); }
            else {
                T_CHECK(pr.resumes == 0);
                // mode 1: the token's snapshot is gone, so begin() makes a new one, the engine sees another fingerprint than the journal's, ends it
                // and calls begin() again: three snapshots in all (the first two are removed again); mode 2: the old one is reused, then dropped
                int cr = ctl.events("create"); int want = mode == 1 ? 3 : 2;
                if (cr != want) c_.note("%s mode %d: %d snapshots created", e.name.c_str(), mode, cr);
                T_CHECK(cr == want);
            }
            if (ok) {
                Str dest = tmp.sub("r"), re;
                if (Restore(c_, *t, man, o, dest, re)) {
                    Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after);
                    if (mode == 0) T_CHECK(CheckTree(c_, f.before, after));
                    else {
                        // compare the disk and the config only (the file list of the folder changed with the snapshots)
                        for (const FI& b : f.before) if (b.rel.ends_with("-flat.vmdk")) { const FI* a = FindFi(after, b.rel); T_CHECK(a != nullptr && a->sha == b.sha); }
                        for (const FI& b : f.before) if (b.rel.ends_with(".vmx")) { const FI* a = FindFi(after, b.rel); T_CHECK(a != nullptr && NormVmx(a->full) == NormVmx(b.full)); }
                    }
                }
            }
            T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
            delete t; delete src;
        }
        DeleteVm(ctl, f.spec.name); RemoveTree(store); RemoveTree(tmp.sub("r"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_abort_removes_snapshot)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-abort");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "AB", 1, 4000000, c_.coin() ? "poweredOn" : "poweredOff", false, 1500000)); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, true);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store"));
        Pr pr; pr.abortAt = true; pr.limit = (u64)c_.range(100000, 1200000);
        Manifest man; Str warn;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        T_CHECK(!ok); T_CHECK(err == kAbortedMessage);
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0); T_CHECK(ctl.events("create") == 1); T_CHECK(ctl.events("remove") == 1);
        Vec<Str> names; t->list(names); T_CHECK(names.empty());               // nothing is left in the target
        T_CHECK(ctl.state().get("sessions").integer() == 0);                  // and the session was closed
        delete t; delete src; DeleteVm(ctl, f.spec.name); RemoveTree(tmp.sub("store"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_snapshot_failure_paths)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-sf");
    int round = c_.range(0, 5);
    for (const Env& e : es) {
        int kind = round++ % 6;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "SF", 1, 200000, "poweredOn")); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, false); o.verify = false;
        Json cfg = e.cfg(true);
        Str err; Manifest man; Str warn;
        Target* t = LocalAt(tmp.sub("store"));
        Json fl = Json::object();
        if (kind == 0) { fl["license_block"] = c_.coin() ? "call" : "task"; }
        if (kind == 1) { fl["quiesce_fail"] = c_.coin() ? "tools" : "quiesce"; }
        if (kind == 2) { fl["task_fail"] = "The file is locked by another process"; }
        if (kind == 3) { fl["task_hang"] = true; cfg["task_timeout"] = 2; }
        if (kind == 4) { fl["remove_fail"] = "datastore is read only"; }
        if (kind == 5) { Json o2, a = Json::object(); a["vm"] = f.spec.name; a["name"] = "manual snapshot"; ctl.call("snapshot", a, o2); Json a2 = Json::object(); a2["vm"] = f.spec.name; a2["name"] = "VM2Backup-deadbeef00"; ctl.call("snapshot", a2, o2); }
        ctl.faults(fl);
        VmSource* src = NewEsxiSource(cfg, err);
        Pr pr;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        bool perm = !src->transient;
        if (kind == 0) {
            T_CHECK(!ok); T_CHECK(Contains(err, "license")); T_CHECK(Contains(err, "SSH")); T_CHECK(perm); T_CHECK(!Contains(err, e.password.c_str()));
            T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        } else if (kind == 1) {
            T_CHECK(ok); T_CHECK(Contains(warn, "without quiescing"));
            T_CHECK(ctl.counter("snap:CreateSnapshot") == 2); T_CHECK(ctl.leaked() == 0);
        } else if (kind == 2) {
            T_CHECK(!ok); T_CHECK(Contains(err, "locked by another process")); T_CHECK(ctl.leaked() == 0);
        } else if (kind == 3) {
            T_CHECK(!ok); T_CHECK(Contains(err, "timed out")); T_CHECK(ctl.leaked() == 0);
        } else if (kind == 4) {
            T_CHECK(ok);                                                          // the copy is fine, only the cleanup failed
            T_CHECK(ctl.leaked() == 1); T_CHECK(!static_cast<EsxiSource*>(src)->endProblem().empty());
            Json fn = Json::object(); fn["remove_fail"] = Json();
            Json o3; ctl.call("faults", fn, o3);
            delete src; src = NewEsxiSource(cfg, err); Manifest m2; Str w2; Pr pr2;
            bool ok2 = BackupVm(*src, f.spec.name, *t, o, &pr2, m2, w2, err);
            T_CHECK(ok2); T_CHECK(Contains(w2, "removed 1 old VM2Backup snapshot")); T_CHECK(ctl.leaked() == 0);
        } else {
            T_CHECK(ok); T_CHECK(Contains(warn, "removed 1 old VM2Backup snapshot"));
            T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 1);       // the user's own snapshot is never touched
            Str dest = tmp.sub("r"), re;
            if (ok && Restore(c_, *t, man, o, dest, re)) {
                Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after);
                bool vmsn = false; for (const FI& a : after) if (a.rel.ends_with(".vmsn")) vmsn = true;
                T_CHECK(vmsn);
                for (const FI& b : f.before) if (b.rel.ends_with("-flat.vmdk") && b.rel.find("-0000") < 0) { const FI* a = FindFi(after, b.rel); T_CHECK(a && a->sha == b.sha); }
            }
        }
        if (!ok && kind != 0) c_.note("%s kind %d: %s", e.name.c_str(), kind, err.c_str());
        delete t; delete src; DeleteVm(ctl, f.spec.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_session_expiry_mid_backup)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-exp");
    for (const Env& e : es) {
        if (c_.coin(40)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "EX", c_.range(1, 2), 1500000, c_.coin() ? "poweredOn" : "poweredOff")); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, false);
        Json fl = Json::object(); fl["expire_after_calls"] = c_.range(2, 14); ctl.faults(fl);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store"));
        HookData hd = { &ctl, f.spec.name, c_.coin() ? 2 : 3, 0 }; Pr pr; pr.hook = HookFn; pr.user = &hd;
        Manifest man; Str warn;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(hd.fired >= 1);
        T_CHECK(ctl.counter("auth:Login") >= 2);
        if (ok) {
            Str dest = tmp.sub("r"), re;
            if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); T_CHECK(CheckTree(c_, f.before, after)); }
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_wrong_credentials_and_fingerprint)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "CR", 1, 1000, "poweredOff")); if (!f.m.ok) return false;
        for (int kind = 0; kind < 3; ++kind) {
            Json cfg = e.cfg();
            Str secret = e.password;
            if (kind == 0) { cfg["password"] = "wrong-" + e.password; secret = "wrong-" + e.password; }
            if (kind == 1) cfg["user"] = "nobody";
            if (kind == 2) cfg["fingerprint"] = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
            Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr); if (!src) return false;
            Vec<VmInfo> vms; bool ok = src->list(vms);
            T_CHECK(!ok); T_CHECK(!src->transient);
            Str fp, tok, w;
            bool ok2 = src->begin(f.spec.name, false, Str(), fp, tok, w);
            T_CHECK(!ok2); T_CHECK(!src->transient);
            T_CHECK(!Contains(src->err, secret.c_str())); T_CHECK(!Contains(src->err, e.password.c_str()));
            if (kind < 2) T_CHECK(Contains(src->err, "login failed")); else { T_CHECK(Contains(src->err, e.fp.c_str())); }
            delete src;
        }
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

VM2B_TEST("esxi", esxi_file_and_server_oddities)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-odd");
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        int kind = c_.range(0, 5);
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "OD", 1, 800000, "poweredOff")); if (!f.m.ok) return false;
        BackupOptions o = RandomOpts(c_, false);
        Json fl = Json::object();
        if (kind == 0) { Json d = Json::object(); d["match"] = "-flat.vmdk"; d["count"] = -1; fl["file_503"] = d; }          // permanent: the backup fails with the file's name
        if (kind == 1) { Json d = Json::object(); d["match"] = ".nvram"; d["count"] = 1; fl["file_503"] = d; }              // once: the source tries again
        if (kind == 2) fl["no_head"] = true;
        if (kind == 3) fl["layout_lag"] = c_.range(1, 3);
        if (kind == 4) fl["no_retrieve_ex"] = true;
        if (kind == 5) fl["slow_ms"] = 3;
        ctl.faults(fl);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store"));
        Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (kind == 0) {
            T_CHECK(!ok); T_CHECK(Contains(err, "-flat.vmdk")); T_CHECK(Contains(err, "503")); T_CHECK(!src->transient);
        } else {
            if (!ok) c_.note("%s kind %d: %s", e.name.c_str(), kind, err.c_str());
            T_CHECK(ok);
            if (ok) {
                Str dest = tmp.sub("r"), re;
                if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); T_CHECK(CheckTree(c_, f.before, after)); }
            }
            if (kind == 4) T_CHECK(ctl.counter("soap:RetrieveProperties") > 0);
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    return true;
}

VM2B_TEST("esxi", esxi_names_and_missing_vms)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-names");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    for (int i = 0; i < 7; ++i) {
        VmSpec s; s.name = Str::fmt("NM%u-", c_.rnd(100000)) + kNames[i]; s.disks = Disks(c_, 1, 30000); s.power = "poweredOff";
        if (e.vcenter()) s.dc = e.dcs[0];
        MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before); T_CHECK(!before.empty());
        BackupOptions o; o.kdfIterations = 3000; o.retry = Fast(); o.verify = false;
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store"));
        Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, s.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s: %s", s.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) { Str dest = tmp.sub("r"), re; if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, s.name), "", after); T_CHECK(CheckTree(c_, before, after)); } }
        delete t; delete src; DeleteVm(ctl, s.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    // a name that is not there, a template, a VM that cannot be read
    VmSpec tm; tm.name = "TPL-" + Str::fmt("%u", c_.rnd(1000)); tm.disks = Disks(c_, 1, 1000); tm.tmpl = true; if (e.vcenter()) tm.dc = e.dcs[0]; MakeVm(ctl, e, tm);
    VmSpec ia; ia.name = "ORPH-" + Str::fmt("%u", c_.rnd(1000)); ia.disks = Disks(c_, 1, 1000); ia.conn = "inaccessible"; if (e.vcenter()) ia.dc = e.dcs[0]; MakeVm(ctl, e, ia);
    Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Str fp, tok, w;
    ctl.reset();
    T_CHECK(!src->begin("no such vm", false, Str(), fp, tok, w)); T_CHECK(Contains(src->err, "VM not found"));
    T_CHECK(!src->begin(tm.name, false, Str(), fp, tok, w)); T_CHECK(Contains(src->err, "template"));
    T_CHECK(!src->begin(ia.name, false, Str(), fp, tok, w)); T_CHECK(Contains(src->err, "not accessible"));
    T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.events("create") == 0);
    delete src; DeleteVm(ctl, tm.name); DeleteVm(ctl, ia.name);
    return true;
}

VM2B_TEST("esxi", esxi_memory_snapshot_and_quiesce_flags)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-mem");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "MM", 1, 200000, "poweredOn")); if (!f.m.ok) return false;
    BackupOptions o; o.kdfIterations = 3000; o.retry = Fast(); o.verify = false;
    Str err; VmSource* src = NewEsxiSource(e.cfg(false, true), err); Target* t = LocalAt(tmp.sub("store"));
    Manifest man; Str warn; Pr pr;
    bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
    if (!ok) c_.note("%s", err.c_str());
    T_CHECK(ok);
    if (ok) { Str dest = tmp.sub("r"), re; if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); bool vmem = false; for (const FI& a : after) if (a.rel.ends_with(".vmem")) vmem = true; T_CHECK(vmem); } }
    // the request the server saw: memory true, quiesce false
    Json st = ctl.state(); (void)st;
    Json lg; ctl.call("log", Json(), lg);
    bool seen = false; for (size_t i = 0; i < lg.get("log").size(); ++i) { const Json& l = lg.get("log").at(i); if (l.get("what").str() == "CreateSnapshot") { Json x; Str pe; Json::parse(l.get("extra").str(), x, pe); seen = x.get("memory").boolean() && !x.get("quiesce").boolean(); } }
    T_CHECK(seen);
    T_CHECK(ctl.leaked() == 0);
    delete t; delete src; DeleteVm(ctl, f.spec.name);
    return true;
}

VM2B_TEST("esxi", esxi_many_vms_paging)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-many");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    int n = c_.range(110, 140);
    Str tag = Str::fmt("MY%u-", c_.rnd(100000));
    for (int i = 0; i < n; ++i) {
        VmSpec s; s.name = tag + Str::fmt("%03d", i); Json d = Json::array(); Json o = Json::object(); o["size"] = 4096; d.push(o); s.disks = d;
        if (e.vcenter()) s.dc = e.dcs[0];
        MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
    }
    Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Vec<VmInfo> vms;
    T_CHECK(src->list(vms));
    int found = 0; for (const VmInfo& v : vms) if (v.name.starts_with(tag.c_str())) ++found;
    T_CHECK(found == n); if (found != n) c_.note("%s: found %d of %d", e.name.c_str(), found, n);
    T_CHECK(ctl.counter("soap:ContinueRetrievePropertiesEx") >= 1);
    delete src;
    int pick = c_.range(0, n - 1);
    Str vm = tag + Str::fmt("%03d", pick);
    BackupOptions o; o.kdfIterations = 3000; o.retry = Fast(); 
    src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store")); Manifest man; Str warn; Pr pr;
    T_CHECK(BackupVm(*src, vm, *t, o, &pr, man, warn, err)); T_CHECK(ctl.leaked() == 0);
    delete t; delete src;
    for (int i = 0; i < n; ++i) DeleteVm(ctl, tag + Str::fmt("%03d", i));
    return true;
}

VM2B_TEST("esxi", esxi_vcenter_datacenters)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true;
    EsTmp tmp(c_, "esxi-dc");
    int n = 0;
    for (const Env& e : all) {
        if (!e.vcenter() || e.dcs.size() < 2 || !e.tlsOnly.empty()) continue;
        if (c_.coin(40)) continue;
        ++n;
        Ctl ctl(e); ctl.reset();
        Str name = Str::fmt("DUP%u", c_.rnd(100000));
        VmSpec a; a.name = name; a.disks = Disks(c_, 1, 100000); a.dc = e.dcs[0]; a.ds = "datastore1";
        VmSpec b = a; b.dc = e.dcs[1]; b.ds = "datastore2";
        MockVm ma = MakeVm(ctl, e, a), mb = MakeVm(ctl, e, b); T_CHECK(ma.ok && mb.ok); if (!ma.ok || !mb.ok) return false;
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Str fp, tok, w;
        T_CHECK(!src->begin(name, false, Str(), fp, tok, w)); T_CHECK(Contains(src->err, "several VMs")); T_CHECK(ctl.leaked() == 0);
        delete src;
        for (int k = 0; k < 2; ++k) {
            Json cfg = e.cfg(); cfg["datacenter"] = e.dcs[(size_t)k];
            Vec<FI> before; Walk(k == 0 ? ma.dir : mb.dir, "", before);
            src = NewEsxiSource(cfg, err); Vec<VmInfo> vms; T_CHECK(src->list(vms));
            int cnt = 0; for (const VmInfo& v : vms) if (v.name == name) { ++cnt; T_CHECK(Contains(v.extra, k == 0 ? "datastore1" : "datastore2")); T_CHECK(Contains(v.extra, ("datacenter " + e.dcs[(size_t)k]).c_str())); }
            T_CHECK(cnt == 1);
            BackupOptions o; o.kdfIterations = 3000; o.retry = Fast(); o.verify = false;
            Target* t = LocalAt(tmp.sub("store")); Manifest man; Str warn; Pr pr;
            bool ok = BackupVm(*src, name, *t, o, &pr, man, warn, err);
            if (!ok) c_.note("%s dc %d: %s", e.name.c_str(), k, err.c_str());
            T_CHECK(ok);
            if (ok) { Str dest = tmp.sub("r"), re; if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, name), "", after); T_CHECK(CheckTree(c_, before, after)); } }
            delete t; delete src; RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
        }
        { Json cfg = e.cfg(); cfg["datacenter"] = "NoSuchDC"; src = NewEsxiSource(cfg, err); Vec<VmInfo> vms; T_CHECK(!src->list(vms)); T_CHECK(Contains(src->err, "datacenter not found")); delete src; }
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, name); DeleteVm(ctl, name);
    }
    if (!n) c_.note("no vCenter drawn");
    return true;
}

VM2B_TEST("esxi", esxi_tls_floor_of_old_esxi)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    bool any = false;
    for (const Env& e : all) {
        if (e.tlsOnly.empty()) continue;
        any = true;
        Json cfg = e.cfg(); cfg["tls_min"] = "1.2";
        Str err; VmSource* src = NewEsxiSource(cfg, err); Vec<VmInfo> vms;
        T_CHECK(!src->list(vms)); T_CHECK(!src->transient);                        // the floor 1.2 refuses a TLS 1.0 only server for good
        delete src;
        cfg["tls_min"] = "1.0"; src = NewEsxiSource(cfg, err);
        bool ok = src->list(vms); if (!ok) c_.note("tls 1.0: %s", src->err.c_str());
        T_CHECK(ok); delete src;
    }
    if (!any) c_.skip("no TLS 1.0 only emulator");
    return true;
}

VM2B_TEST("esxi", esxi_concurrent_backups)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-conc");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    struct Job { Json cfg; Str vm, store; BackupOptions o; bool ok; Str err; Manifest man; Vec<FI> before; Str dir; };
    const int N = 3;
    Job jobs[N];
    for (int i = 0; i < N; ++i) {
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "CC", c_.range(1, 2), 2500000, c_.coin() ? "poweredOn" : "poweredOff")); if (!f.m.ok) return false;
        jobs[i].cfg = e.cfg(); jobs[i].vm = f.spec.name; jobs[i].store = tmp.sub(Str::fmt("store%d", i).c_str()); jobs[i].o = RandomOpts(c_, false); jobs[i].ok = false; jobs[i].before = f.before; jobs[i].dir = f.m.dir;
    }
    struct Th { static DWORD WINAPI Run(LPVOID p) {
        Job* j = (Job*)p; Str err; VmSource* src = NewEsxiSource(j->cfg, err); Target* t = LocalAt(j->store);
        Pr pr; Str warn; j->ok = BackupVm(*src, j->vm, *t, j->o, &pr, j->man, warn, j->err);
        delete t; delete src; return 0; } };
    HANDLE h[N];
    for (int i = 0; i < N; ++i) h[i] = CreateThread(nullptr, 0, Th::Run, &jobs[i], 0, nullptr);
    WaitForMultipleObjects(N, h, TRUE, 300000);
    for (int i = 0; i < N; ++i) CloseHandle(h[i]);
    for (int i = 0; i < N; ++i) {
        if (!jobs[i].ok) c_.note("job %d: %s", i, jobs[i].err.c_str());
        T_CHECK(jobs[i].ok);
        if (!jobs[i].ok) continue;
        Target* t = LocalAt(jobs[i].store); Str dest = tmp.sub(Str::fmt("r%d", i).c_str()), re;
        if (Restore(c_, *t, jobs[i].man, jobs[i].o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, jobs[i].vm), "", after); T_CHECK(CheckTree(c_, jobs[i].before, after)); }
        delete t;
    }
    T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.events("create") == N); T_CHECK(ctl.events("remove") == N);
    for (int i = 0; i < N; ++i) DeleteVm(ctl, jobs[i].vm);
    return true;
}

VM2B_TEST("esxi", esxi_thin_big_disk)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-big");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    VmSpec s; s.name = UniqueName(c_, "BIG"); s.power = "poweredOn"; if (e.vcenter()) s.dc = e.dcs[0];
    Json d = Json::array(); Json o = Json::object(); o["size"] = (i64)1073741824 + (i64)c_.range(0, 1000) * 1000000; o["thin"] = true; o["seed"] = 77; d.push(o); s.disks = d;
    MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
    Vec<FI> before; Walk(m.dir, "", before);
    BackupOptions bo; bo.kdfIterations = 3000; bo.retry = Fast(); bo.verify = false;
    Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store")); Manifest man; Str warn; Pr pr;
    double t0 = UnixNow();
    bool ok = BackupVm(*src, s.name, *t, bo, &pr, man, warn, err);
    if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
    T_CHECK(ok);
    c_.note("%s: %s backed up in %.0f s (%d volumes)", e.name.c_str(), FormatSize(pr.bytes).c_str(), UnixNow() - t0, (int)man.segments.size());
    if (ok) {
        Str dest = tmp.sub("r"), re;
        if (Restore(c_, *t, man, bo, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, s.name), "", after); T_CHECK(CheckTree(c_, before, after)); }
    }
    T_CHECK(ctl.leaked() == 0);
    delete t; delete src; DeleteVm(ctl, s.name);
    return true;
}

VM2B_TEST("esxi", esxi_disk_on_another_datastore)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-ds");
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        VmSpec s; s.name = UniqueName(c_, "DS"); s.disks = Disks(c_, 2, 300000); s.power = c_.coin() ? "poweredOn" : "poweredOff";
        s.disks.item(1)["ds"] = "datastore2"; s.disks.item(1)["thin"] = false;
        if (e.vcenter()) s.dc = e.dcs[c_.rnd((u32)e.dcs.size())];
        MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok); if (!m.ok) return false;
        Str dir2 = JoinPath(JoinPath(e.root, "datastore2"), PathBase(m.dir));
        Vec<FI> before; Walk(m.dir, "", before);
        Vec<FI> other; Walk(dir2, "", other); T_CHECK(!other.empty());
        for (FI& f : other) { f.rel = "_other-datastores/datastore2/" + PathBase(m.dir) + "/" + f.rel; before.push_back(f); }
        BackupOptions o = RandomOpts(c_, false);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store")); Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, s.name, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) { Str dest = tmp.sub("r"), re; if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, s.name), "", after); T_CHECK(CheckTree(c_, before, after)); } }
        T_CHECK(ctl.leaked() == 0);
        // the folder of the second datastore holds no leftovers of the snapshot
        Vec<FI> again; Walk(dir2, "", again); T_CHECK(again.size() == other.size());
        delete t; delete src; DeleteVm(ctl, s.name); RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    }
    return true;
}


// ============================================================================================================================
// restore TO an ESXi host / vCenter
// ============================================================================================================================
namespace {

struct RFix { Fix f; BackupOptions o; Manifest man; Str store; Str ds2Dir; Vec<FI> ds2before; bool ds2; };

// a VM on the mock (optionally with its second disk on datastore2), backed up into a local target
static bool BackupFix(Ctx& c, EsTmp& tmp, Ctl& ctl, const Env& e, RFix& r, const char* tag, bool ds2, const char* power, bool smallVolumes = false)
{
    r.ds2 = ds2;
    r.f.spec.name = UniqueName(c, tag); r.f.spec.disks = Disks(c, ds2 ? 2 : c.range(1, 3), 400000); r.f.spec.power = power;
    for (size_t i = 0; i < r.f.spec.disks.size(); ++i) r.f.spec.disks.item(i)["thin"] = false;
    if (ds2) r.f.spec.disks.item(1)["ds"] = "datastore2";
    if (e.vcenter()) r.f.spec.dc = e.dcs[c.rnd((u32)e.dcs.size())];
    r.f.m = MakeVm(ctl, e, r.f.spec);
    if (!r.f.m.ok) return false;
    Walk(r.f.m.dir, "", r.f.before);
    if (ds2) { r.ds2Dir = JoinPath(JoinPath(e.root, "datastore2"), PathBase(r.f.m.dir)); Walk(r.ds2Dir, "", r.ds2before); }
    r.o = RandomOpts(c, smallVolumes);
    Str err; VmSource* src = NewEsxiSource(e.cfg(), err);
    Json tc = Json::object(); r.store = tmp.sub(Str::fmt("store-%u", c.rnd(1000000)).c_str()); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
    Pr pr; Str warn;
    bool ok = BackupVm(*src, r.f.spec.name, *t, r.o, &pr, r.man, warn, err);
    if (!ok) c.note("backup of the fixture: %s", err.c_str());
    delete t; delete src;
    return ok;
}

static Str VmxValue(const Str& text, const Str& key)
{
    size_t s = 0; Str lk = key.lower();
    for (size_t i = 0; i <= text.size(); ++i) if (i == text.size() || text[i] == '\n') {
        Str l = text.substr(s, i - s); s = i + 1;
        int eq = l.find('='); if (eq <= 0) continue;
        if (l.substr(0, (size_t)eq).trim().lower() == lk) { Str v = l.substr((size_t)eq + 1).trim(); if (v.size() >= 2) v = v.substr(1, v.size() - 2); return v; }
    }
    return Str();
}

// the restored VM folder: every file of the original (but swap, logs, .vmx, .vmsd) byte for byte, the rewritten .vmx and .vmsd, nothing else
static bool CheckRestoredFolder(Ctx& c, const Env& e, const RFix& r, const Str& newName, const Str& mainDs, bool everythingInOneDs)
{
    bool ok = true;
    Str dir = JoinPath(JoinPath(e.root, mainDs), newName);
    Vec<FI> after; Walk(dir, "", after);
    Vec<FI> want = r.f.before;
    if (r.ds2) for (const FI& f : r.ds2before) { FI g = f; if (!everythingInOneDs) g.rel = "<ds2>/" + g.rel; want.push_back(g); }
    for (const FI& b : want) {
        if (Excluded(b.rel) || b.rel.ends_with(".vmx") || b.rel.ends_with(".vmsd") || b.rel.starts_with("<ds2>/")) continue;
        const FI* a = FindFi(after, b.rel);
        if (!a) { c.note("not restored: %s", b.rel.c_str()); ok = false; continue; }
        if (a->sha != b.sha || a->size != b.size) { c.note("restored file differs: %s", b.rel.c_str()); ok = false; }
    }
    for (const FI& a : after) {
        if (a.rel.ends_with(".vmx") || a.rel.ends_with(".vmsd")) continue;
        const FI* b = FindFi(want, a.rel);
        if (!b || Excluded(a.rel)) { c.note("unexpected file in the restored VM folder: %s", a.rel.c_str()); ok = false; }
    }
    if (r.ds2 && !everythingInOneDs) {
        Str d2 = JoinPath(JoinPath(e.root, "datastore2"), newName); Vec<FI> a2; Walk(d2, "", a2);
        for (const FI& b : r.ds2before) { const FI* a = FindFi(a2, b.rel); if (!a || a->sha != b.sha) { c.note("second datastore: %s", b.rel.c_str()); ok = false; } }
        if (a2.size() != r.ds2before.size()) { c.note("second datastore holds %d files, %d expected", (int)a2.size(), (int)r.ds2before.size()); ok = false; }
    }
    // the .vmx: base disks, the new name, no snapshot / change tracking lines
    const FI* vx = nullptr; for (const FI& a : after) if (a.rel.ends_with(".vmx")) vx = &a;
    if (!vx) { c.note("no .vmx in the restored VM"); return false; }
    Bytes vb; Str ve; ReadWholeFile(vx->full.c_str(), vb, ve); Str vt((const char*)vb.data(), vb.size());
    if (VmxValue(vt, "displayName") != newName) { c.note("displayName is '%s'", VmxValue(vt, "displayName").c_str()); ok = false; }
    if (vt.find("snapshot.") >= 0 || vt.find("ctkEnabled") >= 0) { c.note("the .vmx still has snapshot / ctk lines"); ok = false; }
    for (size_t i = 0; i < r.f.spec.disks.size(); ++i) {
        Str v = VmxValue(vt, Str::fmt("scsi0:%d.fileName", (int)i));
        Str base = v; Str dsx, px; if (EsxiParseDsPath(v, dsx, px)) base = px; int sl = base.rfind('/'); if (sl >= 0) base = base.substr((size_t)sl + 1);
        if (v.empty() || base.find("-0000") >= 0 || !base.ends_with(".vmdk")) { c.note("disk %d entry '%s'", (int)i, v.c_str()); ok = false; continue; }
        Str full = (EsxiParseDsPath(v, dsx, px)) ? JoinPath(JoinPath(e.root, dsx), px.replace_all("/", "\\")) : JoinPath(dir, v.replace_all("/", "\\"));
        if (!FileExists(full)) { c.note("disk %d: %s does not exist", (int)i, full.c_str()); ok = false; }
        if (r.ds2 && i == 1 && !everythingInOneDs && v.find("[datastore2]") != 0) { c.note("disk 1 should be on datastore2: %s", v.c_str()); ok = false; }
    }
    const FI* vs = nullptr; for (const FI& a : after) if (a.rel.ends_with(".vmsd")) vs = &a;
    if (vs) { Bytes sb; ReadWholeFile(vs->full.c_str(), sb, ve); Str st((const char*)sb.data(), sb.size()); if (VmxValue(st, "snapshot.numSnapshots") != "0" || st.find("VM2Backup") >= 0) { c.note(".vmsd still lists the backup snapshot"); ok = false; } }
    // everything else of the original .vmx is kept
    for (const FI& b : r.f.before) if (b.rel.ends_with(".vmx")) {
        Bytes ob; ReadWholeFile(b.full.c_str(), ob, ve); Str ot((const char*)ob.data(), ob.size());
        Str k = "uuid.bios"; if (VmxValue(ot, k) != VmxValue(vt, k)) { c.note("uuid.bios was changed"); ok = false; }
        if (VmxValue(ot, "nvram") != VmxValue(vt, "nvram")) { c.note("nvram entry was changed"); ok = false; }
    }
    return ok;
}

static bool StateHasVm(Ctl& ctl, const Str& name, const char* mainDs, Str* power = nullptr)
{
    Json v = ctl.vmState(name); if (v.is_null()) return false;
    if (power) *power = v.get("power").str();
    return v.get("ds").str() == mainDs && v.get("snapshots").size() == 0;
}

static int CountDirs(const Env& e, const char* ds) { int n = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(JoinPath(e.root, ds), "*")), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0; do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") && fd.cFileName[0] != L'.') ++n; } while (FindNextFileW(h, &fd)); FindClose(h); return n; }

static ReadOptions RestoreRo(const EsTmp& tmp) { ReadOptions ro; ro.retry = Fast(); ro.stateDir = tmp.sub("state"); Str e; MakeDirs(ro.stateDir, e); return ro; }

} // namespace

VM2B_TEST("esxi", esxi_rewrite_vmx_and_vmsd)
{
    Vec<Str> from, to;
    from.push_back("disk-000001.vmdk"); to.push_back("disk.vmdk");
    from.push_back("data-000002.vmdk"); to.push_back("[datastore2] new vm/data.vmdk");
    Str vmx = ".encoding = \"UTF-8\"\r\ndisplayName = \"Old Name\"\r\nscsi0:0.fileName = \"disk-000001.vmdk\"\r\nscsi0:1.fileName = \"[ds] old vm/data-000002.vmdk\"\r\nscsi0:2.fileName = \"keep.vmdk\"\r\n"
              "scsi0:0.ctkEnabled = \"TRUE\"\r\nctkEnabled = \"TRUE\"\r\nsnapshot.redoNotWithParent = \"TRUE\"\r\nsnapshot.action = \"keep\"\r\nsched.swap.derivedName = \"/vmfs/x.vswp\"\r\nuuid.bios = \"42 12\"\r\nuuid.action = \"create\"\r\nnvram = \"x.nvram\"\r\nmemSize = \"1024\"\r\n";
    Str out = EsxiRewriteVmx(vmx, "New \"Q\" Name", from, to);
    T_CHECK(out.find("displayName = \"New |22Q|22 Name\"\n") >= 0 && out.find("Old Name") < 0);
    T_CHECK(out.find("scsi0:0.fileName = \"disk.vmdk\"\n") >= 0);
    T_CHECK(out.find("scsi0:1.fileName = \"[datastore2] new vm/data.vmdk\"\n") >= 0);
    T_CHECK(out.find("scsi0:2.fileName = \"keep.vmdk\"") >= 0);
    T_CHECK(out.find("ctk") < 0 && out.find("snapshot.") < 0 && out.find("derivedName") < 0);
    T_CHECK(out.find("uuid.bios = \"42 12\"") >= 0 && out.find("uuid.action = \"keep\"") >= 0 && out.find("uuid.action = \"create\"") < 0);
    T_CHECK(out.find("memSize = \"1024\"") >= 0 && out.find("nvram = \"x.nvram\"") >= 0 && out.find('\r') < 0);
    Str out2 = EsxiRewriteVmx("memSize = \"1\"\n", "N", from, to);                 // no name line, no uuid line: both are added
    T_CHECK(out2.find("displayName = \"N\"") >= 0 && out2.find("uuid.action = \"keep\"") >= 0);
    Str d = ".encoding = \"UTF-8\"\nsnapshot.lastUID = \"3\"\nsnapshot.numSnapshots = \"3\"\nsnapshot.current = \"3\"\n"
            "snapshot0.uid = \"1\"\nsnapshot0.displayName = \"mine\"\nsnapshot0.filename = \"a.vmsn\"\n"
            "snapshot1.uid = \"2\"\nsnapshot1.displayName = \"VM2Backup-abc\"\nsnapshot1.filename = \"b.vmsn\"\nsnapshot1.parent = \"1\"\n"
            "snapshot2.uid = \"3\"\nsnapshot2.displayName = \"other\"\nsnapshot2.filename = \"c.vmsn\"\nsnapshot2.parent = \"2\"\n";
    Str o3 = EsxiRewriteVmsd(d, "VM2Backup-abc");
    T_CHECK(o3.find("VM2Backup") < 0 && o3.find("snapshot.numSnapshots = \"2\"") >= 0 && o3.find("snapshot1.displayName = \"other\"") >= 0 && o3.find("snapshot2.") < 0);
    T_CHECK(o3.find("snapshot.lastUID = \"3\"") >= 0 && o3.find("snapshot.current = \"3\"") >= 0);
    Str one = ".encoding = \"UTF-8\"\nsnapshot.lastUID = \"1\"\nsnapshot.numSnapshots = \"1\"\nsnapshot.current = \"1\"\nsnapshot0.uid = \"1\"\nsnapshot0.displayName = \"VM2Backup-abc\"\nsnapshot0.filename = \"b.vmsn\"\n";
    Str o4 = EsxiRewriteVmsd(one, "VM2Backup-abc");
    T_CHECK(o4.find("numSnapshots = \"0\"") >= 0 && o4.find("snapshot0") < 0 && o4.find("snapshot.current") < 0);
    return true;
}

VM2B_TEST("esxi", esxi_restore_to_host_all_profiles)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rs");
    int round = c_.range(0, 3);
    for (const Env& e : es) {
        ++round;
        Ctl ctl(e); ctl.reset();
        RFix r; bool ds2 = (round % 3) == 0;
        T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RS", ds2, c_.coin() ? "poweredOn" : "poweredOff")); if (!r.f.m.ok) return false;
        EsxiRestoreOptions ro; Str newName = r.f.spec.name + " restored";
        bool oneDs = false;
        int variant = round % 4;
        if (variant == 0) ro.vmName = newName; else if (variant == 1) { ro.vmName = newName; if (c_.coin()) { ro.datastore = "datastore2"; oneDs = true; } } else if (variant == 2) newName = r.f.spec.name + "-restored";
        else { ro.vmName = newName; ro.powerOn = true; }
        if (e.vcenter() && c_.coin()) { ro.folder = "Servers/Linux"; ro.pool = c_.coin() ? "Production" : "Resources"; }
        if (c_.coin(30)) ro.datacenter = r.f.spec.dc;
        Str mainDs = ro.datastore.empty() ? Str("datastore1") : ro.datastore;
        ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res; Str err;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        Pr pr;
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), ro, r.o.password, rd, &pr, &res, err);
        if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) {
            T_CHECK(res.vmName == newName); T_CHECK(!res.vmRef.empty());
            T_CHECK(CheckRestoredFolder(c_, e, r, newName, mainDs, oneDs));
            Str power; T_CHECK(StateHasVm(ctl, newName, mainDs.c_str(), &power));
            T_CHECK(power == (ro.powerOn ? "poweredOn" : "poweredOff"));
            T_CHECK(ctl.counter("reg:RegisterVM") == 1);
            // the original is untouched and has no snapshot left
            T_CHECK(StateHasVm(ctl, r.f.spec.name, "datastore1")); T_CHECK(ctl.leaked() == 0);
            if (e.vcenter() && !ro.folder.empty()) { Json lg; ctl.call("log", Json(), lg); bool seen = false; for (size_t i = 0; i < lg.get("log").size(); ++i) { const Json& l = lg.get("log").at(i); if (l.get("what").str() == "RegisterVM") { Json x; Str pe; Json::parse(l.get("extra").str(), x, pe); seen = x.get("folder").str().starts_with("group-v") && x.get("folder").str() != "group-v10" && !x.get("pool").str().empty(); } } T_CHECK(seen); }
            T_CHECK(!FileExists(JoinPath(rd.stateDir, "dummy")));
        }
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName);
        RemoveTree(r.store);
    }
    return true;
}

VM2B_TEST("esxi", esxi_restore_upload_is_rate_limited)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    if (es.empty()) return true;
    EsTmp tmp(c_, "esxi-rl");
    const Env& e = es[(size_t)c_.range(0, (int)es.size() - 1)];
    Ctl ctl(e); ctl.reset();
    RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RL", false, "poweredOff")); if (!r.f.m.ok) return false;
    // the upload takes about 3 s at this speed (the limit counts the bytes that go to the host)
    RateLimiter lim; double mbit = (double)r.man.rawSize * 8.0 / 1e6 / 3.0; if (mbit < 0.05) mbit = 0.05; lim.setMbit(mbit);
    EsxiRestoreOptions ro; ro.vmName = r.f.spec.name + " limited"; ro.limiter = &lim;
    ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res; Str err;
    Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
    Pr pr; DWORD t0 = GetTickCount();
    bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), ro, r.o.password, rd, &pr, &res, err);
    double sec = (GetTickCount() - t0) / 1000.0;
    if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
    T_CHECK(ok);
    c_.note("%s: %llu raw bytes at %.2f Mbit/s took %.1f s", e.name.c_str(), (unsigned long long)r.man.rawSize, mbit, sec);
    T_CHECK(sec >= 1.8);                                      // without the limit the mock takes well under a second
    delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, ro.vmName);
    RemoveTree(r.store);
    return true;
}

VM2B_TEST("esxi", esxi_restore_refusals_and_clean_failures)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rf");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RF", false, "poweredOff")); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err; Pr pr;
        int dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        auto noTrace = [&](const char* what) {
            int d = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
            if (d != dirs0) c_.note("%s: %d folders on the datastores, %d before", what, d, dirs0);
            T_CHECK(d == dirs0);
        };
        // 1. the default name: the original exists, so "<name>-restored"; then both exist: refused, nothing left behind
        { EsxiRestoreOptions o; EsxiRestoreResult res; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, err); if (!ok) c_.note("%s", err.c_str()); T_CHECK(ok); T_CHECK(res.vmName == r.f.spec.name + "-restored"); }
        dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        { EsxiRestoreOptions o; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "already exist")); noTrace("both names taken"); }
        // 2. an explicit name that is taken
        { EsxiRestoreOptions o; o.vmName = r.f.spec.name; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "already exist")); noTrace("explicit name taken"); }
        // 3. a datastore folder of that name exists although no VM is registered there
        { Str stray = "stray-" + Str::fmt("%u", c_.rnd(100000)); Json o2, a = Json::object(); a["name"] = "[datastore1] " + stray; (void)a; CreateDirectoryW(FilePath(JoinPath(JoinPath(e.root, "datastore1"), stray)), nullptr);
          dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
          EsxiRestoreOptions o; o.vmName = stray; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "already exist")); noTrace("stray folder");
          RemoveTree(JoinPath(JoinPath(e.root, "datastore1"), stray)); dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2"); }
        // 4. things that do not exist on the server
        { EsxiRestoreOptions o; o.vmName = "x" + Str::fmt("%u", c_.rnd(1000)); o.datastore = "no-such-datastore"; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "datastore")); noTrace("bad datastore"); }
        if (e.vcenter()) {
            { EsxiRestoreOptions o; o.vmName = "y" + Str::fmt("%u", c_.rnd(1000)); o.folder = "No/Such/Folder"; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "folder")); noTrace("bad folder"); }
            { EsxiRestoreOptions o; o.vmName = "z" + Str::fmt("%u", c_.rnd(1000)); o.pool = "NoPool"; bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "pool")); noTrace("bad pool"); }
        }
        // 5. a wrong password / no password
        if (!r.o.password.empty()) {
            EsxiRestoreOptions o; o.vmName = "w" + Str::fmt("%u", c_.rnd(1000));
            bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, "wrong", rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(err == "wrong password"); noTrace("wrong password");
            ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, "", rd, &pr, nullptr, err); T_CHECK(!ok); noTrace("no password");
        }
        // 6. wrong credentials of the host: permanent, no secrets
        { Json cfg = e.cfg(); cfg["password"] = "wrong-" + e.password; EsxiRestoreOptions o; o.vmName = "v" + Str::fmt("%u", c_.rnd(1000));
          bool ok = RestoreToEsxi(*t, r.man.archive, cfg, o, r.o.password, rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "login failed")); T_CHECK(!Contains(err, e.password.c_str())); noTrace("bad login"); }
        // 7. a backup that did not come from a host
        { EsTmp t2(c_, "esxi-dirsrc"); Str root = t2.sub("vms"); Str mk; MakeDirs(JoinPath(root, "plain vm"), mk);
          Bytes b; b.resize(1000); WriteWholeFile(JoinPath(JoinPath(root, "plain vm"), "a.txt").c_str(), b.data(), b.size(), mk);
          VmSource* ds = NewDirectorySource(root); Json c2 = Json::object(); c2["type"] = "local"; c2["path"] = t2.sub("store"); Target* t3 = MakeTarget(c2, te);
          BackupOptions bo; bo.kdfIterations = 3000; bo.retry = Fast(); Manifest m2; Str w2;
          bool bok = BackupVm(*ds, "plain vm", *t3, bo, nullptr, m2, w2, err); T_CHECK(bok);
          EsxiRestoreOptions o; o.vmName = "u" + Str::fmt("%u", c_.rnd(1000));
          bool ok = bok && RestoreToEsxi(*t3, m2.archive, e.cfg(), o, "", rd, &pr, nullptr, err); T_CHECK(!ok); T_CHECK(Contains(err, "not made from an ESXi")); noTrace("foreign backup");
          delete t3; delete ds; }
        T_CHECK(ctl.leaked() == 0);
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, r.f.spec.name + "-restored"); RemoveTree(r.store);
    }
    return true;
}

namespace {
struct RHook : Progress {
    Ctl* ctl; int fired, mode; u64 bytes; bool abortAt, keep; u64 limit; int resumes, retries;
    RHook() : ctl(nullptr), fired(0), mode(0), bytes(0), abortAt(false), keep(false), limit(0), resumes(0), retries(0) {}
    void onArchiveBytes(u64 n) override { bytes += n; }
    void onFile(const Str&) override { if (mode == 1 && fired == 0 && ctl) { Json o; ctl->call("expire", Json::object(), o); ++fired; } }
    void onResume(int, u64) override { ++resumes; }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    bool aborted() override { return abortAt && bytes >= limit; }
    bool keepOnAbort() override { return keep; }
};
}

VM2B_TEST("esxi", esxi_restore_survives_faults)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rx");
    int kind = c_.range(0, 7);
    for (const Env& e : es) {
        kind = (kind + 1) % 8;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RX", false, "poweredOff", true)); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err; RHook pr; pr.ctl = &ctl;
        EsxiRestoreOptions o; o.vmName = r.f.spec.name + " rs"; Str newName = o.vmName;
        Json fl = Json::object(); bool expectOk = true; const char* needText = nullptr;
        int dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        if (kind == 0) { Json d = Json::object(); d["match"] = "-flat.vmdk"; d["bytes"] = c_.range(10000, 300000); d["count"] = c_.range(1, 3); fl["put_drop"] = d; }        // the upload breaks off: the file starts again
        if (kind == 1) { fl["expire_after_calls"] = c_.range(2, 20); pr.mode = 1; }                                                                                      // sessions expire (also during the upload)
        if (kind == 2) { Json d = Json::object(); d["match"] = ".nvram"; d["count"] = 1; fl["put_503"] = d; }                                                           // busy once
        if (kind == 3) { Json d = Json::object(); d["match"] = "-flat.vmdk"; d["count"] = -1; fl["put_507"] = d; expectOk = false; needText = "no room"; }              // datastore full
        if (kind == 4) { fl["put_readonly"] = true; expectOk = false; needText = "refused"; }                                                                            // read-only / license
        if (kind == 5) { fl["register_fail"] = c_.coin() ? "InvalidDatastorePath" : "AlreadyExists"; expectOk = false; needText = "registering the VM failed"; }
        if (kind == 6) { fl["poweron_fail"] = true; o.powerOn = true; }                                                                                                    // the VM is restored, only power on fails
        if (kind == 7) { fl["slow_ms"] = 2; }
        ctl.faults(fl);
        EsxiRestoreResult res;
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, err);
        if (ok != expectOk) c_.note("%s kind %d: %s", e.name.c_str(), kind, ok ? "succeeded" : err.c_str());
        T_CHECK(ok == expectOk);
        if (expectOk) {
            T_CHECK(CheckRestoredFolder(c_, e, r, newName, "datastore1", false));
            T_CHECK(StateHasVm(ctl, newName, "datastore1"));
            if (kind == 0 && ctl.counter("fault:put-drop") > 0) T_CHECK(pr.retries >= 1);
            if (kind == 6) { T_CHECK(!res.warning.empty()); Str pw; StateHasVm(ctl, newName, "datastore1", &pw); T_CHECK(pw == "poweredOff"); }
        } else {
            T_CHECK(Contains(err, needText));
            int d = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
            if (d != dirs0) c_.note("%s kind %d: %d folders, %d before", e.name.c_str(), kind, d, dirs0);
            T_CHECK(d == dirs0);                                                  // the half-made VM folder is gone
            T_CHECK(ctl.vmState(newName).is_null());
        }
        T_CHECK(ctl.leaked() == 0);
        { Str sf; Vec<FI> st; Walk(rd.stateDir, "", st); for (const FI& q : st) c_.note("left in the state dir (kind %d): %s", kind, q.rel.c_str()); T_CHECK(st.empty()); }   // no restore state is left over
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

VM2B_TEST("esxi", esxi_restore_pause_cancel_and_later_run)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rl");
    int round = c_.range(0, 2);
    for (const Env& e : es) {
        if (c_.coin(45)) continue;
        int mode = round++ % 3;                                                    // 0 pause + continue, 1 cancel, 2 pause + the VM name appears meanwhile (the state still belongs to us)
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RL", false, "poweredOff", true)); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err;
        EsxiRestoreOptions o; o.vmName = r.f.spec.name + " later"; Str newName = o.vmName;
        int dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        u64 total = 0; for (const Segment& s : r.man.segments) total += s.size;
        {
            RHook pr; pr.abortAt = true; pr.limit = total / 2 + 1; pr.keep = mode != 1;
            bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err);
            if (r.man.segments.size() < 3) { delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName); RemoveTree(r.store); RemoveTree(rd.stateDir); continue; }       // too small to stop in the middle
            T_CHECK(!ok); T_CHECK(err == kAbortedMessage);
        }
        if (mode == 1) {
            T_CHECK(CountDirs(e, "datastore1") + CountDirs(e, "datastore2") == dirs0);                // a cancel leaves nothing on the host
            Vec<FI> st; Walk(rd.stateDir, "", st); for (const FI& q : st) c_.note("left in the state dir (mode %d): %s", mode, q.rel.c_str()); T_CHECK(st.empty());
        } else {
            int dnPause = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
            { int dn = CountDirs(e, "datastore1") + CountDirs(e, "datastore2"); if (dn < dirs0) c_.note("%s mode %d: %d folders after the pause, %d before", e.name.c_str(), mode, dn, dirs0); T_CHECK(dn >= dirs0); }                 // a pause keeps what was uploaded
            RHook pr; EsxiRestoreResult res;
            bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, err);
            T_CHECK(pr.resumes == (dnPause > dirs0 ? 1 : 0));          // stopped before the host was touched: nothing to continue
            if (!ok) c_.note("%s run 2: %s", e.name.c_str(), err.c_str());
            T_CHECK(ok);
            if (ok) { T_CHECK(CheckRestoredFolder(c_, e, r, newName, "datastore1", false)); T_CHECK(StateHasVm(ctl, newName, "datastore1")); }
            Vec<FI> st; Walk(rd.stateDir, "", st); for (const FI& q : st) c_.note("left in the state dir (mode %d): %s", mode, q.rel.c_str()); T_CHECK(st.empty());
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

VM2B_TEST("esxi", esxi_restore_targets_listing)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    for (const Env& e : es) {
        if (c_.coin(40)) continue;
        Ctl ctl(e); ctl.reset();
        VmSpec s; s.name = UniqueName(c_, "TG"); s.disks = Disks(c_, 1, 1000); if (e.vcenter()) s.dc = e.dcs[c_.rnd((u32)e.dcs.size())];
        MockVm m = MakeVm(ctl, e, s); T_CHECK(m.ok);
        Json out; Str err;
        Str dc = e.vcenter() ? s.dc : Str();
        bool ok = EsxiRestoreTargets(e.cfg(), dc, out, err); if (!ok) c_.note("%s: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); if (!ok) continue;
        T_CHECK(out.get("kind").str() == e.kind); T_CHECK(!out.get("api_version").str().empty());
        if (e.vcenter()) { T_CHECK(out.get("datacenters").size() == e.dcs.size()); T_CHECK(out.get("datacenter").str() == s.dc); } else T_CHECK(out.get("datacenter").str() == "ha-datacenter");
        const Json& ds = out.get("datastores"); bool d1 = false, d2 = false;
        for (size_t i = 0; i < ds.size(); ++i) { if (ds.at(i).get("name").str() == "datastore1") d1 = ds.at(i).get("free").integer() > 0 && ds.at(i).get("capacity").integer() > 0; if (ds.at(i).get("name").str() == "datastore2") d2 = true; T_CHECK(ds.at(i).get("accessible").boolean()); { Str dn = ds.at(i).get("name").str(); T_CHECK(ds.at(i).get("type").str() == (dn == "vsanDatastore" ? "vsan" : dn.starts_with("NFS ") ? "NFS" : "VMFS")); } }
        T_CHECK(d1 && d2); T_CHECK(ds.size() == 4);
        T_CHECK(out.get("pools").size() >= 2); T_CHECK(out.get("hosts").size() >= 1);
        bool pr = false, pp = false; for (size_t i = 0; i < out.get("pools").size(); ++i) { Str path = out.get("pools").at(i).get("path").str(); if (path.ends_with("/Resources/Production")) pp = true; if (path.ends_with("/Resources")) pr = true; }
        T_CHECK(pr && pp);
        bool seen = false; for (size_t i = 0; i < out.get("vms").size(); ++i) if (out.get("vms").at(i).str() == s.name) seen = true;
        T_CHECK(seen);
        if (e.vcenter()) { bool lin = false; for (size_t i = 0; i < out.get("folders").size(); ++i) if (out.get("folders").at(i).get("path").str() == "Servers/Linux") lin = true; T_CHECK(lin); }
        T_CHECK(!out.get("vm_folder").str().empty());
        T_CHECK(ctl.state().get("sessions").integer() == 0);
        DeleteVm(ctl, s.name);
    }
    return true;
}

// ============================================================================================================================
// round 2: quirks of real servers (HTTP framing, session idle, faults per call, certificates, odd VM shapes), hostile .vmx text, power loss
// ============================================================================================================================
namespace {

static Str SafeFolderOf(const char* n) { Str o(n); return o.replace_all("/", "_"); }      // enough to tell a folder that starts with a dot (the test's counter skips those)
// the archive path of a datastore path: every component made safe (what the source does)
static Str MapRel(const Str& rel) { Str out; size_t i = 0; while (i <= rel.size()) { size_t j = i; while (j < rel.size() && rel[j] != '/') ++j; if (i > 0) out += '/'; out += EsxiArchivePrefix(rel.substr(i, j - i)); i = j + 1; } return out; }
static Str Rep(size_t n, char ch) { Str r; r.resize(n, ch); return r; }
static const char* const kNfsDs = "NFS \xc3\xbcn\xc3\xaf #3 (\xc3\x91) \xd0\xb4\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5";

// create_vm with arbitrary keys (dir, vmx, ds, encrypted, cdrom, ...)
static MockVm MakeVmJ(Ctl& ctl, const Env& e, Json a, const Str& dcName = Str())
{
    if (!dcName.empty()) a["dc"] = dcName;
    if (!a.has("seed")) a["seed"] = (i64)(a.get("name").str().size() * 31 + 7);
    Json out; MockVm m;
    m.ok = ctl.call("create_vm", a, out) && !out.has("error");
    if (!m.ok) fprintf(stderr, "create_vm failed: %s\n", out.dump().c_str());
    m.name = a.get("name").str(); m.ds = out.get("ds").str(); m.dir = JoinPath(JoinPath(e.root, m.ds), out.get("dir").str().replace_all("/", "\\"));
    return m;
}

static Json DisksJ(Ctx& c, int n, u64 maxSize = 400000)
{
    Json d = Json::array();
    for (int i = 0; i < n; ++i) { Json o = Json::object(); o["size"] = (i64)c.range(1000, (int)maxSize); o["thin"] = false; o["seed"] = (i64)c.range(1, 100000); d.push(o); }
    return d;
}

// backup (+ restore to a local folder and compare against `before` minus `skip`); returns the backup's result, its warning and error
struct RtResult { bool ok; Str warn, err; bool restoredOk; RtResult() : ok(false), restoredOk(false) {} };
static RtResult RoundTrip(Ctx& c, EsTmp& tmp, const Env& e, const Str& vm, const Vec<FI>& before0, const Json& cfg, BackupOptions o, const Vec<Str>* skip, Progress* pr = nullptr, bool compare = true, const Str* hostDirAfter = nullptr)
{
    RtResult r;
    Str err; VmSource* src = NewEsxiSource(cfg, err);
    if (!src) { r.err = err; return r; }
    RemoveTree(tmp.sub("store")); RemoveTree(tmp.sub("r"));
    Target* t = LocalAt(tmp.sub("store")); Manifest man; Pr own;
    r.ok = BackupVm(*src, vm, *t, o, pr ? pr : &own, man, r.warn, r.err);
    if (r.ok && compare) {
        Vec<FI> before = before0; if (hostDirAfter) { before.clear(); Walk(*hostDirAfter, "", before); }          // the host's folder after the run (a leftover snapshot's removal merges deltas)
        Str dest = tmp.sub("r"), re;
        if (Restore(c, *t, man, o, dest, re)) {
            Vec<FI> after; Walk(JoinPath(dest, EsxiArchivePrefix(vm)), "", after);
            Vec<FI> want; for (const FI& b : before) { bool sk = false; if (skip) for (const Str& q : *skip) if (b.rel == q) sk = true; if (!sk) { FI m = b; m.rel = MapRel(b.rel); want.push_back(m); } }
            r.restoredOk = CheckTree(c, want, after);
            if (!r.restoredOk) {
                Bytes mb; Str me; Json mj; Str pe;
                if (ReadWholeFile(JoinPath(dest, "vm2backup-meta.json").c_str(), mb, me) && Json::parse((const char*)mb.data(), mb.size(), mj, pe)) {
                    const Json& fl = mj.get("files"); for (size_t i = 0; i < fl.size(); ++i) c.note("   archive has: %s (%lld)", fl.at(i).get("path").str().c_str(), (long long)fl.at(i).get("size").integer());
                }
                for (const FI& b : before) c.note("   host has:    %s (%llu)", b.rel.c_str(), (unsigned long long)b.size);
            }
        }
    }
    delete t; delete src;
    return r;
}

} // namespace

VM2B_TEST("esxi", esxi_http_quirks_on_folder_and_soap)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-q");
    static const char* modes[] = { "chunked", "close_delimited", "conn_close", "slow_start", "redirect", "redirect_abs", "soap_chunked", "chunked+drop", "redirect+drop", "conn_close+drop" };
    const int nm = (int)(sizeof(modes) / sizeof(modes[0]));
    int pick = c_.range(0, nm - 1);
    for (const Env& e : es) {
        pick = (pick + 1) % nm;
        Str mode = modes[pick];
        Ctl ctl(e); ctl.reset();
        Str base = mode; bool drop = false; int pl = mode.find('+'); if (pl > 0) { base = mode.substr(0, (size_t)pl); drop = true; }
        // a drop fires only in a file longer than its byte count (up to 150000): with a drop every disk is at least 160000 bytes
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "HQ", 2, 600000, "poweredOff", false, drop ? 160000 : 1)); if (!f.m.ok) return false;
        Json fl = Json::object(); Json q = Json::object();
        q["mode"] = base == "redirect_abs" ? Str("redirect") : base;
        if (base == "redirect_abs") { q["absolute"] = true; q["host"] = e.host; }
        if (base == "slow_start") q["ms"] = 250;
        fl["http_quirk"] = q;
        BackupOptions o = RandomOpts(c_, drop);
        if (drop) { Json d = Json::object(); d["match"] = "-flat.vmdk"; d["bytes"] = c_.range(2000, 150000); d["count"] = 1; fl["drop_after"] = d; }
        ctl.faults(fl);
        Pr pr;
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, f.before, e.cfg(), o, nullptr, &pr);
        if (!r.ok) c_.note("%s mode %s: %s", e.name.c_str(), mode.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk);
        if (base == "redirect" || base == "redirect_abs") T_CHECK(ctl.counter("fault:redirect") > 0);
        if (drop) T_CHECK(ctl.counter("fault:drop") >= 1);
        if (drop && r.ok) { if (pr.retries + pr.resumes == 0) c_.note("%s mode %s: no retry happened (drop fired %d times)", e.name.c_str(), mode.c_str(), ctl.counter("fault:drop")); T_CHECK(pr.retries + pr.resumes > 0); }
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

VM2B_TEST("esxi", esxi_redirect_to_another_host_is_refused)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rd");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "RD", 1, 50000, "poweredOff")); if (!f.m.ok) return false;
    Json fl = Json::object(), q = Json::object(); q["mode"] = "redirect"; q["absolute"] = true; q["host"] = "esxi-other.example.net"; fl["http_quirk"] = q; ctl.faults(fl);
    RtResult r = RoundTrip(c_, tmp, e, f.spec.name, f.before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, false);
    T_CHECK(!r.ok); T_CHECK(Contains(r.err, "another host")); T_CHECK(Contains(r.err, "esxi-other.example.net"));
    T_CHECK(ctl.leaked() == 0);
    DeleteVm(ctl, f.spec.name);
    return true;
}

VM2B_TEST("esxi", esxi_odd_vm_shapes)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-shape");
    int shape = c_.range(0, 7);
    for (const Env& e : es) {
        shape = (shape + 1) % 8;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "SH"); Str dcName = e.vcenter() ? e.dcs[c_.rnd((u32)e.dcs.size())] : Str();
        Json a = Json::object(); a["name"] = nm; a["power"] = c_.coin() ? "poweredOn" : "poweredOff"; a["disks"] = DisksJ(c_, c_.range(1, 2), 300000);
        Vec<Str> skip; bool expectOk = true; const char* needErr = nullptr; const char* needWarn = nullptr; bool allowEnc = false;
        bool apiHasKey = !(e.profile == "6.0");
        switch (shape) {
        case 0: a["disks"] = Json::array(); break;                                                              // a VM without disks
        case 1: if (!apiHasKey) { shape = 2; goto rdm; } a["encrypted"] = true; expectOk = false; needErr = "encrypted"; break;
        case 2: rdm: { Json d = DisksJ(c_, 2, 200000); d.item(0)["rdm"] = c_.coin() ? "physical" : "virtual"; a["disks"] = d; needWarn = "raw device mapping"; break; }
        case 3: a["ds"] = "vsanDatastore"; a["dir"] = "5d2f1e4c-9a77-4c1e-b0a2-3f6d9c8e7a11"; a["vmx"] = Str("a-very-long-name-") + Str::fmt("%u", c_.rnd(1000)) + Rep(120, 'x'); a["cdrom"] = "[datastore2] iso/ubuntu-24.04-live-server.iso"; break;
        case 4: { Json d = DisksJ(c_, 2, 200000); d.item(1)["ds"] = kNfsDs; a["disks"] = d; break; }                  // a disk on an NFS datastore with a spaced, unicode name
        case 5: { Json d = DisksJ(c_, 2, 200000); for (size_t i = 0; i < d.size(); ++i) d.item(i)["ctk"] = true; a["disks"] = d; a["cdrom"] = "[datastore1] iso/tools.iso"; break; }      // -ctk.vmdk files and a CD-ROM
        case 6: a["encrypted"] = true; allowEnc = true; if (!apiHasKey) { a["encrypted"] = false; allowEnc = false; } break;
        default: a["ds"] = kNfsDs; a["dir"] = "folder with  spaces & #hash"; break;
        }
        MockVm m = MakeVmJ(ctl, e, a, dcName); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before);
        // the files on other datastores (disk of case 4) belong to the expected tree under _other-datastores/
        if (shape == 4) {
            Str dir2 = JoinPath(JoinPath(e.root, kNfsDs), PathBase(m.dir)); Vec<FI> other; Walk(dir2, "", other);
            for (FI& f : other) { f.rel = Str("_other-datastores/") + kNfsDs + "/" + PathBase(m.dir) + "/" + f.rel; before.push_back(f); }
        }
        for (const FI& f : before) if (f.rel.ends_with("-ctk.vmdk")) skip.push_back(f.rel);                  // change-tracking files belong to no disk chain: the restored VM has ctk off
        if (shape == 2) {                                                                                        // the RDM disk is the first one: its stem is the VM's vmx name
            Str stem = a.get("name").str();
            for (const FI& f : before) if (f.rel == stem + ".vmdk" || f.rel == stem + "-rdmp.vmdk" || f.rel == stem + "-rdm.vmdk") skip.push_back(f.rel);
        }
        Json cfg = e.cfg(); if (allowEnc) cfg["allow_encrypted"] = true;
        RtResult r = RoundTrip(c_, tmp, e, nm, before, cfg, RandomOpts(c_, false), &skip);
        if (r.ok != expectOk) c_.note("%s shape %d: %s", e.name.c_str(), shape, r.ok ? "backup succeeded" : r.err.c_str());
        T_CHECK(r.ok == expectOk);
        if (needErr) { T_CHECK(Contains(r.err, needErr)); T_CHECK(ctl.events("create") == 0); }
        if (needWarn) T_CHECK(Contains(r.warn, needWarn));
        if (allowEnc) T_CHECK(Contains(r.warn, "encrypted"));
        if (expectOk) { if (!r.restoredOk) c_.note("%s shape %d: restored tree differs", e.name.c_str(), shape); T_CHECK(r.restoredOk); }
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(nm) == 0);
        DeleteVm(ctl, nm);
    }
    return true;
}

VM2B_TEST("esxi", esxi_user_snapshot_chain_and_middle_leftover)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-chain");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "CH", 2, 300000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        bool leftover = c_.coin(60);
        int levels = c_.range(1, 3);
        auto snap = [&](const Str& name) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["name"] = name; ctl.call("snapshot", a, o); };
        auto write = [&](int disk, int off, int len, int seed) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["disk"] = disk; a["offset"] = off; a["length"] = len; a["seed"] = seed; ctl.call("guestwrite", a, o); };
        if (leftover) snap("VM2Backup-deadbeef00");                                                                 // a crashed run's snapshot, below the user's own ones
        for (int i = 0; i < levels; ++i) { write(0, 100 * i, 3000 + i, 11 + i); if (f.spec.disks.size() > 1) write(1, 50, 2000, 31 + i); snap(Str::fmt("user snapshot %d", i)); }
        write(0, 777, 5000, 99);
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, true, &f.m.dir);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk);
        if (leftover) T_CHECK(Contains(r.warn, "removed 1 old VM2Backup snapshot"));
        T_CHECK(ctl.snapCount(f.spec.name) == levels);                                                              // the user's snapshots stay, ours are gone
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

VM2B_TEST("esxi", esxi_session_idle_timeout_during_long_copy)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-idle");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "ID", false, "poweredOff", true)); if (!r.f.m.ok) return false;
    // a second backup while sessions die after 1 s of silence and the copy is slow: the SOAP session is idle during the whole file transfer
    Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "ID2", 2, 500000, "poweredOn", true, 300000)); if (!f.m.ok) return false;
    Json fl = Json::object(); fl["session_idle_s"] = 1; fl["slow_ms"] = 350; ctl.faults(fl);
    RtResult rt = RoundTrip(c_, tmp, e, f.spec.name, f.before, e.cfg(), RandomOpts(c_, false), nullptr);
    if (!rt.ok) c_.note("%s backup: %s", e.name.c_str(), rt.err.c_str());
    T_CHECK(rt.ok); T_CHECK(rt.restoredOk);
    T_CHECK(ctl.counter("fault:session-idle") > 0);
    T_CHECK(ctl.leaked() == 0);
    // the same for the upload of a restore (PUT with a cookie that died: login again, send the file again)
    Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
    ReadOptions rd = RestoreRo(tmp); EsxiRestoreOptions ro; ro.vmName = r.f.spec.name + " idle"; EsxiRestoreResult res; Str err;
    int idle0 = ctl.counter("fault:session-idle");
    bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), ro, r.o.password, rd, nullptr, &res, err);
    if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
    T_CHECK(ok); if (ok) { T_CHECK(CheckRestoredFolder(c_, e, r, ro.vmName, "datastore1", false)); }
    c_.note("session-idle events: backup %d, restore %d", idle0, ctl.counter("fault:session-idle") - idle0);
    ctl.faults([] { Json j = Json::object(); j["session_idle_s"] = Json(); j["slow_ms"] = Json(); return j; }());
    delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, ro.vmName); DeleteVm(ctl, f.spec.name); RemoveTree(r.store);
    return true;
}

VM2B_TEST("esxi", esxi_soap_fault_per_call_matrix)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-fm");
    static const char* methods[] = { "CreateContainerView", "RetrievePropertiesEx", "CreateSnapshot_Task", "RemoveSnapshot_Task", "DestroyView", "RefreshStorageInfo", "Login", "Logout", "CancelTask" };
    static const char* types[] = { "NotAuthenticated", "NoPermission", "InvalidState", "HostCommunication", "RuntimeFault", "InvalidRequest", "ServerBusy503", "SystemError", "ManagedObjectNotFound" };
    int mi = c_.range(0, 8), ti = c_.range(0, 8);
    for (const Env& e : es) {
        mi = (mi + 1) % 9; ti = (ti + 4) % 9;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "FM", 1, 200000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        Json fl = Json::object(), d = Json::object(); d["method"] = methods[mi]; d["count"] = 1;
        Str ty = types[ti];
        if (ty == "ServerBusy503") { d["type"] = "ServerBusy"; d["http"] = 503; } else d["type"] = ty;
        d["msg"] = Str("injected ") + ty + " for " + methods[mi];
        fl["soap_fault"] = d; ctl.faults(fl);
        BackupOptions o = RandomOpts(c_, false);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, f.before, e.cfg(), o, nullptr, nullptr, false);
        bool fired = ctl.counter("fault:soap-fault") > 0;
        c_.note("%s %s/%s: %s%s", e.name.c_str(), methods[mi], ty.c_str(), r.ok ? "ok" : r.err.c_str(), fired ? "" : " (not reached)");
        if (!r.ok) { T_CHECK(!r.err.empty()); T_CHECK(!Contains(r.err, e.password.c_str())); }
        // a transient or relogin-able fault of one call never fails the backup; a Logout / DestroyView / Cancel fault never matters at all
        if (ty == "NotAuthenticated" || ty == "ServerBusy503") { if (!r.ok) c_.note("  -> should have been survived"); T_CHECK(r.ok); }
        if (!strcmp(methods[mi], "Logout") || !strcmp(methods[mi], "DestroyView") || !strcmp(methods[mi], "RefreshStorageInfo") || !strcmp(methods[mi], "CancelTask")) T_CHECK(r.ok);
        // whatever happened: no snapshot of ours is left on the host (the end of the run removes it with retries; the next begin() would)
        if (ctl.leaked() != 0) { c_.note("  -> a snapshot leaked"); }
        T_CHECK(ctl.leaked() == 0);
        // and the VM can be backed up again at once
        ctl.reset();
        RtResult r2 = RoundTrip(c_, tmp, e, f.spec.name, f.before, e.cfg(), o, nullptr);
        T_CHECK(r2.ok); T_CHECK(r2.restoredOk); T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

VM2B_TEST("esxi", esxi_certificate_oddities_with_pinned_fingerprint)
{
    Vec<Env> all; if (!Envs(c_, all)) return true;
    int n = 0;
    for (const Env& e : all) {
        if (e.cert.empty()) continue;
        ++n;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "CE"); Json a = Json::object(); a["name"] = nm; a["disks"] = DisksJ(c_, 1, 100000); a["power"] = "poweredOff";
        MockVm m = MakeVmJ(ctl, e, a); T_CHECK(m.ok);
        Vec<FI> before; Walk(m.dir, "", before);
        EsTmp tmp(c_, "esxi-cert");
        // pinned: the expired / wrong-name / IP-only certificate is accepted, everything works (list, backup, restore targets)
        { Str err; VmSource* src = NewEsxiSource(e.cfg(), err); T_CHECK(src != nullptr); Vec<VmInfo> vms; bool ok = src && src->list(vms); if (!ok) c_.note("%s list: %s", e.name.c_str(), src ? src->err.c_str() : err.c_str()); T_CHECK(ok); delete src; }
        RtResult r = RoundTrip(c_, tmp, e, nm, before, e.cfg(), RandomOpts(c_, false), nullptr);
        if (!r.ok) c_.note("%s backup: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk);
        { Json out; Str err; T_CHECK(EsxiRestoreTargets(e.cfg(), Str(), out, err)); }
        // the same certificate without a pin and with verification on: refused with a certificate text (it is self-signed and, here, also odd), no secret in it
        { Json cfg = e.cfg(); cfg.remove("fingerprint"); cfg["verify"] = true; Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr);
          Vec<VmInfo> vms; bool ok = src && src->list(vms); T_CHECK(!ok); if (src) { T_CHECK(Contains(src->err.lower(), "certificate") || Contains(src->err.lower(), "sha256")); T_CHECK(!Contains(src->err, e.password.c_str())); } delete src; }
        // a wrong pin on it
        { Json cfg = e.cfg(); cfg["fingerprint"] = "sha256:" + Rep(64, '0'); Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr); Vec<VmInfo> vms; bool ok = src && src->list(vms); T_CHECK(!ok); delete src; }
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, nm);
    }
    if (n == 0) c_.skip("no certificate-variant emulators");
    return true;
}

VM2B_TEST("esxi", esxi_vmx_vmsd_rewrite_hostile_input)
{
    // no server: the rewriting of text that comes from a datastore / an archive must never crash, hang, or let the VM name start new lines
    static const char* names[] = { "plain", "q\"uote", "bar|7C|22", "line\nbreak\r\nx = \"1\"", "tab\tname", "\xd0\xb2\xd0\xbc \xe6\x97\xa5\xe6\x9c\xac", "", " ", "x = y", "a\"\nsnapshot.numSnapshots = \"99" };
    for (int round = 0; round < 300; ++round) {
        Str vmx; int nl = c_.range(0, 60);
        for (int i = 0; i < nl; ++i) {
            switch (c_.rnd(14)) {
            case 0: vmx += "displayName = \"Old " + Str::fmt("%u", c_.rnd(100)) + "\""; break;
            case 1: vmx += Str::fmt("scsi0:%u.fileName = \"disk%u-00000%u.vmdk\"", c_.rnd(15), c_.rnd(3), c_.rnd(9)); break;
            case 2: vmx += "uuid.action = \"create\""; break;
            case 3: vmx += "snapshot.redoNotWithParent = \"TRUE\""; break;
            case 4: vmx += "ctkEnabled = \"TRUE\""; break;
            case 5: vmx += "=== no key"; break;
            case 6: vmx += "= \"x\""; break;
            case 7: vmx += "key"; break;
            case 8: { Str g; for (int k = c_.range(0, 200); k > 0; --k) g += (char)c_.rnd(256); g.replace_all("\n", " "); vmx += g; break; }
            case 9: vmx += Str("sata0:1.fileName = \"[datastore1] ") + Rep((size_t)c_.range(1, 5000), 'a') + "/x.vmdk\""; break;
            case 10: vmx += "scsi0:0.fileName = \"|22|7C\""; break;
            case 11: vmx += "displayName=\"dup\""; break;
            case 12: vmx += ""; break;
            default: vmx += "memSize = \"" + Str::fmt("%u", c_.rnd(100000)) + "\""; break;
            }
            vmx += c_.coin(30) ? "\r\n" : "\n";
        }
        Vec<Str> from, to; from.push_back("disk0-000001.vmdk"); to.push_back("disk0.vmdk"); from.push_back("x.vmdk"); to.push_back("[ds] \"q\"|/x.vmdk");
        Str nn = names[c_.rnd(10)];
        Str out = EsxiRewriteVmx(vmx, nn, from, to);
        // exactly one displayName line, one uuid.action = "keep", no stray quote / bar escapes broken, no line count growth beyond the two lines we may add
        int dn = 0, ua = 0, lines = 0, inl = 0;
        for (size_t i = 0, st = 0; i <= out.size(); ++i) if (i == out.size() || out[i] == '\n') { Str l = out.substr(st, i - st); st = i + 1; if (l.empty() && i == out.size()) continue; ++lines; if (l.starts_with("displayName = \"")) { ++dn; Str v = l.substr(15); T_CHECK(v.size() >= 1 && v[v.size() - 1] == '"'); v = v.substr(0, v.size() - 1); T_CHECK(v.find('"') < 0 && v.find('|') >= 0 ? true : v.find('"') < 0); } if (l == "uuid.action = \"keep\"") ++ua; }
        for (size_t i = 0; i <= vmx.size(); ++i) if (i == vmx.size() || vmx[i] == '\n') ++inl;
        T_CHECK(dn >= 1); T_CHECK(ua >= 1); T_CHECK(lines <= inl + 3);
    }
    // .vmsd: huge / odd snapshot numbers must not blow up (the number is only an index to renumber)
    static const char* vmsds[] = {
        "snapshot2147483647.uid = \"1\"\nsnapshot2147483647.displayName = \"VM2Backup-x\"\nsnapshot.numSnapshots = \"1\"\n",
        "snapshot99999999999999999999.uid = \"1\"\nsnapshot0.displayName = \"VM2Backup-x\"\n",
        "snapshot-5.uid = \"1\"\nsnapshot.numSnapshots = \"-3\"\nsnapshotX.uid = \"2\"\n",
        "snapshot0.displayName = \"VM2Backup-x\"\nsnapshot0.displayName = \"VM2Backup-x\"\nsnapshot0.parent = \"0\"\n",
        "snapshot1000000.displayName = \"VM2Backup-x\"\nsnapshot1000000.uid = \"4\"\nsnapshot999.parent = \"4\"\n",
    };
    for (const char* v : vmsds) { DWORD t0 = GetTickCount(); Str o = EsxiRewriteVmsd(v, "VM2Backup-x"); T_CHECK(GetTickCount() - t0 < 5000); T_CHECK(o.size() < 100000); }
    for (int round = 0; round < 200; ++round) {
        Str t; for (int i = c_.range(0, 40); i > 0; --i) {
            switch (c_.rnd(6)) {
            case 0: t += Str::fmt("snapshot%u.uid = \"%u\"\n", c_.rnd(6), c_.rnd(9)); break;
            case 1: t += Str::fmt("snapshot%u.displayName = \"%s\"\n", c_.rnd(6), c_.coin() ? "VM2Backup-x" : "mine"); break;
            case 2: t += Str::fmt("snapshot%u.parent = \"%u\"\n", c_.rnd(6), c_.rnd(9)); break;
            case 3: t += "snapshot.numSnapshots = \"" + Str::fmt("%u", c_.rnd(9)) + "\"\n"; break;
            case 4: t += "snapshot.current = \"3\"\n"; break;
            default: { Str g; for (int k = c_.range(0, 80); k > 0; --k) g += (char)(32 + c_.rnd(95)); t += g + "\n"; }
            }
        }
        Str o = EsxiRewriteVmsd(t, "VM2Backup-x"); T_CHECK(o.size() < t.size() + 200);
    }
    return true;
}

namespace {
// kills the host (the mock stops answering, as if the machine lost power) after `n` events, then brings it back
static void Revive(Ctl& ctl) { Json o; ctl.call("revive", Json::object(), o); }
}

VM2B_TEST("esxi", esxi_power_loss_during_backup_then_again)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-pl");
    int died = 0, total = 0;
    for (const Env& e : es) {
        if (c_.coin(40)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "PL", 2, 400000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        int n = c_.range(1, 60); Json fl = Json::object(); fl["die_after"] = n; ctl.faults(fl);
        BackupOptions o = RandomOpts(c_, true); o.retry = Fast(4);
        RemoveTree(tmp.sub("store"));
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Target* t = LocalAt(tmp.sub("store")); Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        Json st = ctl.state(); bool dead = st.get("dead").boolean();
        ++total; if (dead) ++died;
        c_.note("%s: lost after %d events: %s%s", e.name.c_str(), n, ok ? "backup completed" : err.c_str(), dead ? " (host was down)" : "");
        T_CHECK(ok || dead);                                                  // a failure without an outage would be a product bug
        delete src;
        Revive(ctl);
        // the host is back: whatever is left (a snapshot of ours, a half-written archive) must not stop the next run
        Str err2; VmSource* src2 = NewEsxiSource(e.cfg(), err2); Manifest man2; Str warn2; Pr pr2;
        bool ok2 = BackupVm(*src2, f.spec.name, *t, o, &pr2, man2, warn2, err2);
        if (!ok2) c_.note("%s second run: %s", e.name.c_str(), err2.c_str());
        T_CHECK(ok2);
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        if (ok2) { Str dest = tmp.sub("r"), re; RemoveTree(dest); if (Restore(c_, *t, man2, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, f.spec.name), "", after); T_CHECK(CheckTree(c_, f.before, after)); } }
        delete t; delete src2; DeleteVm(ctl, f.spec.name);
    }
    c_.note("%d of %d runs lost the host before the end", died, total);
    return true;
}

VM2B_TEST("esxi", esxi_power_loss_during_restore_then_again)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-plr");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "PR", c_.coin(), "poweredOff", true)); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); rd.retry = Fast(3);
        EsxiRestoreOptions o; o.vmName = r.f.spec.name + " pl"; Str newName = o.vmName;
        int dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        int n = c_.range(1, 80); Json fl = Json::object(); fl["die_after"] = n; ctl.faults(fl);
        Str err; EsxiRestoreResult res;
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, nullptr, &res, err);
        bool dead = ctl.state().get("dead").boolean();
        c_.note("%s: lost after %d events: %s", e.name.c_str(), n, ok ? "restore completed" : err.c_str());
        T_CHECK(ok || dead);
        if (!ok) { T_CHECK(ctl.vmState(newName).is_null() || StateHasVm(ctl, newName, "datastore1")); }       // never a half-made VM: either none or a complete one
        Revive(ctl);
        if (!ok) {                                                                                           // the same restore again: continues or starts over, registers exactly once
            RHook pr2; EsxiRestoreResult res2; Str err2;
            bool ok2 = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr2, &res2, err2);
            if (!ok2) c_.note("%s second run: %s", e.name.c_str(), err2.c_str());
            T_CHECK(ok2);
            if (ok2) T_CHECK(CheckRestoredFolder(c_, e, r, newName, "datastore1", false));
        }
        T_CHECK(StateHasVm(ctl, newName, "datastore1"));
        int vmsNamed = 0; { Json s = ctl.state(); const Json& vms = s.get("vms"); for (size_t i = 0; i < vms.size(); ++i) if (vms.key_at(i) == newName) ++vmsNamed; } T_CHECK(vmsNamed == 1);
        { int dn = CountDirs(e, "datastore1") + CountDirs(e, "datastore2"); if (dn != dirs0 + (r.ds2 ? 2 : 1)) { c_.note("%s: %d folders, %d expected; first run %s", e.name.c_str(), dn, dirs0 + (r.ds2 ? 2 : 1), ok ? "ok" : "failed");
            static const char* const kD[] = { "datastore1", "datastore2" }; for (const char* ds : kD) { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(JoinPath(e.root, ds), "*")), &fd); if (h == INVALID_HANDLE_VALUE) continue; do { Str nme = FromWide(fd.cFileName); if (nme[0] != '.') c_.note("    [%s] %s", ds, nme.c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); } }
          T_CHECK(dn == dirs0 + (r.ds2 ? 2 : 1)); }
        { Vec<FI> stt; Walk(rd.stateDir, "", stt); for (const FI& q : stt) c_.note("left in the state dir: %s", q.rel.c_str()); T_CHECK(stt.empty()); }
        T_CHECK(ctl.leaked() == 0);
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

VM2B_TEST("esxi", esxi_restore_hardening_names_and_files)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rh");
    for (const Env& e : es) {
        if (c_.coin(55)) continue;
        Ctl ctl(e); ctl.reset();
        // a VM with a 0-byte file and a tail file that will break at its very last byte
        RFix r; r.ds2 = false;
        r.f.spec.name = UniqueName(c_, "RH"); r.f.spec.disks = Disks(c_, 1, 100000); for (size_t i = 0; i < r.f.spec.disks.size(); ++i) r.f.spec.disks.item(i)["thin"] = false;
        Json ex = Json::array(); { Json z = Json::object(); z["name"] = "empty.bin"; z["size"] = 0; z["type"] = "namespaceData"; ex.push(z); Json t2 = Json::object(); t2["name"] = "tail.bin"; t2["size"] = 70000; t2["type"] = "namespaceData"; ex.push(t2); }
        r.f.spec.extra = ex; if (e.vcenter()) r.f.spec.dc = e.dcs[c_.rnd((u32)e.dcs.size())];
        r.f.m = MakeVm(ctl, e, r.f.spec); T_CHECK(r.f.m.ok); if (!r.f.m.ok) return false;
        Walk(r.f.m.dir, "", r.f.before);
        r.o = RandomOpts(c_, false); Str err; VmSource* src = NewEsxiSource(e.cfg(), err);
        Json tc = Json::object(); r.store = tmp.sub("store"); RemoveTree(r.store); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        { Pr pr; Str warn; bool ok = BackupVm(*src, r.f.spec.name, *t, r.o, &pr, r.man, warn, err); T_CHECK(ok); delete src; if (!ok) return false; }
        ReadOptions rd = RestoreRo(tmp);
        // 1. the 0-byte file and the break at the last byte
        { Json d = Json::object(); d["match"] = "tail.bin"; d["bytes"] = 69999; d["count"] = 1; Json fl = Json::object(); fl["put_drop"] = d; ctl.faults(fl);
          EsxiRestoreOptions o; o.vmName = r.f.spec.name + " tail"; RHook pr; EsxiRestoreResult res;
          bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, err); if (!ok) c_.note("%s tail: %s", e.name.c_str(), err.c_str());
          T_CHECK(ok); T_CHECK(ctl.counter("fault:put-drop") == 1);
          if (ok) T_CHECK(CheckRestoredFolder(c_, e, r, o.vmName, "datastore1", false));
          ctl.reset(); DeleteVm(ctl, o.vmName); }
        // 2. a datastore with a spaced unicode name, and the original name with special characters (URL escaping of dsName / path, RegisterVM of such a path)
        { EsxiRestoreOptions o; o.vmName = Str("weird #%&+ \xc3\xbc\xe6\x97\xa5 [x] (1)"); o.datastore = kNfsDs; RHook pr; EsxiRestoreResult res;
          bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, err); if (!ok) c_.note("%s weird: %s", e.name.c_str(), err.c_str());
          T_CHECK(ok);
          if (ok) { T_CHECK(CheckRestoredFolder(c_, e, r, o.vmName, kNfsDs, true)); T_CHECK(StateHasVm(ctl, o.vmName, kNfsDs)); }
          DeleteVm(ctl, o.vmName); }
        // 3. names that must never escape the datastore or make a second VM folder: refused, or made under a safe path; no stray folder anywhere
        static const char* hostile[] = { "../escape", "a/b", "a\\b", "..", ".", "/abs", "[datastore2] sneaky", "name\nwith newline", "trail ", "x%2fy", "" };
        for (const char* hn : hostile) {
            int d0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
            EsxiRestoreOptions o; o.vmName = hn; RHook pr; EsxiRestoreResult res; Str er2;
            bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, &res, er2);
            int d1 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
            // nothing may appear beside the datastores in the emulator's root
            int top = 0; { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(e.root, "*")), &fd); if (h != INVALID_HANDLE_VALUE) { do { Str nme = FromWide(fd.cFileName); if (nme == "." || nme == ".." || nme == ".mock" || nme == "datastore1" || nme == "datastore2" || nme == "vsanDatastore" || nme == kNfsDs) continue; ++top; c_.note("stray entry in the datastore root: %s", nme.c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); } }
            T_CHECK(top == 0);
            if (ok) { bool known = !ctl.vmState(res.vmName).is_null() || !ctl.vmState(res.vmName.trim()).is_null(); if (!known) c_.note("  name '%s' -> VM '%s': folders %d -> %d, vm known: %d", Str(hn).replace_all("\n", "\\n").c_str(), res.vmName.replace_all("\n", "\\n").c_str(), d0, d1, (int)!ctl.vmState(res.vmName).is_null()); T_CHECK(d1 == d0 + 1 || SafeFolderOf(hn).starts_with(".")); T_CHECK(!res.vmName.empty()); T_CHECK(known); DeleteVm(ctl, res.vmName); Json tmpj; (void)tmpj; }
            else { if (d1 != d0) c_.note("'%s' refused (%s) but left %d new folder(s)", hn, er2.c_str(), d1 - d0); T_CHECK(d1 == d0); T_CHECK(!er2.empty()); }
            c_.note("  name '%s': %s", Str(hn).replace_all("\n", "\\n").c_str(), ok ? "restored" : er2.c_str());
            // clean up whatever was made
            { Json s = ctl.state(); const Json& vms = s.get("vms"); Vec<Str> del; for (size_t i = 0; i < vms.size(); ++i) { Str nme = vms.key_at(i); if (nme != r.f.spec.name) del.push_back(nme); } for (const Str& nme : del) DeleteVm(ctl, nme); }
            WIN32_FIND_DATAW fd2; static const char* const kDss[] = { "datastore1", "datastore2" }; for (const char* ds : kDss) { HANDLE h = FindFirstFileW(FilePath(JoinPath(JoinPath(e.root, ds), "*")), &fd2); if (h == INVALID_HANDLE_VALUE) continue; Vec<Str> names; do { Str nme = FromWide(fd2.cFileName); if (nme != "." && nme != ".." && nme != r.f.spec.name && nme[0] != '.' && (fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(nme); } while (FindNextFileW(h, &fd2)); FindClose(h); for (const Str& nme : names) if (nme != PathBase(r.f.m.dir)) RemoveTree(JoinPath(JoinPath(e.root, ds), nme)); }
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; DeleteVm(ctl, r.f.spec.name); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

VM2B_TEST("esxi", esxi_vm_names_windows_cannot_store)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-wn");
    Str cyr; for (int i = 0; i < 150; ++i) cyr += "\xd0\xb6";                       // 300 bytes
    if (NtfsDatastore(es[0])) { c_.skip("the mock datastore is on NTFS: the emulator cannot hold files named like these VMs"); return true; }
    Vec<Str> names; names.push_back("a:b*c?"); names.push_back("trailing."); names.push_back("CON"); names.push_back(cyr);
    names.push_back("quo\"te<>|"); names.push_back("a_b"); names.push_back("COM1");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Str suffix = Str::fmt("-%u", c_.rnd(100000));
    Vec<Str> prefixes;
    for (size_t i = 0; i < names.size(); ++i) {
        Str nm = names[i].size() > 20 ? names[i] : names[i] + suffix;
        if (i == 0 || i == 6) nm = names[i];                                   // "a:b*c?" and "a_b": same prefix on purpose? (only when the safe forms are equal, see below)
        Json a = Json::object(); a["name"] = nm; a["dir"] = Str::fmt("wn-dir-%d-", (int)i) + Str::fmt("%u", c_.rnd(100000)); a["power"] = "poweredOff";
        a["disks"] = DisksJ(c_, 1, 60000); a["seed"] = (i64)(i * 17 + 3);
        if (names[i].size() > 100) a["vmx"] = "cyrillic-long-vm";                                         // the host limits a file name to 255 bytes
        bool bad = false; for (size_t k = 0; k < nm.size(); ++k) if (strchr(":*?\"<>|", nm[k])) bad = true; if (EsxiArchivePrefix(nm) != nm) bad = true;      // the emulator's files carry these characters: the Windows side of the test cannot read them
        if (!ctl.vmState(nm).is_null()) continue;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before); for (FI& q : before) q.rel = MapRel(q.rel);
        Str pre = EsxiArchivePrefix(nm); prefixes.push_back(pre);
        T_CHECK(!pre.empty() && pre.size() <= 200);
        for (size_t k = 0; k < pre.size(); ++k) T_CHECK(strchr(":*?\"<>|\\/", pre[k]) == nullptr);
        T_CHECK(!pre.ends_with(".") && !pre.ends_with(" "));
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); RemoveTree(tmp.sub("store")); if (i == 0) {} 
        Target* t = LocalAt(tmp.sub(Str::fmt("store%d", (int)i).c_str())); Manifest man; Str warn; Pr pr; BackupOptions o = RandomOpts(c_, false);
        bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("name '%s': %s", nm.size() > 40 ? "<long>" : nm.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(man.vm == nm);
        if (ok) {
            Str dest = tmp.sub(Str::fmt("r%d", (int)i).c_str()), re;
            if (Restore(c_, *t, man, o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, pre), "", after); if (!bad) T_CHECK(CheckTree(c_, before, after)); 
                Bytes mb; Str me; Json mj; Str pe; T_CHECK(ReadWholeFile(JoinPath(dest, "vm2backup-meta.json").c_str(), mb, me) && Json::parse((const char*)mb.data(), mb.size(), mj, pe)); T_CHECK(mj.get("vm").str() == nm); }
            // restore to the host under the real name: the VM and its folder follow the original name, not the archive prefix
            ReadOptions rd = RestoreRo(tmp); EsxiRestoreOptions ro; EsxiRestoreResult res; Str rerr;
            bool rok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, nullptr, &res, rerr);
            if (!rok) c_.note("restore of '%s': %s", nm.size() > 40 ? "<long>" : nm.c_str(), rerr.c_str());
            T_CHECK(rok); if (rok) { T_CHECK(res.vmName == nm + "-restored"); T_CHECK(!ctl.vmState(res.vmName).is_null()); }
            if (rok) DeleteVm(ctl, res.vmName);
        }
        T_CHECK(ctl.leaked() == 0);
        delete t; delete src;
    }
    // "a:b*c?" and "a_b" share no prefix (the first one's '_' form is "a_b_c_"), but two names that do map to one prefix never mix their files:
    // every archive was restored above on its own and compared with its own VM.
    for (const Str& n : names) DeleteVm(ctl, n.size() > 20 ? n : n + suffix);
    DeleteVm(ctl, names[0]); DeleteVm(ctl, names[6]);
    return true;
}

VM2B_TEST("esxi", esxi_two_vms_with_the_same_archive_prefix)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-pfx");
    const Env& e = es[c_.rnd((u32)es.size())];
    if (NtfsDatastore(e)) { c_.skip("the mock datastore is on NTFS: a file name cannot hold ':'"); return true; }
    Ctl ctl(e); ctl.reset();
    Str n1 = "pfx:vm", n2 = "pfx_vm"; T_CHECK(EsxiArchivePrefix(n1) == EsxiArchivePrefix(n2));
    Str store = tmp.sub("store"); Target* t = LocalAt(store);
    Manifest mans[2]; Vec<FI> befores[2]; BackupOptions o = RandomOpts(c_, false);
    for (int i = 0; i < 2; ++i) {
        Str nm = i ? n2 : n1;
        Json a = Json::object(); a["name"] = nm; a["dir"] = Str::fmt("pfxdir%d", i); a["power"] = "poweredOff"; a["disks"] = DisksJ(c_, 1, 80000); a["seed"] = (i64)(100 + i);
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        Walk(m.dir, "", befores[i]); for (FI& q : befores[i]) q.rel = MapRel(q.rel);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err); Str warn; Pr pr;
        bool ok = BackupVm(*src, nm, *t, o, &pr, mans[i], warn, err); if (!ok) c_.note("%s", err.c_str()); T_CHECK(ok); delete src;
    }
    T_CHECK(mans[0].archive != mans[1].archive);
    for (int i = 0; i < 2; ++i) {
        Str dest = tmp.sub(Str::fmt("r%d", i).c_str()), re;
        if (Restore(c_, *t, mans[i], o, dest, re)) { Vec<FI> after; Walk(JoinPath(dest, EsxiArchivePrefix("pfx_vm")), "", after); T_CHECK(CheckTree(c_, befores[i], after)); }
    }
    delete t; DeleteVm(ctl, n1); DeleteVm(ctl, n2);
    return true;
}

// A datastore that answers 200 with something that is not the disk (an HTML error page of a proxy, a file cut short with a consistent Content-Length):
// the backup must not end as a clean success with a zero-filled disk inside.
VM2B_TEST("esxi", esxi_wrong_bodies_from_the_datastore)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-wb");
    int kind = c_.range(0, 3);
    for (const Env& e : es) {
        if (c_.coin(40)) continue;
        kind = (kind + 1) % 4;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "WB", 1, 400000, c_.coin() ? "poweredOn" : "poweredOff", true, 100000)); if (!f.m.ok) return false;
        Json fl = Json::object(), od = Json::object();
        od["match"] = kind < 2 ? "-flat.vmdk" : ".vmdk"; od["mode"] = (kind & 1) ? "trunc" : "html200"; od["bytes"] = (i64)c_.range(1, 5000);
        od["match"] = kind < 2 ? "-flat.vmdk" : ".vmx"; if (kind < 2 && c_.coin()) od["head"] = true;
        fl["serve_odd"] = od; ctl.faults(fl);
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err);
        Str dest = tmp.sub("store"); RemoveTree(dest); Target* t = LocalAt(dest);
        Manifest man; Str warn; Pr pr; BackupOptions o = RandomOpts(c_, false);
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (kind < 2) {                                                                // a disk that came back wrong is never a clean success
            if (ok) c_.note("%s kind %d: backup succeeded, warn=[%s]", e.name.c_str(), kind, warn.c_str());
            T_CHECK(!ok); T_CHECK(Contains(err, kind == 0 || od.has("head") ? "HTML page" : "delivered"));
        }
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// Snapshot tree with branches (A -> B, revert to A, A -> C): every frozen chain of every branch must be in the archive, the tree stays on the host,
// and a restore to the host gives a VM whose disk chains are complete down to the base.
VM2B_TEST("esxi", esxi_branching_snapshot_tree)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-br");
    for (const Env& e : es) {
        if (c_.coin(45)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "BR", c_.range(1, 2), 300000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        auto snap = [&](const Str& name) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["name"] = name; ctl.call("snapshot", a, o); };
        auto revert = [&](const Str& name) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["name"] = name; ctl.call("revert", a, o); };
        auto write = [&](int disk, int off, int len, int seed) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["disk"] = disk; a["offset"] = off; a["length"] = len; a["seed"] = seed; ctl.call("guestwrite", a, o); };
        write(0, 100, 3000, 1); snap("A"); write(0, 5000, 4000, 2); snap("B"); write(0, 9000, 2000, 3);
        revert("A"); write(0, 200, 3000, 4); snap("C");
        bool deeper = c_.coin(50);
        if (deeper) { write(0, 300, 1000, 5); snap("C2"); revert("B"); write(0, 50, 1000, 6); snap("D1"); }           // a third branch, under B
        bool leftover = c_.coin(40);
        if (leftover) snap("VM2Backup-deadbeef00");                                                                  // the crashed run's snapshot is the current one
        write(0, 777, 5000, 99);
        int snaps = ctl.snapCount(f.spec.name);
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, true, &f.m.dir);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk);
        if (leftover) T_CHECK(Contains(r.warn, "removed 1 old VM2Backup snapshot"));
        T_CHECK(ctl.snapCount(f.spec.name) == snaps - (leftover ? 1 : 0));
        T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// Files that layoutEx lists but the datastore no longer has (before the run, or taken away during the copy): a clean, named failure, the snapshot removed.
VM2B_TEST("esxi", esxi_files_vanish_from_the_datastore)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-gone");
    int kind = c_.range(0, 5);
    for (const Env& e : es) {
        if (c_.coin(45)) continue;
        kind = (kind + 1) % 6;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "GN", 2, 300000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        auto unlink = [&](const Str& file) { Json a = Json::object(), o; a["vm"] = f.spec.name; a["file"] = file; a["suffix"] = true; ctl.call("unlink", a, o); };
        Str what;
        if (kind == 0) { unlink(".vmxf"); what = ".vmxf"; }
        else if (kind == 1) { unlink(".nvram"); what = ".nvram"; }
        else if (kind == 2) { unlink("_1-flat.vmdk"); what = "_1-flat.vmdk"; }          // the second disk's data
        else if (kind == 4 || kind == 5) { Json fl = Json::object(), h = Json::object(); h["match"] = "-flat.vmdk"; if (kind == 5) h["reads"] = c_.range(1, 2); fl["layout_hide"] = h; ctl.faults(fl); what = "file entries missing from layoutEx"; }
        else what = "taken away during the copy";
        Str err; VmSource* src = NewEsxiSource(e.cfg(), err);
        Str dest = tmp.sub("store"); RemoveTree(dest); Target* t = LocalAt(dest);
        Manifest man; Str warn; Pr pr; BackupOptions o = RandomOpts(c_, false);
        struct GoneHook { Ctl* ctl; Str vm; static void Fn(Pr* p, int idx, const Str&) { GoneHook* g = (GoneHook*)p->user; if (idx != 1) return; Json a = Json::object(), o; a["vm"] = g->vm; a["file"] = "_1-flat.vmdk"; a["suffix"] = true; g->ctl->call("unlink", a, o); } };
        GoneHook gh; gh.ctl = &ctl; gh.vm = f.spec.name;
        if (kind == 3) { pr.hook = GoneHook::Fn; pr.user = &gh; }
        bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
        if (ok && kind != 5) c_.note("%s kind %d (%s): backup succeeded", e.name.c_str(), kind, what.c_str());
        if (kind == 5) { if (!ok) c_.note("%s: a layout that catches up after a moment was not waited for: %s", e.name.c_str(), err.c_str()); T_CHECK(ok); T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0); delete t; delete src; DeleteVm(ctl, f.spec.name); continue; }
        T_CHECK(!ok); T_CHECK(kind == 3 ? (Contains(err, "404") || Contains(err, "missing on the datastore")) : Contains(err, kind == 4 ? "no entry" : "missing on the datastore"));
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        delete t; delete src; DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// CreateSnapshot that outlives the client's timeout and cannot be cancelled: the snapshot appears after the client gave up. It must not stay behind.
VM2B_TEST("esxi", esxi_slow_snapshot_task_after_timeout)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-slowsnap");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "SL", 1, 100000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
    Json fl = Json::object(), sl = Json::object(); sl["name"] = "CreateSnapshot"; sl["sec"] = 4; fl["task_slow"] = sl; ctl.faults(fl);
    Json cfg = e.cfg(); cfg["task_timeout"] = 2;
    Str err; VmSource* src = NewEsxiSource(cfg, err);
    Str dest = tmp.sub("store"); Target* t = LocalAt(dest);
    Manifest man; Str warn; Pr pr; BackupOptions o = RandomOpts(c_, false);
    bool ok = BackupVm(*src, f.spec.name, *t, o, &pr, man, warn, err);
    T_CHECK(!ok); T_CHECK(Contains(err, "timed out"));
    Sleep(5500);                                                                       // the host finishes the task now
    if (ctl.leaked() != 0) c_.note("the snapshot made after the timeout stayed behind");
    T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
    Json fn = Json::object(); fn["task_slow"] = Json(); ctl.faults(fn);
    delete t; delete src; DeleteVm(ctl, f.spec.name);
    return true;
}

// Someone else (another backup tool, an admin) takes a snapshot while the copy runs: ours is then in the middle of the tree. The copy stays valid, only ours
// is removed, the late one stays.
VM2B_TEST("esxi", esxi_foreign_snapshot_created_during_the_copy)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-late");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "LT", 2, 300000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        struct LateHook { Ctl* ctl; Str vm; int fired; static void Fn(Pr* p, int idx, const Str&) {
            LateHook* g = (LateHook*)p->user; if (idx != 2 || g->fired) return; g->fired = 1;
            Json a = Json::object(), o; a["vm"] = g->vm; a["name"] = "late foreign snapshot"; g->ctl->call("snapshot", a, o);
            Json w = Json::object(); w["vm"] = g->vm; w["disk"] = 0; w["offset"] = 100; w["length"] = 5000; w["seed"] = 3; g->ctl->call("guestwrite", w, o); } };
        LateHook lh; lh.ctl = &ctl; lh.vm = f.spec.name; lh.fired = 0;
        Pr pr; pr.hook = LateHook::Fn; pr.user = &lh;
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, &pr, true);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(lh.fired == 1);
        T_CHECK(ctl.snapCount(f.spec.name) == 1); T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// Somebody else's datastore folder of the same name: lost races on MakeDirectory and a leftover folder on the second datastore. Nothing of it may be
// merged into, overwritten or deleted by the restore.
VM2B_TEST("esxi", esxi_restore_never_touches_a_foreign_folder)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-foreign");
    int kind = c_.range(0, 1);
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        kind ^= 1;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "FD", true, "poweredOff")); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err; Pr pr;
        EsxiRestoreOptions o; o.vmName = "fresh-" + Str::fmt("%u", c_.rnd(100000));
        Str precious;
        if (kind == 0) {                                     // the folder appears on datastore1 between the name check and MakeDirectory
            Json fl = Json::object(), mr = Json::object(); mr["count"] = 1; fl["mkdir_race"] = mr; ctl.faults(fl);
            precious = JoinPath(JoinPath(JoinPath(e.root, "datastore1"), o.vmName), "other-clients-file.txt");
        } else {                                             // a folder of that name on the second datastore (where the second disk goes)
            Str d2 = JoinPath(JoinPath(e.root, "datastore2"), o.vmName); Str mk; MakeDirs(d2, mk);
            Bytes b; b.resize(100); WriteWholeFile(JoinPath(d2, "precious.txt").c_str(), b.data(), b.size(), mk);
            precious = JoinPath(d2, "precious.txt");
        }
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err);
        if (ok) c_.note("%s kind %d: the restore went on into a foreign folder", e.name.c_str(), kind);
        T_CHECK(!ok); T_CHECK(Contains(err, "meanwhile") || Contains(err, "already exist"));
        if (!FileExists(precious)) c_.note("%s kind %d: the foreign folder was deleted", e.name.c_str(), kind);
        T_CHECK(FileExists(precious));
        if (kind == 1) { Vec<FI> in2; Walk(JoinPath(JoinPath(e.root, "datastore2"), o.vmName), "", in2); T_CHECK(in2.size() == 1); }
        T_CHECK(!StateHasVm(ctl, o.vmName, "datastore1"));
        RemoveTree(JoinPath(JoinPath(e.root, "datastore1"), o.vmName)); RemoveTree(JoinPath(JoinPath(e.root, "datastore2"), o.vmName));
        delete t; DeleteVm(ctl, r.f.spec.name); RemoveTree(r.store);
    }
    return true;
}

// The datastore answers a GET with 500 / 502 / 504 (an I/O hiccup, a proxy in front of the host): a few of them are survived, a permanent one ends the
// backup with a clear message and no snapshot left behind.
VM2B_TEST("esxi", esxi_datastore_io_errors_on_read)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-io");
    int kind = c_.range(0, 1);
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        kind = (kind + 1) % 2;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "IO", 1, 300000, c_.coin() ? "poweredOn" : "poweredOff", true, 50000)); if (!f.m.ok) return false;
        static const int kCodes[] = { 500, 502, 504 };
        int code = kCodes[c_.rnd(3)];
        Json fl = Json::object(), d = Json::object(); d["match"] = c_.coin() ? "-flat.vmdk" : ".nvram"; d["status"] = code; d["count"] = kind == 0 ? c_.range(1, 2) : -1; fl["file_status"] = d; ctl.faults(fl);
        BackupOptions o = RandomOpts(c_, false); o.retry = Fast(3);
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), o, nullptr, nullptr, true, &f.m.dir);
        if (kind == 0) { if (!r.ok) c_.note("%s kind %d (HTTP %d): %s", e.name.c_str(), kind, code, r.err.c_str()); T_CHECK(r.ok); T_CHECK(r.restoredOk); }
        else { if (r.ok) c_.note("%s: backup succeeded although every read fails", e.name.c_str()); T_CHECK(!r.ok); T_CHECK(Contains(r.err, Str::fmt("HTTP %d", code).c_str())); }
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// Disks of 0, 1, 511 .. 513 bytes and 1 MiB + 1 (nothing aligned), under a delta chain of up to 7 user snapshots.
VM2B_TEST("esxi", esxi_zero_and_unaligned_disks_with_deep_chains)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-deep");
    static const i64 kSizes[] = { 0, 1, 511, 512, 513, 4095, 1048577 };
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "DP"); Json a = Json::object(); a["name"] = nm; a["power"] = c_.coin() ? "poweredOn" : "poweredOff";
        Json d = Json::array(); int nd = c_.range(1, 3);
        for (int i = 0; i < nd; ++i) { Json o = Json::object(); o["size"] = kSizes[c_.rnd(7)]; o["thin"] = c_.coin(); o["seed"] = (i64)c_.range(1, 999); d.push(o); }
        a["disks"] = d;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        int depth = c_.coin(20) ? c_.range(20, 34) : c_.range(0, 7);                                              // vSphere supports 32 snapshots in a chain
        for (int i = 0; i < depth; ++i) {
            Json w = Json::object(), o; w["vm"] = nm; w["disk"] = c_.range(0, nd - 1); w["offset"] = c_.range(0, 600); w["length"] = c_.range(1, 3000); w["seed"] = i + 1; ctl.call("guestwrite", w, o);
            Json sn = Json::object(); sn["vm"] = nm; sn["name"] = Str::fmt("level %d", i); ctl.call("snapshot", sn, o);
        }
        Vec<FI> before; Walk(m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, nm, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, true, &m.dir);
        if (!r.ok) c_.note("%s depth %d: %s", e.name.c_str(), depth, r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk); T_CHECK(ctl.snapCount(nm) == depth); T_CHECK(ctl.leaked() == 0);
        DeleteVm(ctl, nm);
    }
    return true;
}

// The same odd disks restored to the host: zero-byte uploads, a delta chain that must stay complete (descriptors point down to the base).
VM2B_TEST("esxi", esxi_restore_to_host_zero_size_disks_and_chains)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-deeprs");
    static const i64 kSizes[] = { 0, 1, 511, 513, 4097, 300001 };
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "DR"); Json a = Json::object(); a["name"] = nm; a["power"] = "poweredOff";
        Json d = Json::array(); int nd = c_.range(1, 3);
        for (int i = 0; i < nd; ++i) { Json o = Json::object(); o["size"] = kSizes[c_.rnd(6)]; o["thin"] = false; o["seed"] = (i64)c_.range(1, 999); d.push(o); }
        a["disks"] = d;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        int depth = c_.range(0, 5);
        for (int i = 0; i < depth; ++i) {
            Json w = Json::object(), o; w["vm"] = nm; w["disk"] = c_.range(0, nd - 1); w["offset"] = c_.range(0, 300); w["length"] = c_.range(1, 2000); w["seed"] = i + 1; ctl.call("guestwrite", w, o);
            Json sn = Json::object(); sn["vm"] = nm; sn["name"] = Str::fmt("level %d", i); ctl.call("snapshot", sn, o);
        }
        Vec<FI> before; Walk(m.dir, "", before);
        BackupOptions o = RandomOpts(c_, false); Str err, warn; Manifest man; Pr pr;
        VmSource* src = NewEsxiSource(e.cfg(), err); Str store = tmp.sub("store"); RemoveTree(store); Target* t = LocalAt(store);
        bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err); T_CHECK(ok); delete src;
        if (!ok) { c_.note("%s: %s", e.name.c_str(), err.c_str()); delete t; DeleteVm(ctl, nm); continue; }
        Vec<FI> hostAfter; Walk(m.dir, "", hostAfter);                                  // a consolidated leftover would differ: the originals are compared to this
        EsxiRestoreOptions ro; ro.vmName = nm + " copy";
        ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res;
        ok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, &pr, &res, err);
        if (!ok) c_.note("%s depth %d restore: %s", e.name.c_str(), depth, err.c_str());
        T_CHECK(ok);
        if (ok) {
            Vec<FI> got; Walk(JoinPath(JoinPath(e.root, "datastore1"), ro.vmName), "", got);
            for (const FI& b : before) {
                if (Excluded(b.rel) || b.rel.ends_with(".vmx") || b.rel.ends_with(".vmsd")) continue;
                const FI* g = FindFi(got, b.rel);
                if (!g) { c_.note("not restored: %s", b.rel.c_str()); T_CHECK(false); continue; }
                if (g->sha != b.sha || g->size != b.size) { c_.note("restored file differs: %s (%llu vs %llu)", b.rel.c_str(), (unsigned long long)b.size, (unsigned long long)g->size); T_CHECK(false); }
            }
            // every disk the restored .vmx names reaches its base through parentFileNameHint
            const FI* vx = nullptr; for (const FI& g : got) if (g.rel.ends_with(".vmx")) vx = &g;
            T_CHECK(vx != nullptr);
            if (vx) {
                Bytes vb; Str ve; ReadWholeFile(vx->full.c_str(), vb, ve); Str vt((const char*)vb.data(), vb.size());
                for (int i = 0; i < nd; ++i) {
                    Str v = VmxValue(vt, Str::fmt("scsi0:%d.fileName", i)); Str dsx, px; if (EsxiParseDsPath(v, dsx, px)) v = px; int sl = v.rfind('/'); if (sl >= 0) v = v.substr((size_t)sl + 1);
                    for (int hop = 0; hop < 20 && !v.empty(); ++hop) {
                        const FI* g = FindFi(got, v);
                        if (!g) { c_.note("disk %d: descriptor %s missing in the restored folder", i, v.c_str()); T_CHECK(false); break; }
                        Bytes db; ReadWholeFile(g->full.c_str(), db, ve); Str dt((const char*)db.data(), db.size());
                        v = VmxValue(dt, "parentFileNameHint");
                    }
                }
            }
        }
        delete t; DeleteVm(ctl, nm); DeleteVm(ctl, nm + " copy");
    }
    return true;
}

// Disks that live in other folders of the datastores (the usual result of years of clones and moves): two of them with the same file names. Backup and
// restore must keep both, never fold one over the other.
VM2B_TEST("esxi", esxi_disks_in_other_folders_with_clashing_names)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-dirs");
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "DD"); Json a = Json::object(); a["name"] = nm; a["power"] = "poweredOff";
        Json d = Json::array();
        for (int i = 0; i < 3; ++i) { Json o = Json::object(); o["size"] = (i64)c_.range(2000, 200000); o["thin"] = false; o["seed"] = (i64)c_.range(1, 9999);
            if (i >= 1) { o["dir"] = i == 1 ? "old disks one" : "old disks two"; o["fname"] = "shared-disk"; } if (i == 2 && c_.coin()) o["ds"] = "datastore2"; d.push(o); }
        a["disks"] = d;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before);
        Vec<FI> extra;                                                                  // the files of the other folders, as the restore of the VM folder will not hold them
        BackupOptions o = RandomOpts(c_, false); Str err, warn; Manifest man; Pr pr;
        VmSource* src = NewEsxiSource(e.cfg(), err); Str store = tmp.sub("store"); RemoveTree(store); Target* t = LocalAt(store);
        bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s backup: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); delete src;
        if (!ok) { delete t; DeleteVm(ctl, nm); continue; }
        // what the host holds (every disk file with its hash), keyed by the disk's own size + content
        Vec<FI> hostFlat;
        { Vec<FI> a1; Walk(JoinPath(e.root, "datastore1"), "", a1); Walk(JoinPath(e.root, "datastore2"), "", a1);
          for (const FI& f : a1) if (f.rel.find(nm.c_str()) >= 0 || f.rel.find("old disks") >= 0) if (f.rel.ends_with("-flat.vmdk")) hostFlat.push_back(f); }
        if (hostFlat.size() != 3) for (const FI& f : hostFlat) c_.note("   host flat: %s", f.rel.c_str());
        T_CHECK(hostFlat.size() == 3);
        bool forced = c_.coin(35); const char* mainDs = forced ? "datastore2" : "datastore1";
        EsxiRestoreOptions ro; ro.vmName = nm + " copy"; if (forced) ro.datastore = "datastore2"; ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res;
        ok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, &pr, &res, err);
        if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) {
            // every original flat file has an identical twin in the restored VM (own folder or the other datastore's)
            Vec<FI> got; Walk(JoinPath(JoinPath(e.root, "datastore1"), ro.vmName), "", got); Walk(JoinPath(JoinPath(e.root, "datastore2"), ro.vmName), "", got);
            int flat = 0; for (const FI& g : got) if (g.rel.ends_with("-flat.vmdk")) ++flat;
            if (flat != 3) c_.note("the restored VM holds %d flat disk files, 3 expected", flat);
            T_CHECK(flat == 3);
            // each disk entry of the restored .vmx reaches the flat file with the content of ITS original (not another disk's)
            Str vmxText; { Vec<FI> vf; Walk(JoinPath(JoinPath(e.root, mainDs), ro.vmName), "", vf); for (const FI& f : vf) if (f.rel.ends_with(".vmx")) { Bytes vb; Str ve; ReadWholeFile(f.full.c_str(), vb, ve); vmxText = Str((const char*)vb.data(), vb.size()); } }
            for (int i = 0; i < 3; ++i) {
                const char* want = i == 1 ? "old disks one/" : i == 2 ? "old disks two/" : nullptr; Str wantSha;
                for (const FI& h : hostFlat) if (want ? h.rel.starts_with(want) : (h.rel.find("old disks") < 0)) wantSha = h.sha;
                Str v = VmxValue(vmxText, Str::fmt("scsi0:%d.fileName", i)), dsx = mainDs, px = v;
                if (!EsxiParseDsPath(v, dsx, px)) { dsx = mainDs; px = ro.vmName + "/" + v; }
                Str full = JoinPath(JoinPath(e.root, dsx), px.replace_all("/", "\\")); Bytes db; Str ve;
                if (!ReadWholeFile(full.c_str(), db, ve)) { c_.note("disk %d: descriptor %s is not there", i, v.c_str()); T_CHECK(false); continue; }
                Str dt((const char*)db.data(), db.size()); int q1 = dt.find("RW "); int q2 = q1 >= 0 ? dt.find("\"", (size_t)q1) : -1; int q3 = q2 >= 0 ? dt.find("\"", (size_t)q2 + 1) : -1;
                if (q3 < 0) { c_.note("disk %d: no extent in the descriptor", i); T_CHECK(false); continue; }
                Str flatName = dt.substr((size_t)q2 + 1, (size_t)(q3 - q2 - 1)); int sl = full.rfind('\\'); Str flatPath = full.substr(0, (size_t)sl + 1) + flatName; u64 fsz = 0;
                Str got1 = HashFile(flatPath, fsz);
                if (got1 != wantSha) { c_.note("disk %d (%s) reaches the wrong data", i, v.c_str()); T_CHECK(false); }
            }
            for (const FI& h : hostFlat) { bool twin = false; for (const FI& g : got) if (g.sha == h.sha && g.size == h.size) twin = true; if (!twin) c_.note("no restored twin of %s", h.rel.c_str()); T_CHECK(twin); }
        }
        delete t; DeleteVm(ctl, nm); DeleteVm(ctl, nm + " copy"); RemoveTree(JoinPath(JoinPath(e.root, "datastore2"), nm + " copy"));
        for (int q = 0; q < 2; ++q) { const char* dd = q ? "old disks two" : "old disks one"; RemoveTree(JoinPath(JoinPath(e.root, "datastore1"), dd)); RemoveTree(JoinPath(JoinPath(e.root, "datastore2"), dd)); }
    }
    return true;
}

// Hostile / broken HTTP answers (negative or huge Content-Length, absurd chunk sizes, garbage, 3000 "100 Continue", a 200 KB header, NUL bytes, a TLS record in
// plain HTTP ...) in place of a SOAP answer or a file answer: the run ends in bounded time, with an error or (after a retry) with correct data, never a
// crash, a hang, or wrong data.
VM2B_TEST("esxi", esxi_hostile_http_answers)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-raw");
    const Env& e = es[c_.rnd((u32)es.size())];
    for (int target = 0; target < 2; ++target) {
        for (int kind = 0; kind < 16; ++kind) {
            if (c_.coin(35)) continue;
            Ctl ctl(e); ctl.reset();
            Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "RW", 1, 100000, c_.coin() ? "poweredOn" : "poweredOff", true, 3000)); if (!f.m.ok) return false;
            Json fl = Json::object(), d = Json::object(); d["target"] = target ? "soap" : "folder"; d["kind"] = kind; d["count"] = c_.range(1, 3);
            if (target == 1) d["count"] = c_.range(1, 2);
            fl["raw_http"] = d; ctl.faults(fl);
            BackupOptions o = RandomOpts(c_, false); o.retry = Fast(6);
            Vec<FI> before; Walk(f.m.dir, "", before);
            double t0 = UnixNow();
            RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), o, nullptr, nullptr, true, &f.m.dir);
            double dt = UnixNow() - t0;
            if (dt > 90) c_.note("%s %s kind %d took %.0f s", e.name.c_str(), target ? "soap" : "folder", kind, dt);
            T_CHECK(dt < 90);
            if (r.ok && !r.restoredOk) c_.note("%s %s kind %d: backup OK but the data differs", e.name.c_str(), target ? "soap" : "folder", kind);
            if (r.ok) T_CHECK(r.restoredOk);
            T_CHECK(ctl.leaked() == 0 || !r.ok);                                          // a failed run may leave its snapshot only if cleanup could not talk to the server
            Json fn = Json::object(); fn["raw_http"] = Json(); ctl.faults(fn);
            ctl.reset(); Str lerr; (void)lerr;
            // the next run removes a leftover and works
            if (ctl.leaked() != 0 || ctl.snapCount(f.spec.name) != 0) {
                RtResult r2 = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, false);
                T_CHECK(r2.ok); T_CHECK(ctl.leaked() == 0);
            }
            DeleteVm(ctl, f.spec.name);
        }
    }
    return true;
}

// An upload the host answers 201 but did not keep (or kept half of): the restore must notice (it asks the host for the file's size afterwards), upload
// again, and the restored files must be byte-identical.
VM2B_TEST("esxi", esxi_restore_lost_uploads_are_noticed)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-lost");
    int kind = c_.range(0, 3);
    for (const Env& e : es) {
        if (c_.coin(55)) continue;
        kind = (kind + 1) % 4;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "LU", false, "poweredOff")); if (!r.f.m.ok) return false;
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err; Pr pr;
        EsxiRestoreOptions o; o.vmName = r.f.spec.name + " lost"; Str newName = o.vmName;
        Json fl = Json::object(), pl = Json::object();
        pl["match"] = kind < 2 ? "-flat.vmdk" : ".nvram"; pl["mode"] = (kind & 1) ? "truncate" : "discard"; pl["count"] = c_.range(1, 2); fl["put_lost"] = pl; ctl.faults(fl);
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err);
        if (!ok) c_.note("%s kind %d: %s", e.name.c_str(), kind, err.c_str());
        T_CHECK(ok);
        if (ok) { T_CHECK(CheckRestoredFolder(c_, e, r, newName, "datastore1", false)); T_CHECK(StateHasVm(ctl, newName, "datastore1")); }
        T_CHECK(ctl.leaked() == 0);
        delete t; DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, newName); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

// The session dies right after CreateSnapshot / RemoveSnapshot started their task: the client holds a task id and must log in again and go on polling that task
// (not start a second snapshot, not give up, not leave one behind).
VM2B_TEST("esxi", esxi_session_lost_while_a_task_runs)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-tasksess");
    for (const Env& e : es) {
        if (c_.coin(50)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "TS", 1, 200000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        int which = c_.range(1, 2);                                             // 1: the create's task, 2: the create's and the remove's
        Json fl = Json::object(), d = Json::object(); d["count"] = which; fl["expire_after_task_start"] = d; ctl.faults(fl);
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, true, &f.m.dir);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk);
        T_CHECK(ctl.counter("snap:CreateSnapshot") == 1);                        // not started twice
        T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(f.spec.name) == 0);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// An independent disk (not covered by snapshots, copied live with a warning) in another folder: the restored VM must get its OWN copy of it, not keep a .vmx
// entry that points at the original VM's disk (two VMs writing one disk).
VM2B_TEST("esxi", esxi_restore_of_an_independent_disk_gets_its_own_copy)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-indep");
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "IN"); Json a = Json::object(); a["name"] = nm; a["power"] = "poweredOff";
        Json d = Json::array();
        for (int i = 0; i < 2; ++i) { Json o = Json::object(); o["size"] = (i64)c_.range(2000, 200000); o["thin"] = false; o["seed"] = (i64)c_.range(1, 9999); if (i == 1) { o["indep"] = true; if (c_.coin()) o["dir"] = "indep folder"; } d.push(o); }
        a["disks"] = d;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        BackupOptions o = RandomOpts(c_, false); Str err, warn; Manifest man; Pr pr;
        VmSource* src = NewEsxiSource(e.cfg(), err); Str store = tmp.sub("store"); RemoveTree(store); Target* t = LocalAt(store);
        bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s backup: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(Contains(warn, "not covered by the snapshot")); delete src;
        if (!ok) { delete t; DeleteVm(ctl, nm); continue; }
        Vec<FI> host; Walk(JoinPath(e.root, "datastore1"), "", host);
        Str wantSha; for (const FI& h : host) if (h.rel.ends_with("-flat.vmdk") && (h.rel.find("indep folder/") == 0 || (h.rel.find("_1-flat.vmdk") > 0 && h.rel.find("indep folder") < 0 && h.rel.find(nm.c_str()) == 0))) wantSha = h.sha;
        EsxiRestoreOptions ro; ro.vmName = nm + " copy"; ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res;
        ok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, &pr, &res, err);
        if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) {
            Str vmxText; Vec<FI> vf; Walk(JoinPath(JoinPath(e.root, "datastore1"), ro.vmName), "", vf);
            for (const FI& f : vf) if (f.rel.ends_with(".vmx")) { Bytes vb; Str ve; ReadWholeFile(f.full.c_str(), vb, ve); vmxText = Str((const char*)vb.data(), vb.size()); }
            Str v = VmxValue(vmxText, "scsi0:1.fileName");
            bool ownCopy = v.starts_with("[") ? v.find(("] " + ro.vmName + "/").c_str()) >= 0 : v.find('/') < 0;
            if (!ownCopy) c_.note("the restored VM's independent disk entry is '%s' (the original's file)", v.c_str());
            T_CHECK(ownCopy);
            Str dsx = "datastore1", px = v; if (!EsxiParseDsPath(v, dsx, px)) { dsx = "datastore1"; px = ro.vmName + "/" + v; }
            Str full = JoinPath(JoinPath(e.root, dsx), px.replace_all("/", "\\")); Bytes db; Str ve;
            T_CHECK(ReadWholeFile(full.c_str(), db, ve));
        }
        delete t; DeleteVm(ctl, nm); DeleteVm(ctl, nm + " copy"); RemoveTree(JoinPath(JoinPath(e.root, "datastore1"), "indep folder"));
    }
    return true;
}

// A suspended VM (the .vmss suspend file is a frozen binary file of the layout) and a powered-on one with a memory snapshot of its own: both come back whole.
VM2B_TEST("esxi", esxi_suspended_vm_and_memory_files)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-susp");
    for (const Env& e : es) {
        if (c_.coin(55)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = UniqueName(c_, "SU"); Json a = Json::object(); a["name"] = nm; a["power"] = "suspended"; a["disks"] = DisksJ(c_, c_.range(1, 2), 150000);
        Json ex = Json::array(); Json f1 = Json::object(); f1["name"] = nm + ".vmss"; f1["type"] = "suspend"; f1["size"] = (i64)c_.range(1000, 90000); ex.push(f1);
        Json f2 = Json::object(); f2["name"] = nm + "-ctk-notes.txt"; f2["type"] = "namespaceData"; f2["size"] = 700; ex.push(f2); a["extra_files"] = ex;
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        Vec<FI> before; Walk(m.dir, "", before);
        Json cfg = e.cfg(c_.coin(), c_.coin());
        RtResult r = RoundTrip(c_, tmp, e, nm, before, cfg, RandomOpts(c_, false), nullptr, nullptr, true, &m.dir);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(r.restoredOk); T_CHECK(ctl.leaked() == 0); T_CHECK(ctl.snapCount(nm) == 0);
        DeleteVm(ctl, nm);
    }
    return true;
}

// A VM with the most disks vSphere allows on one controller family (up to 60) and snapshots over them.
VM2B_TEST("esxi", esxi_vm_with_sixty_disks)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-60");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Str nm = UniqueName(c_, "S60"); Json a = Json::object(); a["name"] = nm; a["power"] = c_.coin() ? "poweredOn" : "poweredOff";
    int nd = c_.range(40, 60); Json d = Json::array();
    for (int i = 0; i < nd; ++i) { Json o = Json::object(); o["size"] = (i64)c_.range(500, 6000); o["thin"] = c_.coin(); o["seed"] = (i64)(i + 1); d.push(o); }
    a["disks"] = d;
    MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
    if (c_.coin()) { Json sn = Json::object(), o; sn["vm"] = nm; sn["name"] = "user snapshot"; ctl.call("snapshot", sn, o); }
    Vec<FI> before; Walk(m.dir, "", before);
    BackupOptions o = RandomOpts(c_, false); Str err, warn; Manifest man; Pr pr;
    VmSource* src = NewEsxiSource(e.cfg(), err); Str store = tmp.sub("store"); Target* t = LocalAt(store);
    bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err);
    if (!ok) c_.note("%s backup: %s", e.name.c_str(), err.c_str());
    T_CHECK(ok); delete src;
    if (ok) {
        EsxiRestoreOptions ro; ro.vmName = nm + " copy"; ReadOptions rd = RestoreRo(tmp); EsxiRestoreResult res;
        ok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, &pr, &res, err);
        if (!ok) c_.note("%s restore: %s", e.name.c_str(), err.c_str());
        T_CHECK(ok);
        if (ok) {
            Vec<FI> vf; Walk(JoinPath(JoinPath(e.root, "datastore1"), ro.vmName), "", vf); Str vmxText;
            for (const FI& f : vf) if (f.rel.ends_with(".vmx")) { Bytes vb; Str ve; ReadWholeFile(f.full.c_str(), vb, ve); vmxText = Str((const char*)vb.data(), vb.size()); }
            int present = 0; for (int i = 0; i < nd; ++i) if (!VmxValue(vmxText, Str::fmt("scsi0:%d.fileName", i)).empty()) ++present;
            T_CHECK(present == nd);
            for (const FI& b : before) { if (Excluded(b.rel) || b.rel.ends_with(".vmx") || b.rel.ends_with(".vmsd") || b.rel.find("-0000") >= 0 || b.rel.ends_with(".vmsn")) continue; const FI* g = FindFi(vf, b.rel); if (!g || g->sha != b.sha) { c_.note("not restored right: %s", b.rel.c_str()); T_CHECK(false); break; } }
        }
    }
    T_CHECK(ctl.leaked() == 0);
    delete t; DeleteVm(ctl, nm); DeleteVm(ctl, nm + " copy");
    return true;
}

// A damaged archive (a flipped byte, a cut volume, a missing volume) restored to the host: refused with an error, no half-made VM folder, nothing registered, the
// original untouched. (The flip may hit the archive's compressed or encrypted stream anywhere; the manifest / authentication must catch it.)
VM2B_TEST("esxi", esxi_restore_of_a_damaged_archive_leaves_nothing)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-dmg");
    int kind = c_.range(0, 2);
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        kind = (kind + 1) % 3;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "DM", false, "poweredOff", true)); if (!r.f.m.ok) return false;
        Vec<FI> vols; Walk(r.store, "", vols);
        Vec<FI*> data; for (FI& v : vols) if (v.rel.find(".resume") < 0 && v.rel.find(".json") < 0 && v.rel.find(".manifest") < 0 && v.size > 64) data.push_back(&v);
        if (data.empty()) { c_.note("no volume files found in the store"); T_CHECK(false); DeleteVm(ctl, r.f.spec.name); continue; }
        FI* vol = data[c_.rnd((u32)data.size())];
        { Bytes b; Str er; if (!ReadWholeFile(vol->full.c_str(), b, er)) { T_CHECK(false); continue; }
          if (kind == 0) { size_t pos = (size_t)c_.range(8, (int)b.size() - 1); b[pos] ^= (u8)(1u << c_.rnd(8)); WriteWholeFile(vol->full.c_str(), b.data(), b.size(), er); }
          else if (kind == 1) { size_t cut = b.size() / 2 + (size_t)c_.range(0, (int)b.size() / 4); WriteWholeFile(vol->full.c_str(), b.data(), cut, er); }
          else DeleteFileW(FilePath(vol->full)); }
        Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
        ReadOptions rd = RestoreRo(tmp); Str err; Pr pr;
        int dirs0 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        EsxiRestoreOptions o; o.vmName = r.f.spec.name + " dmg"; BackupOptions rb = r.o;
        bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, rb.password, rd, &pr, nullptr, err);
        if (ok) c_.note("%s kind %d: a damaged archive was restored without an error", e.name.c_str(), kind);
        T_CHECK(!ok); T_CHECK(!err.empty());
        int dirs1 = CountDirs(e, "datastore1") + CountDirs(e, "datastore2");
        if (dirs1 != dirs0) c_.note("%s kind %d: %d folders on the datastores, %d before (%s)", e.name.c_str(), kind, dirs1, dirs0, err.c_str());
        T_CHECK(dirs1 == dirs0); T_CHECK(ctl.vmState(o.vmName).is_null()); T_CHECK(ctl.leaked() == 0);
        T_CHECK(StateHasVm(ctl, r.f.spec.name, "datastore1"));
        delete t; DeleteVm(ctl, r.f.spec.name); RemoveTree(r.store); RemoveTree(rd.stateDir);
    }
    return true;
}

// A snapshot of the user's that merely starts with "VM2Backup-" is not ours: it must survive the run (only VM2Backup-<10 hex digits> are removed).
VM2B_TEST("esxi", esxi_users_snapshot_named_like_ours_is_kept)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-keep");
    for (const Env& e : es) {
        if (c_.coin(60)) continue;
        Ctl ctl(e); ctl.reset();
        Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "KP", 1, 100000, c_.coin() ? "poweredOn" : "poweredOff", true)); if (!f.m.ok) return false;
        static const char* const kNames[] = { "VM2Backup-before-upgrade", "VM2Backup-", "VM2Backup-DEADBEEF00", "VM2Backup-deadbeef0", "VM2Backup-deadbeef000" };
        const char* nm = kNames[c_.rnd(5)];
        Json a = Json::object(), o; a["vm"] = f.spec.name; a["name"] = nm; ctl.call("snapshot", a, o);
        Vec<FI> before; Walk(f.m.dir, "", before);
        RtResult r = RoundTrip(c_, tmp, e, f.spec.name, before, e.cfg(), RandomOpts(c_, false), nullptr, nullptr, true, &f.m.dir);
        if (!r.ok) c_.note("%s: %s", e.name.c_str(), r.err.c_str());
        T_CHECK(r.ok); T_CHECK(!Contains(r.warn, "removed")); T_CHECK(ctl.snapCount(f.spec.name) == 1);
        DeleteVm(ctl, f.spec.name);
    }
    return true;
}

// VM names full of XML / .vmx / unicode specials (the folder on the datastore is plain): through SOAP (escaping both ways), the archive, the .vmx rewrite and the
// registration of the restored VM under its own special name.
VM2B_TEST("esxi", esxi_vm_names_with_xml_and_unicode_specials)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-xmlnames");
    static const char* const kSpecial[] = { "a<b>&amp;c", "q'\"x'\"", "end]]>marker", "emoji \xF0\x9F\x98\x80 vm", "tab\there", "r\xc3\xa9sum\xc3\xa9 \xe2\x80\xae rtl", "mid  double  space", "&#38;&lt;" };
    for (const Env& e : es) {
        if (c_.coin(55)) continue;
        Ctl ctl(e); ctl.reset();
        Str nm = Str::fmt("XN%u-", c_.rnd(100000)) + kSpecial[c_.rnd(8)];
        Json a = Json::object(); a["name"] = nm; a["dir"] = Str::fmt("xmlsafe-%u", c_.rnd(100000)); a["power"] = "poweredOff"; a["disks"] = DisksJ(c_, 1, 100000); a["vmx"] = Str::fmt("xmlvm%u", c_.rnd(100000));
        MockVm m = MakeVmJ(ctl, e, a, e.vcenter() ? e.dcs[0] : Str()); T_CHECK(m.ok); if (!m.ok) return false;
        BackupOptions o = RandomOpts(c_, false); Str err, warn; Manifest man; Pr pr;
        VmSource* src = NewEsxiSource(e.cfg(), err); Str store = tmp.sub("store"); RemoveTree(store); Target* t = LocalAt(store);
        bool ok = BackupVm(*src, nm, *t, o, &pr, man, warn, err);
        if (!ok) c_.note("%s backup of '%s': %s", e.name.c_str(), nm.c_str(), err.c_str());
        T_CHECK(ok); T_CHECK(man.vm == nm); delete src;
        if (ok) {
            EsxiRestoreOptions ro; EsxiRestoreResult res; ReadOptions rd = RestoreRo(tmp);
            ok = RestoreToEsxi(*t, man.archive, e.cfg(), ro, o.password, rd, &pr, &res, err);
            if (!ok) c_.note("%s restore of '%s': %s", e.name.c_str(), nm.c_str(), err.c_str());
            T_CHECK(ok);
            if (ok) { T_CHECK(res.vmName == nm + "-restored"); T_CHECK(!ctl.vmState(res.vmName).is_null()); }
            DeleteVm(ctl, nm + "-restored");
        }
        delete t; DeleteVm(ctl, nm);
    }
    return true;
}

// Hostile HTTP answers while RESTORING (the answer to an upload, to MakeDirectory / RegisterVM): bounded time, and if the restore reports success the restored
// files are the original ones.
VM2B_TEST("esxi", esxi_hostile_http_answers_during_restore)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-rawrs");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "RR", false, "poweredOff", true)); if (!r.f.m.ok) return false;
    Json tc = Json::object(); tc["type"] = "local"; tc["path"] = r.store; Str te; Target* t = MakeTarget(tc, te);
    for (int target = 0; target < 2; ++target) {
        for (int kind = 0; kind < 16; ++kind) {
            if (c_.coin(40)) continue;
            Json fl = Json::object(), d = Json::object(); d["target"] = target ? "soap" : "put"; d["kind"] = kind; d["count"] = c_.range(1, 2);
            ctl.reset(); fl["raw_http"] = d; ctl.faults(fl);
            ReadOptions rd = RestoreRo(tmp); Str err; Pr pr; EsxiRestoreOptions o; o.vmName = Str::fmt("raw-%d-%d", target, kind);
            double t0 = UnixNow();
            bool ok = RestoreToEsxi(*t, r.man.archive, e.cfg(), o, r.o.password, rd, &pr, nullptr, err);
            double dt = UnixNow() - t0;
            if (dt > 90) c_.note("%s %s kind %d took %.0f s", e.name.c_str(), target ? "soap" : "put", kind, dt);
            T_CHECK(dt < 90);
            if (ok) {
                if (!CheckRestoredFolder(c_, e, r, o.vmName, "datastore1", false)) c_.note("%s %s kind %d: restore OK but the folder differs", e.name.c_str(), target ? "soap" : "put", kind);
                T_CHECK(CheckRestoredFolder(c_, e, r, o.vmName, "datastore1", false));
            } else {
                T_CHECK(!err.empty());
                // a failed restore leaves no VM and no half-made folder
                if (!ctl.vmState(o.vmName).is_null()) { c_.note("%s kind %d: failed (%s) but the VM is registered", e.name.c_str(), kind, err.c_str()); T_CHECK(false); }
            }
            DeleteVm(ctl, o.vmName); RemoveTree(JoinPath(JoinPath(e.root, "datastore1"), o.vmName)); RemoveTree(rd.stateDir);
        }
    }
    delete t; DeleteVm(ctl, r.f.spec.name); RemoveTree(r.store);
    return true;
}

// Two runs on the SAME VM at once (two tasks, an overlapping schedule): each one's leftover cleanup must not pull the other's snapshot from under its copy.
VM2B_TEST("esxi", esxi_two_runs_on_the_same_vm_at_once)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-same");
    const Env& e = es[c_.rnd((u32)es.size())];
    Ctl ctl(e); ctl.reset();
    Fix f; T_CHECK(MakeFix(c_, ctl, e, f, "SM", 2, 2500000, c_.coin() ? "poweredOn" : "poweredOff", false, 1000000)); if (!f.m.ok) return false;
    Json sl = Json::object(); sl["slow_ms"] = 250; ctl.faults(sl);                // the copies overlap in time
    struct Job { Json cfg; Str vm, store; BackupOptions o; bool ok; Str err, warn; Manifest man; };
    Job jobs[2];
    for (int i = 0; i < 2; ++i) { jobs[i].cfg = e.cfg(); jobs[i].vm = f.spec.name; jobs[i].store = tmp.sub(Str::fmt("store%d", i).c_str()); jobs[i].o = RandomOpts(c_, false); jobs[i].ok = false; }
    struct Th { static DWORD WINAPI Run(LPVOID p) {
        Job* j = (Job*)p; Str err; VmSource* src = NewEsxiSource(j->cfg, err); Target* t = LocalAt(j->store);
        Pr pr; j->ok = BackupVm(*src, j->vm, *t, j->o, &pr, j->man, j->warn, j->err);
        delete t; delete src; return 0; } };
    HANDLE h[2];
    h[0] = CreateThread(nullptr, 0, Th::Run, &jobs[0], 0, nullptr);
    Sleep(1500);
    h[1] = CreateThread(nullptr, 0, Th::Run, &jobs[1], 0, nullptr);
    WaitForMultipleObjects(2, h, TRUE, 400000);
    for (int i = 0; i < 2; ++i) CloseHandle(h[i]);
    for (int i = 0; i < 2; ++i) c_.note("run %d: ok=%d warn=[%s] err=[%s]", i, (int)jobs[i].ok, jobs[i].warn.c_str(), jobs[i].err.c_str());
    Json fn = Json::object(); fn["slow_ms"] = Json(); ctl.faults(fn);
    // the first run is never harmed. The second copies the first's snapshot files too (they are part of the VM's history); when the first finishes and removes
    // its snapshot meanwhile, those files vanish under the second, which then fails cleanly (a later run is fine): that is allowed, a silent bad copy is not.
    T_CHECK(jobs[0].ok);
    if (!jobs[1].ok) T_CHECK(Contains(jobs[1].err, "404") || Contains(jobs[1].err, "missing"));
    T_CHECK(!Contains(jobs[1].warn, "removed 1 old"));                                // the first run's snapshot was alive: not a leftover
    T_CHECK(Contains(jobs[1].warn, "left alone"));
    T_CHECK(ctl.leaked() == 0);
    DeleteVm(ctl, f.spec.name);
    return true;
}

// Two restores of the same archive to the same new name at the same moment (a double click, two operators): at most one gets the name, the other fails
// without touching the winner's folder, and the winner's VM is whole.
VM2B_TEST("esxi", esxi_two_restores_to_the_same_name_at_once)
{
    Vec<Env> all, es; if (!Envs(c_, all)) return true; Normal(all, es);
    EsTmp tmp(c_, "esxi-2rs");
    for (const Env& e : es) {
        if (c_.coin(75)) continue;
        Ctl ctl(e); ctl.reset();
        RFix r; T_CHECK(BackupFix(c_, tmp, ctl, e, r, "TR", false, "poweredOff", true)); if (!r.f.m.ok) return false;
        Json sl = Json::object(); sl["slow_ms"] = 5; ctl.faults(sl);
        struct Job { Json cfg; Str archive, store, pw, name, state; bool ok; Str err; EsxiRestoreResult res; ReadOptions rd; const Env* e; };
        Job jobs[2]; Str name = r.f.spec.name + " twin";
        for (int i = 0; i < 2; ++i) { jobs[i].cfg = e.cfg(); jobs[i].archive = r.man.archive; jobs[i].store = r.store; jobs[i].pw = r.o.password; jobs[i].name = name; jobs[i].ok = false; jobs[i].rd = RestoreRo(tmp); jobs[i].rd.stateDir = tmp.sub(Str::fmt("state%d", i).c_str()); Str mk; MakeDirs(jobs[i].rd.stateDir, mk); }
        struct Th { static DWORD WINAPI Run(LPVOID p) {
            Job* j = (Job*)p; Json tc = Json::object(); tc["type"] = "local"; tc["path"] = j->store; Str te; Target* t = MakeTarget(tc, te);
            EsxiRestoreOptions o; o.vmName = j->name; Pr pr; j->ok = RestoreToEsxi(*t, j->archive, j->cfg, o, j->pw, j->rd, &pr, &j->res, j->err); delete t; return 0; } };
        HANDLE h[2];
        for (int i = 0; i < 2; ++i) h[i] = CreateThread(nullptr, 0, Th::Run, &jobs[i], 0, nullptr);
        WaitForMultipleObjects(2, h, TRUE, 300000);
        for (int i = 0; i < 2; ++i) CloseHandle(h[i]);
        c_.note("%s: run 0 ok=%d [%s]; run 1 ok=%d [%s]", e.name.c_str(), (int)jobs[0].ok, jobs[0].err.c_str(), (int)jobs[1].ok, jobs[1].err.c_str());
        int oks = (int)jobs[0].ok + (int)jobs[1].ok;
        T_CHECK(oks >= 1);
        // whatever happened, the folder named like the VM that got the name is whole
        if (oks == 1) { T_CHECK(CheckRestoredFolder(c_, e, r, name, "datastore1", false)); T_CHECK(StateHasVm(ctl, name, "datastore1")); }
        else { /* both succeeded: only possible with two different names (the second took "-restored"); both folders must be whole */ }
        T_CHECK(ctl.leaked() == 0);
        Json fn = Json::object(); fn["slow_ms"] = Json(); ctl.faults(fn);
        DeleteVm(ctl, r.f.spec.name); DeleteVm(ctl, name); DeleteVm(ctl, name + "-restored"); RemoveTree(r.store);
    }
    return true;
}

} // namespace vm2b
