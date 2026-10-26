// VMware ESXi / vCenter source, see esxi.h and docs/ESXI.md.
#include "esxi.h"
#include "http.h"
#include "xml.h"
#include "crypto.h"
#include "winutil.h"
#include "version.h"

namespace vm2b {

// ---- pure helpers -----------------------------------------------------------------------------------------------
bool EsxiParseDsPath(const Str& s, Str& ds, Str& path)
{
    if (s.empty() || s[0] != '[') return false;
    int e = s.find(']');
    if (e < 2) return false;
    ds = s.substr(1, (size_t)e - 1);
    size_t p = (size_t)e + 1;
    while (p < s.size() && s[p] == ' ') ++p;
    path = s.substr(p);
    return true;
}

Str EsxiUrlEscape(const Str& s)
{
    static const char* hex = "0123456789ABCDEF";
    Str o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

Str EsxiFolderUrl(const Str& ds, const Str& path, const Str& dc)
{
    return "/folder/" + EsxiUrlEscape(path) + "?dcPath=" + EsxiUrlEscape(dc) + "&dsName=" + EsxiUrlEscape(ds);
}

// A GET / HEAD that follows redirects to the same server (vCenter / a proxy may answer 301/302/303/307/308 for /folder). The body of a redirect is
// dropped, the Location is a path or an absolute URL of this very host and port; another host is a permanent error (its certificate is not the pinned one).
static bool FolderRequest(HttpClient& c, const char* method, const Str& url0, const HttpHeaders& h, HttpResponse& r, const Str& host, int port)
{
    Str url = url0;
    for (int hop = 0;; ++hop) {
        if (!c.request(method, url, h, nullptr, 0, r)) return false;
        int st = r.status;
        if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) return true;
        const Str* loc = r.headers.get("Location");
        if (!loc || loc->empty() || hop >= 5) return true;                       // the caller reports the 3xx
        Str l = *loc, next;
        if (l[0] == '/') next = l;
        else {
            int sch = l.find("://");
            if (sch <= 0) return true;
            Str rest = l.substr((size_t)sch + 3); int sl = rest.find('/');
            Str auth = sl >= 0 ? rest.substr(0, (size_t)sl) : rest; next = sl >= 0 ? rest.substr((size_t)sl) : Str("/");
            Str hh = auth; int pp = 0;
            int col = auth.rfind(':'); if (col > 0 && auth.find(']') < col) { hh = auth.substr(0, (size_t)col); pp = atoi(auth.c_str() + col + 1); }
            if (hh.lower() != host.lower() || (pp != 0 && pp != port) || (pp == 0 && port != 443)) {
                c.close(); c.err = "the datastore file is served from another host (" + auth + "): redirects to other hosts are not followed"; c.transient = false; c.certError = false; c.aborted = false;
                return false;
            }
        }
        r.body->close(); url = next;
    }
}

static i64 DaysFromCivil(i64 y, int m, int d)
{
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

i64 EsxiParseHttpDate(const Str& s)
{
    static const char* mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int c = s.find(',');
    Str t = (c >= 0 ? s.substr((size_t)c + 1) : s).trim();
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0; char mon[8] = {0};
    if (sscanf(t.c_str(), "%d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return -1;
    int m = 0;
    for (int i = 0; i < 12; ++i) if (!strncmp(mon, mons[i], 3)) m = i + 1;
    if (!m || day < 1 || day > 31 || year < 1970 || year > 9999 || hh > 23 || mm > 59 || ss > 60) return -1;
    return DaysFromCivil(year, m, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

int EsxiCompareVersion(const Str& a, const Str& b)
{
    const char* p = a.c_str(); const char* q = b.c_str();
    for (;;) {
        long x = 0, y = 0;
        bool ha = *p != 0, hb = *q != 0;
        if (!ha && !hb) return 0;
        if (ha) { x = strtol(p, (char**)&p, 10); while (*p && *p != '.') ++p; if (*p == '.') ++p; }
        if (hb) { y = strtol(q, (char**)&q, 10); while (*q && *q != '.') ++q; if (*q == '.') ++q; }
        if (x != y) return x < y ? -1 : 1;
    }
}

namespace {

static i64 I64(const Str& s) { return (i64)strtoll(s.c_str(), nullptr, 10); }
static bool IsTrue(const Str& s) { return s == "true" || s == "1"; }
static Str Tag(const char* tag, const Str& text) { return Str("<") + tag + ">" + XmlEscape(text) + "</" + tag + ">"; }
static Str Ref(const char* tag, const char* type, const Str& id) { return Str("<") + tag + " type=\"" + type + "\">" + XmlEscape(id) + "</" + tag + ">"; }

// One line for an error text: no line breaks, bounded.
static Str OneLine(const Str& s)
{
    Str o;
    for (size_t i = 0; i < s.size() && o.size() < 400; ++i) { char c = s[i]; o += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c; }
    return o.trim();
}

static const char* kEnvelopeOpen =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<soapenv:Envelope xmlns:soapenc=\"http://schemas.xmlsoap.org/soap/encoding/\" xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><soapenv:Body>";
static const char* kEnvelopeClose = "</soapenv:Body></soapenv:Envelope>";

// ---- decoded vSphere data ----------------------------------------------------------------------------------------
struct Prop { Str name; XmlNode val; };
struct ObjData {
    Str type, id; Vec<Prop> props;
    const XmlNode* get(const char* n) const { for (size_t i = 0; i < props.size(); ++i) if (props[i].name == n) return &props[i].val; return nullptr; }
    Str text(const char* n, const char* def = "") const { const XmlNode* v = get(n); return v ? v->text.trim() : Str(def); }
};

struct LxFile { i64 key; Str name, type; u64 size; LxFile() : key(-1), size(0) {} };
struct LxDisk { i64 key; Vec<Vec<i64>> chain; LxDisk() : key(-1) {} };
struct LxSnap { Str key; i64 dataKey, memoryKey; Vec<LxDisk> disks; LxSnap() : dataKey(-1), memoryKey(-1) {} };
struct Layout { Vec<LxFile> files; Vec<LxDisk> disks; Vec<LxSnap> snaps; bool have; Layout() : have(false) {}
    const LxFile* file(i64 key) const { for (const LxFile& f : files) if (f.key == key) return &f; return nullptr; } };
struct SnapInfo { Str ref, name, id, desc; bool quiesced; SnapInfo() : quiesced(false) {} };
struct DiskDev { i64 key; Str label, backing, mode; u64 capacity; bool thin; DiskDev() : key(-1), capacity(0), thin(false) {} };

static void ParseFileInfo(const XmlNode& n, LxFile& f)
{
    f.key = I64(n.childText("key")); f.name = n.childText("name"); f.type = n.childText("type"); f.size = (u64)I64(n.childText("size"));
}

static void ParseDiskLayout(const XmlNode& n, LxDisk& d)
{
    d.key = I64(n.childText("key"));
    for (const XmlNode& c : n.kids) {
        if (c.name != "chain") continue;
        Vec<i64> keys;
        for (const XmlNode& k : c.kids) if (k.name == "fileKey") keys.push_back(I64(k.text));
        d.chain.push_back(keys);
    }
}

static void ParseLayout(const XmlNode& n, Layout& lx)
{
    lx = Layout();
    for (const XmlNode& c : n.kids) {
        if (c.name == "file") { LxFile f; ParseFileInfo(c, f); lx.files.push_back(f); }
        else if (c.name == "disk") { LxDisk d; ParseDiskLayout(c, d); lx.disks.push_back(d); }
        else if (c.name == "snapshot") {
            LxSnap s; s.key = c.childText("key").trim();
            s.dataKey = c.child("dataKey") ? I64(c.childText("dataKey")) : -1;
            s.memoryKey = c.child("memoryKey") ? I64(c.childText("memoryKey")) : -1;
            for (const XmlNode& d : c.kids) if (d.name == "disk") { LxDisk dd; ParseDiskLayout(d, dd); s.disks.push_back(dd); }
            lx.snaps.push_back(s);
        }
    }
    lx.have = true;
}

static void FlattenSnaps(const XmlNode& list, Vec<SnapInfo>& out, int depth = 0)
{
    if (depth > 120) return;
    SnapInfo s;
    s.ref = list.childText("snapshot").trim(); s.name = list.childText("name"); s.id = list.childText("id").trim(); s.desc = list.childText("description"); s.quiesced = IsTrue(list.childText("quiesced").trim());
    if (!s.ref.empty()) out.push_back(s);
    for (const XmlNode& c : list.kids) if (c.name == "childSnapshotList") FlattenSnaps(c, out, depth + 1);
}

static void ParseSnapshotInfo(const XmlNode& val, Vec<SnapInfo>& out)
{
    out.clear();
    for (const XmlNode& c : val.kids) if (c.name == "rootSnapshotList") FlattenSnaps(c, out);
}

static void ParseDevices(const XmlNode& val, Vec<DiskDev>& out)
{
    out.clear();
    for (const XmlNode& d : val.kids) {
        if (d.xsiType() != "VirtualDisk") continue;
        DiskDev k; k.key = I64(d.childText("key"));
        const XmlNode* di = d.child("deviceInfo"); if (di) k.label = di->childText("label");
        const XmlNode* b = d.child("backing");
        if (b) { k.backing = b->childText("fileName"); k.mode = b->childText("diskMode"); k.thin = IsTrue(b->childText("thinProvisioned").trim()); }
        if (d.child("capacityInBytes")) k.capacity = (u64)I64(d.childText("capacityInBytes"));
        else k.capacity = (u64)I64(d.childText("capacityInKB")) * 1024;
        out.push_back(k);
    }
}

static Str SafeComponent(const Str& vm)
{
    Str o;
    for (size_t i = 0; i < vm.size(); ++i) { u8 c = (u8)vm[i]; o += (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : (char)c; }
    o = TruncateUtf8(o, 200);
    while (o.ends_with(".") || o.ends_with(" ")) o = o.substr(0, o.size() - 1);
    size_t n = 0; while (n < o.size() && o[n] != '.') ++n;
    while (n > 0 && o[n - 1] == ' ') --n;
    if (n == 3 || n == 4) {
        Str b = o.substr(0, n).lower();
        if (b == "con" || b == "prn" || b == "aux" || b == "nul" || (n == 4 && (b.starts_with("com") || b.starts_with("lpt")) && b[3] >= '0' && b[3] <= '9')) o = "_" + o;
    }
    return o.empty() ? Str("vm") : o;
}

// every component of an archive path made safe (a file named after a VM called "a:b" would otherwise make the backup impossible to restore)
static Str SafeArcPath(const Str& p)
{
    Str out; size_t i = 0;
    while (i <= p.size()) {
        size_t j = i; while (j < p.size() && p[j] != '/') ++j;
        Str c = p.substr(i, j - i);
        if (!out.empty() || i > 0) out += '/';
        out += c.empty() ? c : SafeComponent(c);
        i = j + 1;
    }
    return out;
}

// files whose content cannot legitimately change while the snapshot holds them, and files that are never text
static bool IsFrozenType(const Str& t) { return t == "diskExtent" || t == "snapshotData" || t == "snapshotMemory" || t == "suspend" || t == "suspendMemory"; }
static bool IsBinaryType(const Str& t) { return IsFrozenType(t) || t == "nvram"; }

// "VM2Backup-" + 10 hex digits is what this program names its snapshots; anything else with that prefix is somebody's own and is never touched
static bool IsOurSnapshotName(const Str& n)
{
    if (n.size() != 20 || !n.starts_with("VM2Backup-")) return false;
    for (size_t i = 10; i < 20; ++i) { char c = n[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false; }
    return true;
}

static const char* const kSnapDesc = "Created by VM2Backup, removed automatically when the backup ends";
// A run that is working marks its snapshot "[live <unix time> <computer> <pid>]" and renews the mark every couple of minutes; another run that finds a snapshot
// of ours with a fresh mark leaves it alone (it is somebody's copy in progress, not a leftover) - unless the mark is from this computer and that process is gone
// (a crashed run: its snapshot is removed at once). A run that ends (or pauses) takes the mark away.
static Str ThisComputer()
{
    char n[MAX_COMPUTERNAME_LENGTH + 2]; DWORD k = (DWORD)sizeof(n);
    Str out; if (GetComputerNameA(n, &k)) out = Str(n, (size_t)k);
    Str r; for (size_t i = 0; i < out.size(); ++i) r += (out[i] == ' ' || out[i] == ']' || out[i] == '[') ? '_' : out[i];
    return r.empty() ? Str("host") : r;
}
// the runs of this process that are between begin() and end(): a mark with this process's id but a token that is not here belongs to a run that is gone
static CRITICAL_SECTION g_liveLock; static bool g_liveInit = false; static Vec<Str> g_liveTokens;
static void LiveLockInit() { static LONG once = 0; if (InterlockedCompareExchange(&once, 1, 0) == 0) { InitializeCriticalSection(&g_liveLock); g_liveInit = true; } while (!g_liveInit) Sleep(1); }
static void LiveRegister(const Str& tok, bool on)
{
    LiveLockInit(); EnterCriticalSection(&g_liveLock);
    for (size_t i = 0; i < g_liveTokens.size(); ++i) if (g_liveTokens[i] == tok) { g_liveTokens.erase(i); break; }
    if (on) g_liveTokens.push_back(tok);
    LeaveCriticalSection(&g_liveLock);
}
static bool LiveRegistered(const Str& tok)
{
    LiveLockInit(); EnterCriticalSection(&g_liveLock);
    bool r = false; for (const Str& t : g_liveTokens) if (t == tok) r = true;
    LeaveCriticalSection(&g_liveLock);
    return r;
}
static Str LiveMark(const Str& tok) { return Str::fmt(" [live %lld ", (long long)UnixNow()) + ThisComputer() + Str::fmt(" %lu ", (unsigned long)GetCurrentProcessId()) + tok + "]"; }
static bool SnapshotIsLive(const Str& desc)
{
    int p = desc.find("[live "); if (p < 0) return false;
    const char* c = desc.c_str() + p + 6;
    long long t = atoll(c);
    double d = UnixNow() - (double)t;
    if (t <= 0 || d >= 600 || d <= -600) return false;
    // "<time> <computer> <pid> <token>]"
    const char* sp1 = strchr(c, ' '); if (!sp1) return true;
    const char* sp2 = strchr(sp1 + 1, ' '); const char* br = strchr(c, ']');
    if (!sp2 || !br || sp2 > br) return true;
    Str host(sp1 + 1, (size_t)(sp2 - sp1 - 1)); unsigned long pid = strtoul(sp2 + 1, nullptr, 10);
    const char* sp3 = strchr(sp2 + 1, ' '); Str tok; if (sp3 && sp3 < br) tok = Str(sp3 + 1, (size_t)(br - sp3 - 1));
    if (host != ThisComputer() || pid == 0) return true;
    if (pid == (unsigned long)GetCurrentProcessId()) return tok.empty() || LiveRegistered(tok);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;                  // gone: a crashed run
    DWORD code = 0; bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

struct FileItem { Str name, ds, path, arc, origArc, type; u64 size; i64 mtime; int group; FileItem() : size(0), mtime(0), group(0) {} };

struct VmData {
    Str id, name, power, conn, uuid, instUuid, guest, vmPath, dc;
    bool isTemplate, haveConfig, encrypted; i64 cpu, mem;
    Layout lx; Vec<SnapInfo> snaps; Vec<DiskDev> disks;
    VmData() : isTemplate(false), haveConfig(false), encrypted(false), cpu(0), mem(0) {}
};

// ---- the source -------------------------------------------------------------------------------------------------
class RestoreImpl;

class Impl : public EsxiSource {
    friend class RestoreImpl;
public:
    explicit Impl(const Json& cfg) : m_cfg(cfg), m_soap(nullptr), m_files(nullptr), m_abort(0), m_haveContent(false), m_loggedIn(false), m_status(0), m_hasFault(false),
        m_noEx(false), m_begun(false), m_reused(false), m_skippedLive(0), m_hbTick(0), m_metaTime(0) {
        m_vcenter = cfg.get("type").str() == "vcenter";
        m_host = cfg.get("host").str(); m_port = (int)cfg.get("port").integer(443); m_user = cfg.get("user").str(); m_password = cfg.get("password").str();
        m_dcCfg = cfg.get("datacenter").str();
        const Json& sn = cfg.get("snapshot");
        m_quiesce = sn.is_object() ? sn.get("quiesce").boolean(true) : true;
        m_memory = sn.is_object() ? sn.get("memory").boolean(false) : false;
        m_taskTimeout = (int)cfg.get("task_timeout").integer(600); m_removeTimeout = (int)cfg.get("remove_timeout").integer(3600);
        m_pollMs = (int)cfg.get("poll_ms").integer(150);
        m_ver = "6.0";
    }
    ~Impl() override { logout(); LiveRegister(m_runToken, false); delete m_soap; delete m_files; }

    const char* kind() const override { return "esxi"; }
    void requestAbort() override { InterlockedExchange(&m_abort, 1); }
    Str serverInfo() const override { if (!m_haveContent) return Str(); return m_fullName + " (api " + m_apiVersion + ")"; }
    Str endProblem() const override { return m_leaked; }

    bool list(Vec<VmInfo>& vms) override {
        vms.clear();
        if (!ensureSession()) return false;
        Vec<VmData> all;
        bool ok = collectVms(all, true);
        Str e = err; bool t = transient;
        logout();
        if (!ok) return fail(e, t);
        for (const VmData& v : all) {
            if (v.isTemplate || !v.haveConfig) continue;
            VmInfo i; i.name = v.name;
            i.state = v.power == "poweredOn" ? "running" : v.power == "poweredOff" ? "stopped" : v.power == "suspended" ? "suspended" : v.power;
            for (const LxFile& f : v.lx.files) i.size += f.size;
            Str ds, p; EsxiParseDsPath(v.vmPath, ds, p);
            i.extra = v.guest;
            if (v.cpu) { if (!i.extra.empty()) i.extra += ", "; i.extra += Str::fmt("%lld vCPU", (long long)v.cpu); }
            if (v.mem) { if (!i.extra.empty()) i.extra += ", "; i.extra += Str::fmt("%lld MB RAM", (long long)v.mem); }
            if (!ds.empty()) { if (!i.extra.empty()) i.extra += ", "; i.extra += "datastore " + ds; }
            if (m_vcenter && !v.dc.empty()) { if (!i.extra.empty()) i.extra += ", "; i.extra += "datacenter " + v.dc; }
            vms.push_back(i);
        }
        return true;
    }

    bool begin(const Str& vm, bool allowLive, const Str& resumeToken, Str& fingerprint, Str& token, Str& warning) override;
    bool pack(TarWriter& tar, Progress* pr, Str& warning) override;
    void end(bool keepForResume) override;

private:
    // ---- plumbing ------------------------------------------------------------------------------------------------
    bool failHttp(HttpClient& c) {
        if (c.aborted) return fail(kAbortedMessage);
        return fail(c.err.empty() ? Str("network error") : c.err, c.transient);
    }
    bool aborted() const { return m_abort != 0; }

    void makeClients() {
        if (m_soap) return;
        HttpOptions o = HttpOptions::FromJson(m_cfg);
        o.tls = true; o.port = (int)m_cfg.get("port").integer(443); o.host = m_host;
        o.tlsOpt.abort = &m_abort;
        m_soap = new HttpClient(o);
        m_files = new HttpClient(o);
    }

    // One SOAP call. On success `resp` points at the <XxxResponse> element inside m_doc (valid until the next call).
    bool call(const char* method, const Str& inner, const XmlNode*& resp, bool relogin = true) {
        resp = nullptr; int busy = 0;
        for (int attempt = 0;; ++attempt) {
            if (aborted()) return fail(kAbortedMessage);
            Str env = kEnvelopeOpen;
            env += Str("<") + method + " xmlns=\"urn:vim25\">" + inner + "</" + method + ">";
            env += kEnvelopeClose;
            HttpHeaders h; h.add("Content-Type", "text/xml; charset=utf-8"); h.add("SOAPAction", "\"urn:vim25/" + m_ver + "\"");
            HttpResponse r; Bytes out;
            if (!m_soap->requestBytes("POST", "/sdk", h, env.data(), env.size(), r, out)) return failHttp(*m_soap);
            m_status = r.status; m_hasFault = false; m_faultType.clear(); m_faultMsg.clear();
            Str xerr; bool parsed = !out.empty() && XmlParse((const char*)out.data(), out.size(), m_doc, xerr);
            const XmlNode* body = parsed ? m_doc.child("Body") : nullptr;
            const XmlNode* fault = body ? body->child("Fault") : nullptr;
            if (fault) {
                m_hasFault = true;
                m_faultMsg = OneLine(fault->childText("faultstring"));
                const XmlNode* det = fault->child("detail");
                if (det && !det->kids.empty()) {
                    const XmlNode& d = det->kids[0];
                    m_faultType = d.xsiType();
                    if (m_faultType.empty()) { m_faultType = d.name; if (m_faultType.ends_with("Fault") && m_faultType.size() > 5) m_faultType = m_faultType.substr(0, m_faultType.size() - 5); }
                }
                if (m_faultType == "NotAuthenticated" && relogin && attempt == 0 && m_loggedIn && strcmp(method, "Login") != 0) {
                    m_loggedIn = false;
                    if (!login()) return false;
                    continue;
                }
                return fail(m_faultMsg.empty() ? m_faultType : m_faultMsg, false);
            }
            if (r.status == 503) {
                if (busy++ < 3 && !aborted()) { Sleep(300 * (DWORD)busy); --attempt; continue; }        // hostd restarting / overloaded: a short wait often cures it
                return fail("the server is busy (HTTP 503)", true);
            }
            if (r.status != 200) return fail(Str::fmt("unexpected HTTP %d %s from the SOAP endpoint", r.status, OneLine(r.reason).c_str()), r.status >= 500 && r.status != 501);
            if (!parsed || !body || body->kids.empty()) return fail("the server's answer is not a SOAP response" + (xerr.empty() ? Str() : " (" + xerr + ")"));
            resp = &body->kids[0];
            return true;
        }
    }

    bool negotiate() {
        static const char* vers[] = { "6.0", "6.5", "6.7", "7.0", "8.0", "9.0" };
        Str last;
        for (const char* v : vers) {
            m_ver = v;
            const XmlNode* r;
            if (call("RetrieveServiceContent", Ref("_this", "ServiceInstance", "ServiceInstance"), r, false)) {
                const XmlNode* rv = r->child("returnval");
                if (!rv) return fail("RetrieveServiceContent returned nothing");
                m_rootFolder = rv->childText("rootFolder").trim(); m_pc = rv->childText("propertyCollector").trim();
                m_viewMgr = rv->childText("viewManager").trim(); m_sessMgr = rv->childText("sessionManager").trim(); m_fileMgr = rv->childText("fileManager").trim();
                const XmlNode* ab = rv->child("about");
                if (ab) { m_fullName = ab->childText("fullName"); m_apiVersion = ab->childText("apiVersion").trim(); m_apiType = ab->childText("apiType").trim(); m_product = ab->childText("productLineId").trim(); }
                if (m_rootFolder.empty() || m_pc.empty() || m_viewMgr.empty() || m_sessMgr.empty()) return fail("unexpected ServiceContent (no inventory managers)");
                m_haveContent = true;
                return true;
            }
            last = err;
            // only an answer from the SOAP layer (a fault or an HTTP error) can mean "this version string is refused"; a lost connection cannot
            if (!(m_hasFault || (m_status >= 400 && m_status < 600 && m_status != 503))) return false;
        }
        return fail(last + " (no SOAP version string was accepted: tried 6.0 .. 9.0)");
    }

    bool login() {
        const XmlNode* r;
        Str in = Ref("_this", "SessionManager", m_sessMgr) + Tag("userName", m_user) + Tag("password", m_password);
        if (!call("Login", in, r, false)) {
            if (m_hasFault && (m_faultType == "InvalidLogin" || m_faultMsg.find("incorrect user name") >= 0)) return fail("login failed: the user name or password was refused by the server");
            if (m_hasFault) return fail("login failed: " + err);
            return false;
        }
        m_loggedIn = true;
        Str c = m_soap->getCookie("vmware_soap_session");
        m_files->clearCookies();
        if (!c.empty()) m_files->setCookie("vmware_soap_session", c);
        else m_files->setBasicAuth(m_user, m_password);
        return true;
    }

    void logout() {
        if (!m_soap || !m_loggedIn) return;
        const XmlNode* r;
        long a = m_abort; m_abort = 0;
        call("Logout", Ref("_this", "SessionManager", m_sessMgr), r, false);
        if (a) m_abort = a;
        m_loggedIn = false;
        m_soap->clearCookies(); m_files->clearCookies();
        m_soap->close(); m_files->close();
    }

    bool ensureSession() {
        makeClients();
        if (!m_haveContent && !negotiate()) return false;
        if (!m_loggedIn && !login()) return false;
        return true;
    }

    // ---- property collector ----------------------------------------------------------------------------------------
    static Str PropSet(const char* type, const char* const* paths, size_t n) {
        Str s = "<propSet>" + Tag("type", type) + "<all>false</all>";
        for (size_t i = 0; i < n; ++i) s += Tag("pathSet", paths[i]);
        return s + "</propSet>";
    }
    static Str ViewSelect(const Str& view) {
        return "<objectSet>" + Ref("obj", "ContainerView", view) + "<skip>true</skip><selectSet xsi:type=\"TraversalSpec\"><name>traverseEntities</name>"
               "<type>ContainerView</type><path>view</path><skip>false</skip></selectSet></objectSet>";
    }
    static Str ObjSelect(const char* type, const Str& id) { return "<objectSet>" + Ref("obj", type, id) + "<skip>false</skip></objectSet>"; }

    static void ParseObjects(const XmlNode& n, const char* objName, Vec<ObjData>& out) {
        for (const XmlNode& o : n.kids) {
            if (o.name != objName) continue;
            ObjData d;
            const XmlNode* ob = o.child("obj"); if (!ob) continue;
            d.type = ob->attr("type"); d.id = ob->text.trim();
            for (const XmlNode& ps : o.kids) {
                if (ps.name != "propSet") continue;
                Prop p; p.name = ps.childText("name").trim();
                const XmlNode* v = ps.child("val");
                if (v) p.val = *v;
                d.props.push_back(p);
            }
            out.push_back(d);
        }
    }

    bool retrieve(const Str& propSets, const Str& objSets, Vec<ObjData>& out) {
        out.clear();
        Str spec = "<specSet>" + propSets + objSets + "</specSet>";
        const XmlNode* r;
        if (!m_noEx) {
            Str in = Ref("_this", "PropertyCollector", m_pc) + spec + "<options><maxObjects>100</maxObjects></options>";
            if (call("RetrievePropertiesEx", in, r)) {
                const XmlNode* rv = r->child("returnval");
                int guard = 0;
                while (rv) {
                    ParseObjects(*rv, "objects", out);
                    Str tok = rv->childText("token").trim();
                    if (tok.empty() || ++guard > 10000) break;
                    if (!call("ContinueRetrievePropertiesEx", Ref("_this", "PropertyCollector", m_pc) + Tag("token", tok), r)) return false;
                    rv = r->child("returnval");
                }
                return true;
            }
            // transient, auth and abort problems are final; a fault from an old / odd server falls back to the older call
            if (!m_hasFault || err == kAbortedMessage || m_faultType == "NotAuthenticated" || m_faultType == "InvalidProperty" || m_faultType == "ManagedObjectNotFound") return false;
            out.clear();
        }
        Str in = Ref("_this", "PropertyCollector", m_pc) + spec;
        if (!call("RetrieveProperties", in, r)) return false;
        m_noEx = true;
        ParseObjects(*r, "returnval", out);
        return true;
    }

    bool createView(const char* ctype, const Str& cid, const char* vtype, Str& view) {
        const XmlNode* r;
        Str in = Ref("_this", "ViewManager", m_viewMgr) + Ref("container", ctype, cid) + Tag("type", vtype) + "<recursive>true</recursive>";
        if (!call("CreateContainerView", in, r)) return false;
        const XmlNode* rv = r->child("returnval");
        if (!rv || rv->text.trim().empty()) return fail("CreateContainerView returned no view");
        view = rv->text.trim();
        return true;
    }
    void destroyView(const Str& view) {
        const XmlNode* r;
        Str e = err; bool t = transient;
        call("DestroyView", Ref("_this", "ContainerView", view), r);
        err = e; transient = t;
    }

    struct Dc { Str id, name; };
    bool datacenters(Vec<Dc>& out) {
        out.clear();
        Str view; if (!createView("Folder", m_rootFolder, "Datacenter", view)) return false;
        static const char* p[] = { "name" };
        Vec<ObjData> objs;
        bool ok = retrieve(PropSet("Datacenter", p, 1), ViewSelect(view), objs);
        destroyView(view);
        if (!ok) return false;
        for (const ObjData& o : objs) {
            Dc d; d.id = o.id; d.name = o.text("name");
            if (!m_dcCfg.empty() && d.name != m_dcCfg) continue;
            out.push_back(d);
        }
        if (out.empty()) return fail(m_dcCfg.empty() ? Str("the server has no datacenter") : "datacenter not found: " + m_dcCfg);
        return true;
    }

    static void FillVm(const ObjData& o, VmData& v) {
        v.id = o.id; v.name = o.text("name"); v.power = o.text("runtime.powerState"); v.conn = o.text("runtime.connectionState");
        v.isTemplate = IsTrue(o.text("config.template")); v.uuid = o.text("config.uuid"); v.instUuid = o.text("config.instanceUuid");
        v.guest = o.text("config.guestFullName"); v.vmPath = o.text("config.files.vmPathName");
        v.cpu = I64(o.text("config.hardware.numCPU")); v.mem = I64(o.text("config.hardware.memoryMB"));
        v.haveConfig = !v.vmPath.empty();
        v.encrypted = o.get("config.keyId") != nullptr;                      // vSphere VM Encryption: the files on the datastore are ciphertext
        const XmlNode* n = o.get("layoutEx"); if (n) ParseLayout(*n, v.lx);
        if (!v.lx.have) {
            n = o.get("layoutEx.file");
            if (n) { for (const XmlNode& f : n->kids) { LxFile lf; ParseFileInfo(f, lf); v.lx.files.push_back(lf); } v.lx.have = true; }
        }
        n = o.get("snapshot"); if (n) ParseSnapshotInfo(*n, v.snaps);
        n = o.get("config.hardware.device"); if (n) ParseDevices(*n, v.disks);
    }

    // The properties of VMs: `forList` = what the list needs, otherwise what a backup needs
    bool hasKeyId() const { int a = atoi(m_apiVersion.c_str()), b = 0; int d = m_apiVersion.find('.'); if (d > 0) b = atoi(m_apiVersion.c_str() + d + 1); return a > 6 || (a == 6 && b >= 5); }   // VirtualMachineConfigInfo.keyId exists since 6.5
    Str VmPropSet(bool forList) const {
        static const char* common[] = { "name", "runtime.powerState", "runtime.connectionState", "config.template", "config.uuid", "config.instanceUuid", "config.guestFullName",
                                        "config.files.vmPathName" };
        static const char* listOnly[] = { "config.hardware.numCPU", "config.hardware.memoryMB", "layoutEx.file" };
        static const char* backupOnly[] = { "layoutEx", "snapshot", "config.hardware.device" };
        Vec<const char*> p;
        for (const char* s : common) p.push_back(s);
        if (hasKeyId()) p.push_back("config.keyId");
        if (forList) { for (const char* s : listOnly) p.push_back(s); } else { for (const char* s : backupOnly) p.push_back(s); }
        return PropSet("VirtualMachine", p.data(), p.size());
    }

    bool collectVms(Vec<VmData>& all, bool forList) {
        Vec<Dc> dcs; if (!datacenters(dcs)) return false;
        for (const Dc& dc : dcs) {
            Str view; if (!createView("Datacenter", dc.id, "VirtualMachine", view)) return false;
            Vec<ObjData> objs;
            bool ok = retrieve(VmPropSet(forList), ViewSelect(view), objs);
            destroyView(view);
            if (!ok) return false;
            for (const ObjData& o : objs) { VmData v; FillVm(o, v); v.dc = dc.name; all.push_back(v); }
        }
        return true;
    }

    bool findVm(const Str& name, VmData& out) {
        Vec<Dc> dcs; if (!datacenters(dcs)) return false;
        Vec<Dc> hit; Vec<Str> ids;
        for (const Dc& dc : dcs) {
            Str view; if (!createView("Datacenter", dc.id, "VirtualMachine", view)) return false;
            static const char* p[] = { "name" };
            Vec<ObjData> objs;
            bool ok = retrieve(PropSet("VirtualMachine", p, 1), ViewSelect(view), objs);
            destroyView(view);
            if (!ok) return false;
            for (const ObjData& o : objs) if (o.text("name") == name) { hit.push_back(dc); ids.push_back(o.id); }
        }
        if (hit.empty()) return fail("VM not found: " + name);
        if (hit.size() > 1 || ids.size() > 1) return fail("several VMs are named \"" + name + "\" (in different folders or datacenters): rename one of them, or set the datacenter of the server");
        return readVm(ids[0], hit[0].name, out);
    }

    bool readVm(const Str& id, const Str& dc, VmData& out, bool refresh = false) {
        const XmlNode* r;
        if (refresh) { call("RefreshStorageInfo", Ref("_this", "VirtualMachine", id), r); err.clear(); m_hasFault = false; }
        Vec<ObjData> objs;
        if (!retrieve(VmPropSet(false), ObjSelect("VirtualMachine", id), objs)) return false;
        if (objs.empty()) return fail("the VM disappeared from the inventory");
        out = VmData(); FillVm(objs[0], out); out.dc = dc;
        return true;
    }

    // ---- tasks ---------------------------------------------------------------------------------------------------
    struct TaskResult { bool ok; Str result, faultType, faultMsg; bool timedOut; TaskResult() : ok(false), timedOut(false) {} };

    bool waitTask(const Str& task, int timeoutSec, TaskResult& tr, bool honourAbort = true) {
        static const char* p[] = { "info" };
        DWORD t0 = GetTickCount(); int delay = m_pollMs < 20 ? 20 : m_pollMs; bool cancelled = false;
        for (;;) {
            Vec<ObjData> objs;
            long a = m_abort; if (!honourAbort) m_abort = 0;
            bool ok = retrieve(PropSet("Task", p, 1), ObjSelect("Task", task), objs);
            if (!honourAbort && a) m_abort = a;
            if (!ok) return false;
            const XmlNode* info = objs.empty() ? nullptr : objs[0].get("info");
            if (!info) return fail("the task has no state");
            Str st = info->childText("state").trim();
            if (st == "success") {
                tr.ok = true;
                const XmlNode* res = info->child("result"); if (res) tr.result = res->text.trim();
                return true;
            }
            if (st == "error") {
                tr.ok = false;
                const XmlNode* e = info->child("error");
                if (e) {
                    const XmlNode* f = e->child("fault"); if (f) tr.faultType = f->xsiType();
                    tr.faultMsg = OneLine(e->childText("localizedMessage"));
                    if (tr.faultMsg.empty() && f) tr.faultMsg = OneLine(f->childText("faultMessage"));
                }
                if (tr.faultMsg.empty()) tr.faultMsg = tr.faultType.empty() ? Str("the task failed") : tr.faultType;
                return true;
            }
            if (honourAbort && aborted() && !cancelled) {
                cancelled = true;
                const XmlNode* r; long a2 = m_abort; m_abort = 0; call("CancelTask", Ref("_this", "Task", task), r); m_abort = a2;
                t0 = GetTickCount(); timeoutSec = 30;                           // wait a little for the task to end after the cancel
            }
            if (cancelled && (int)((GetTickCount() - t0) / 1000) >= timeoutSec) return fail(kAbortedMessage);
            if (!cancelled && (int)((GetTickCount() - t0) / 1000) >= timeoutSec) {
                tr.timedOut = true;
                const XmlNode* r; call("CancelTask", Ref("_this", "Task", task), r);
                // a task that cannot be cancelled goes on: give it a moment to end, so that what it makes (a snapshot) exists when the caller cleans up
                for (DWORD g0 = GetTickCount(); (GetTickCount() - g0) < 30000; ) {
                    Sleep(m_pollMs < 100 ? 100 : (DWORD)m_pollMs);
                    Vec<ObjData> o2; long a = m_abort; m_abort = 0;
                    bool got = retrieve(PropSet("Task", p, 1), ObjSelect("Task", task), o2);
                    m_abort = a;
                    if (!got) break;
                    const XmlNode* i2 = o2.empty() ? nullptr : o2[0].get("info");
                    Str st2 = i2 ? i2->childText("state").trim() : Str();
                    if (st2 == "success" || st2 == "error" || !i2) break;
                }
                return fail(Str::fmt("timed out after %d s waiting for the task on the server", timeoutSec));
            }
            for (int w = 0; w < delay; w += 20) { if (!cancelled && aborted()) break; Sleep(20); }
            if (delay < 1500) delay = delay * 2 > 1500 ? 1500 : delay * 2;
        }
    }

    // Runs a method that returns a Task and waits for it. Faults raised by the call itself are returned like task errors.
    bool runTask(const char* method, const Str& inner, int timeoutSec, TaskResult& tr, bool honourAbort = true) {
        const XmlNode* r;
        tr = TaskResult();
        if (!call(method, inner, r)) {
            if (m_hasFault) { tr.ok = false; tr.faultType = m_faultType; tr.faultMsg = m_faultMsg; return true; }
            return false;
        }
        const XmlNode* rv = r->child("returnval");
        if (!rv || rv->text.trim().empty()) return fail(Str(method) + " returned no task");
        return waitTask(rv->text.trim(), timeoutSec, tr, honourAbort);
    }

    static bool IsQuiesceProblem(const Str& type, const Str& msg) {
        Str m = msg.lower();
        return type == "ToolsUnavailable" || type == "ApplicationQuiesceFault" || type == "QuiesceDatastoreIOFailed" || type == "GuestOperationsUnavailable" ||
               type == "SnapshotFault" || m.find("quiesc") >= 0 || m.find("vmware tools") >= 0;
    }
    static bool IsLicenseProblem(const Str& type, const Str& msg) {
        Str m = msg.lower();
        return type == "RestrictedVersion" || type == "NoPermission" || m.find("prohibits") >= 0 || (m.find("license") >= 0 && m.find("restrict") >= 0);
    }
    static Str LicenseMessage(const Str& what) {
        return what + " was refused by the server: the license of this ESXi host does not allow the write API (free ESXi) or the user lacks the snapshot privilege. "
                      "Use a licensed host (or vCenter), or the SSH transport of the server settings";
    }

    // Removes a snapshot and waits. A snapshot that is already gone counts as removed.
    // "not found" from RemoveSnapshot means the snapshot is gone - unless the server is wrong or confused: the tree is asked before that is believed
    bool snapshotStillListed(const Str& ref) {
        if (m_vmRef.empty()) return false;
        VmData v; Str e = err; bool t = transient;
        bool listed = false;
        if (readVm(m_vmRef, m_dc, v)) { for (const SnapInfo& s : v.snaps) if (s.ref == ref) listed = true; }
        err = e; transient = t;
        return listed;
    }
    bool removeSnapshot(const Str& ref) {
        TaskResult tr;
        Str in = Ref("_this", "VirtualMachineSnapshot", ref) + "<removeChildren>false</removeChildren><consolidate>true</consolidate>";
        if (!runTask("RemoveSnapshot_Task", in, m_removeTimeout, tr, false)) {
            if (m_hasFault && m_faultType == "ManagedObjectNotFound") {
                if (snapshotStillListed(ref)) return fail("removing the snapshot: the server says it does not exist, but still lists it", true);
                return true;
            }
            return false;
        }
        if (!tr.ok) {
            if (tr.faultType == "ManagedObjectNotFound" || tr.faultType == "NotFound") {
                if (snapshotStillListed(ref)) return fail("removing the snapshot: the server says it does not exist, but still lists it", true);
                return true;
            }
            return fail("removing the snapshot failed: " + tr.faultMsg);
        }
        return true;
    }

    // Removes every snapshot of this VM whose name starts with VM2Backup- (except `keepRef`), e.g. leftovers of a crashed run.
    bool removeLeftovers(const VmData& vm, const Str& keepRef, int& removed) {
        removed = 0; m_skippedLive = 0; bool all = true; Str firstErr; bool tr = false;
        for (const SnapInfo& s : vm.snaps) {
            if (!IsOurSnapshotName(s.name) || s.ref == keepRef) continue;
            if (SnapshotIsLive(s.desc)) { ++m_skippedLive; continue; }
            if (removeSnapshot(s.ref)) ++removed; else { all = false; if (firstErr.empty()) { firstErr = err; tr = transient; } }
        }
        if (!all) { err = firstErr; transient = tr; }
        return all;
    }

    // (un)marks our snapshot as being worked on; best effort, never changes err
    void markSnapshot(bool live) {
        if (m_snapRef.empty() || !m_soap) return;
        Str e = err; bool t = transient;
        const XmlNode* r;
        Str d = kSnapDesc; if (live) d += LiveMark(m_runToken);
        if (ensureSession()) call("RenameSnapshot", Ref("_this", "VirtualMachineSnapshot", m_snapRef) + Tag("name", m_snapName) + Tag("description", d), r);
        err = e; transient = t; m_hasFault = false;
        m_hbTick = GetTickCount();
    }

    // Best effort after a failed begin(): look for our snapshot by name and remove it.
    void cleanupByName(const Str& name, const Str& knownRef) {
        long a = m_abort; m_abort = 0;
        Str e = err; bool t = transient;
        Str ref = knownRef;
        if (ref.empty()) {
            VmData v;
            if (readVm(m_vmRef, m_dc, v)) for (const SnapInfo& s : v.snaps) if (s.name == name) ref = s.ref;
        }
        if (!ref.empty()) removeSnapshot(ref);
        err = e; transient = t;
        m_abort = a;
    }

    // ---- the file list -------------------------------------------------------------------------------------------
    bool statFile(FileItem& f, bool& relogged) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            HttpHeaders h; HttpResponse r;
            if (!FolderRequest(*m_files, "HEAD", EsxiFolderUrl(f.ds, f.path, m_dc), h, r, m_host, m_port)) return failHttp(*m_files);
            if ((r.status == 401 || r.status == 403) && attempt == 0 && !relogged) { relogged = true; m_loggedIn = false; if (!login()) return false; continue; }
            if (r.status == 404) return fail("the file is missing on the datastore: " + f.name);
            if (r.status == 200) {
                if (IsBinaryType(f.type) && r.headers.value("Content-Type").lower().find("text/html") >= 0) return fail("the datastore answered with an HTML page instead of the file: " + f.name, true);
                if (r.contentLength >= 0) {
                    // a binary file cannot be smaller on the datastore than the layout says it is: ask again, then refuse (a broken answer must not shrink the copy)
                    if (IsBinaryType(f.type) && f.size > 0 && (u64)r.contentLength < f.size) {
                        if (attempt == 0) { m_files->close(); Sleep(300); continue; }
                        return fail(Str::fmt("the datastore reports %lld bytes for %s, the VM's file layout says %llu", (long long)r.contentLength, f.name.c_str(), (unsigned long long)f.size), true);
                    }
                    f.size = (u64)r.contentLength;
                }
                const Str* lm = r.headers.get("Last-Modified");
                if (lm) { i64 t = EsxiParseHttpDate(*lm); if (t > 0) f.mtime = t; }
            }
            return true;                                        // HEAD not supported (405, 501...): the layout's size stays
        }
        return true;
    }

    bool buildFiles(const VmData& vm, const Str& snapRef, Vec<FileItem>& out, Str& warning, Json& ignored, Json& disksMeta, Json& snapFiles);

    // ---- state ---------------------------------------------------------------------------------------------------
    Json m_cfg;
    Str m_host, m_user, m_password, m_dcCfg, m_ver; int m_port;
    bool m_vcenter, m_quiesce, m_memory;
    int m_taskTimeout, m_removeTimeout, m_pollMs;
    HttpClient* m_soap; HttpClient* m_files;
    volatile long m_abort;
    bool m_haveContent, m_loggedIn;
    Str m_rootFolder, m_pc, m_viewMgr, m_sessMgr, m_fileMgr, m_fullName, m_apiVersion, m_apiType, m_product;
    XmlNode m_doc; int m_status; bool m_hasFault; Str m_faultType, m_faultMsg;
    bool m_noEx;
    // what begin() prepared
    bool m_begun, m_reused; int m_skippedLive; DWORD m_hbTick; Str m_runToken;
    Str m_vmRef, m_vmName, m_dc, m_snapRef, m_snapName, m_prefix, m_meta, m_leaked; i64 m_metaTime;
    Vec<FileItem> m_list;
};

bool Impl::buildFiles(const VmData& vm, const Str& snapRef, Vec<FileItem>& out, Str& warning, Json& ignored, Json& disksMeta, Json& snapFiles)
{
    out.clear();
    Str vmDs, vmPath; EsxiParseDsPath(vm.vmPath, vmDs, vmPath);
    Str vmDir; { int c = vmPath.rfind('/'); if (c >= 0) vmDir = vmPath.substr(0, (size_t)c); }
    const LxSnap* snap = nullptr;
    for (const LxSnap& s : vm.lx.snaps) if (s.key == snapRef) snap = &s;
    if (!snap) return fail("the server's file layout does not list the new snapshot (cannot tell which files it froze)");

    Vec<i64> keys; Vec<int> groups;
    auto add = [&](i64 k, int g) { for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return; keys.push_back(k); groups.push_back(g); };
    // configuration and snapshot description files
    for (const LxFile& f : vm.lx.files) {
        const Str& t = f.type;
        if (t == "config" || t == "extendedConfig" || t == "nvram" || t == "snapshotList" || t == "namespaceData") add(f.key, 0);
        else if (t == "snapshotData" || t == "snapshotMemory" || t == "suspend" || t == "suspendMemory") add(f.key, 1);
        else if (t == "diskDescriptor" || t == "diskExtent") {}                    // decided through the chains below
        else if (t == "swap" || t == "uwswap" || t == "log" || t == "stat" || t == "core" || t == "screenshot" || t == "digestDescriptor" || t == "digestExtent" ||
                 t == "diskReplicationState" || t == "ctkEnabledDigest") {}
        else { Json o = Json::object(); o["name"] = f.name; o["type"] = f.type; ignored.push(o); }
    }
    // disk chains of every snapshot: the files the snapshots froze (the running point's new delta is in none of them)
    for (const LxSnap& s : vm.lx.snaps) {
        for (const LxDisk& d : s.disks) for (const Vec<i64>& link : d.chain) for (i64 k : link) add(k, 2);
        if (s.dataKey >= 0) add(s.dataKey, 1);
        if (s.memoryKey >= 0) add(s.memoryKey, 1);
    }
    // disks the snapshot does not cover (independent disks): copied as they are, live
    for (const LxDisk& d : vm.lx.disks) {
        bool covered = false;
        for (const LxDisk& sd : snap->disks) if (sd.key == d.key) covered = true;
        if (covered) continue;
        for (const Vec<i64>& link : d.chain) for (i64 k : link) add(k, 2);
        if (!warning.empty()) warning += "; ";
        warning += Str::fmt("a disk (key %lld) is not covered by the snapshot (independent disk?): it is copied live", (long long)d.key);
    }

    // raw device mappings: the "-rdmp.vmdk" / "-rdm.vmdk" file stands for a LUN (the host cannot stream it over HTTP) and the small descriptor beside it is
    // useless without the LUN: neither is copied, one warning names the disk
    Vec<Str> rdmSkip;
    for (size_t i = 0; i < keys.size(); ++i) {
        const LxFile* lf = vm.lx.file(keys[i]);
        if (!lf) continue;
        static const char* const kRdmSuf[] = { "-rdmp.vmdk", "-rdm.vmdk" };
        for (const char* suf : kRdmSuf) {
            size_t sl = strlen(suf);
            if (lf->type == "diskExtent" && lf->name.size() > sl && lf->name.ends_with(suf)) {
                Str stem = lf->name.substr(0, lf->name.size() - sl);
                rdmSkip.push_back(lf->name); rdmSkip.push_back(stem + ".vmdk");
                if (!warning.empty()) warning += "; ";
                warning += "a raw device mapping (RDM) disk is not copied (the LUN cannot be read through the datastore): " + lf->name;
            }
        }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        const LxFile* lf = vm.lx.file(keys[i]);
        if (!lf) {                                                           // a key without a file entry
            if (groups[i] == 2) return fail(Str::fmt("the file layout of the VM lists a disk file (key %lld) that it has no entry for: the disk cannot be copied completely", (long long)keys[i]), true);
            continue;
        }
        { bool skipIt = false; for (const Str& r : rdmSkip) if (r == lf->name) skipIt = true; if (skipIt) continue; }
        FileItem f; f.name = lf->name; f.type = lf->type; f.size = lf->size; f.group = groups[i];
        if (!EsxiParseDsPath(f.name, f.ds, f.path) || f.path.empty()) { if (!warning.empty()) warning += "; "; warning += "skipped (cannot read the datastore path): " + f.name; continue; }
        if (f.ds == vmDs && (vmDir.empty() ? f.path.find('/') < 0 : f.path.starts_with((vmDir + "/").c_str()))) f.arc = vmDir.empty() ? f.path : f.path.substr(vmDir.size() + 1);
        else f.arc = "_other-datastores/" + f.ds + "/" + f.path;
        bool bad = f.arc.empty();
        for (size_t k = 0; k + 1 < f.arc.size(); ++k) if (f.arc[k] == '/' && f.arc[k + 1] == '/') bad = true;
        if (!bad && (f.arc.starts_with("../") || f.arc == ".." || f.arc.find("/../") >= 0 || f.arc.ends_with("/..") || f.arc[0] == '/')) bad = true;
        if (bad) { if (!warning.empty()) warning += "; "; warning += "skipped (unsafe path): " + f.name; continue; }
        bool dup = false; for (const FileItem& o : out) if (o.origArc == f.arc) dup = true;
        if (dup) continue;
        f.origArc = f.arc; f.arc = SafeArcPath(f.arc);
        for (int n = 2;; ++n) { bool clash = false; for (const FileItem& o : out) if (o.arc == f.arc) clash = true; if (!clash) break; f.arc = SafeArcPath(f.origArc) + Str::fmt("~%d", n); }
        out.push_back(f);
    }
    // deterministic order: configuration, snapshot data, disks; by name inside a group
    for (size_t i = 1; i < out.size(); ++i) {
        FileItem x = out[i]; size_t j = i;
        while (j > 0 && (out[j - 1].group > x.group || (out[j - 1].group == x.group && x.arc < out[j - 1].arc))) { out[j] = out[j - 1]; --j; }
        out[j] = x;
    }
    // the files that describe the new snapshot itself (a restore does not upload them: the VM comes back without it)
    i64 snapKeys[2] = { snap->dataKey, snap->memoryKey };
    for (i64 k : snapKeys) {
        const LxFile* lf = k >= 0 ? vm.lx.file(k) : nullptr;
        if (lf) for (const FileItem& f : out) if (f.name == lf->name) snapFiles.push(f.arc);
    }
    // the disks as the VM had them when the snapshot was taken (for a later restore)
    for (const DiskDev& dd : vm.disks) {
        Json o = Json::object(); o["key"] = (i64)dd.key; o["label"] = dd.label; o["capacity_bytes"] = (u64)dd.capacity; o["thin"] = dd.thin; o["mode"] = dd.mode;
        o["running_point"] = dd.backing;
        bool covered = false;
        for (const LxDisk& sd : snap->disks) if (sd.key == dd.key) covered = true;
        if (!covered) {                                                      // an independent disk (copied live): its own descriptor stands for it, so that a restore points the .vmx at the copy
            for (const LxDisk& ld : vm.lx.disks) {
                if (ld.key != dd.key || ld.chain.empty()) continue;
                for (i64 k : ld.chain[0]) {
                    const LxFile* lf = vm.lx.file(k);
                    if (lf && lf->type == "diskDescriptor") for (const FileItem& f : out) if (f.name == lf->name) o["snapshot_descriptor"] = f.arc;
                }
            }
        }
        for (const LxDisk& sd : snap->disks) {
            if (sd.key != dd.key || sd.chain.empty()) continue;
            for (i64 k : sd.chain[0]) {
                const LxFile* lf = vm.lx.file(k);
                if (lf && lf->type == "diskDescriptor") for (const FileItem& f : out) if (f.name == lf->name) o["snapshot_descriptor"] = f.arc;
            }
        }
        disksMeta.push(o);
    }
    return true;
}

bool Impl::begin(const Str& vm, bool allowLive, const Str& resumeToken, Str& fingerprint, Str& token, Str& warning)
{
    (void)allowLive;                                                       // a snapshot makes a running VM consistent: there is nothing to refuse
    m_begun = false; m_reused = false; m_list.clear(); m_snapRef.clear(); m_snapName.clear(); m_meta.clear(); token.clear(); warning.clear();
    { unsigned char tb[4]; RandomBytes(tb, 4); LiveRegister(m_runToken, false); m_runToken = HexEncode(tb, 4); LiveRegister(m_runToken, true); }
    m_leaked.clear();
    if (vm.empty()) return fail("no VM name");
    if (!ensureSession()) return false;
    if (m_vcenter && !m_apiType.empty() && m_apiType != "VirtualCenter") warning = "the server is not a vCenter (it is a standalone host): the server type is set wrongly, the copy works the same";
    if (!m_vcenter && m_apiType == "VirtualCenter") warning = "the server is a vCenter: the server type is set wrongly, the copy works the same";

    VmData v;
    if (!findVm(vm, v)) return false;
    m_vmRef = v.id; m_vmName = v.name; m_dc = v.dc;
    if (v.isTemplate) return fail("\"" + vm + "\" is a template, not a virtual machine");
    if (v.encrypted) {
        // the datastore files of an encrypted VM are ciphertext: a copy can only be used on a host that gets the same key from the same key provider
        if (!m_cfg.get("allow_encrypted").boolean()) return fail("the VM \"" + vm + "\" is encrypted (vSphere VM Encryption): its files cannot be backed up in a usable form; remove the encryption policy first or set \"allow_encrypted\" in the server settings to copy the ciphertext anyway");
        if (!warning.empty()) warning += "; ";
        warning += "the VM is encrypted (vSphere VM Encryption): the copy is ciphertext and is usable only with the same key provider";
    }
    if (!v.haveConfig) return fail("the VM \"" + vm + "\" is not accessible (state: " + (v.conn.empty() ? Str("unknown") : v.conn) + ")");
    if (!v.conn.empty() && v.conn != "connected") return fail("the VM \"" + vm + "\" is not accessible (state: " + v.conn + ")");
    bool poweredOn = v.power == "poweredOn";

    // 1. the snapshot: re-attach to the one of the interrupted run, or start over (leftovers of earlier runs are removed)
    Str keepRef, keepName; bool reuse = false;
    if (!resumeToken.empty()) {
        int bar = resumeToken.find('|');
        if (bar > 0) {
            Str ref = resumeToken.substr(0, (size_t)bar), name = resumeToken.substr((size_t)bar + 1);
            for (const SnapInfo& s : v.snaps) if (s.ref == ref && s.name == name && IsOurSnapshotName(name)) { reuse = true; keepRef = ref; keepName = name; }
        }
    }
    {
        int n = 0;
        if (!removeLeftovers(v, keepRef, n)) { Str e = err; bool t = transient; return fail("cannot remove an old VM2Backup snapshot: " + e, t); }
        if (n) { if (!warning.empty()) warning += "; "; warning += Str::fmt("removed %d old VM2Backup snapshot(s) left by an earlier run", n); }
    }

    if (m_skippedLive) { if (!warning.empty()) warning += "; "; warning += Str::fmt("%d VM2Backup snapshot(s) of another run that is working on this VM were left alone", m_skippedLive); }
    if (reuse) {
        m_snapRef = keepRef; m_snapName = keepName; m_reused = true; markSnapshot(true);
    } else {
        unsigned char rb[5]; RandomBytes(rb, 5);
        Str newName = "VM2Backup-" + HexEncode(rb, 5), newRef;
        bool q = m_quiesce && poweredOn;
        for (int attempt = 0; attempt < 2; ++attempt) {
            Str in = Ref("_this", "VirtualMachine", m_vmRef) + Tag("name", newName) + Tag("description", Str(kSnapDesc) + LiveMark(m_runToken)) +
                     Tag("memory", (m_memory && poweredOn) ? "true" : "false") + Tag("quiesce", q ? "true" : "false");
            TaskResult tr;
            if (!runTask("CreateSnapshot_Task", in, m_taskTimeout, tr)) {
                Str e = err; bool t = transient;
                if (e == kAbortedMessage || tr.timedOut) cleanupByName(newName, Str());
                return fail(e, t);
            }
            if (tr.ok) { newRef = tr.result; break; }
            if (IsLicenseProblem(tr.faultType, tr.faultMsg)) return fail(LicenseMessage("creating the snapshot"));
            if (q && IsQuiesceProblem(tr.faultType, tr.faultMsg)) {
                q = false;
                if (!warning.empty()) warning += "; ";
                warning += "the quiesced snapshot failed (" + tr.faultMsg + "): taken without quiescing (VMware Tools missing or not running), the copy is crash-consistent";
                cleanupByName(newName, Str());                       // a half-made one must not stay
                continue;
            }
            return fail("creating the snapshot failed: " + tr.faultMsg);
        }
        if (newRef.empty()) return fail("the snapshot task returned no snapshot");
        m_snapRef = newRef; m_snapName = newName; m_hbTick = GetTickCount();
    }

    // From here on a failure removes a snapshot this run made (a re-attached one only when the failure is final).
    auto abandon = [&](bool final_) { if (!m_reused || final_) { Str e = err; bool t = transient; cleanupByName(m_snapName, m_snapRef); err = e; transient = t; } m_snapRef.clear(); m_snapName.clear(); };

    // 2. what the snapshot froze (read after the snapshot: the layout lists its files)
    VmData after; bool found = false;
    for (int i = 0; i < 12 && !found; ++i) {
        if (!readVm(m_vmRef, m_dc, after, i > 0)) { abandon(!transient); return false; }
        for (const LxSnap& s : after.lx.snaps) if (s.key == m_snapRef) found = true;
        if (!found) Sleep(250);
    }
    if (!found) { fail("the server did not report the snapshot's files"); abandon(true); return false; }
    Str w2; Json ignored = Json::array(), disksMeta = Json::array(), snapFiles = Json::array(); bool built = false;
    for (int pass = 0; pass < 5 && !built; ++pass) {                       // a layout that names a disk file it has no entry for yet: the host is still writing it up, ask again
        if (pass) { Sleep(400); if (!readVm(m_vmRef, m_dc, after, true)) { abandon(!transient); return false; } }
        w2.clear(); ignored = Json::array(); disksMeta = Json::array(); snapFiles = Json::array();
        built = buildFiles(after, m_snapRef, m_list, w2, ignored, disksMeta, snapFiles);
        if (!built && !transient) break;
    }
    if (!built) { abandon(!transient); return false; }
    if (!w2.empty()) { if (!warning.empty()) warning += "; "; warning += w2; }
    if (m_list.empty()) { fail("the VM has no files to copy"); abandon(true); return false; }

    // 3. the real sizes and times from the datastore (the layout's numbers are only a hint)
    double now = UnixNow();
    for (FileItem& f : m_list) {
        bool relogged = false;
        f.mtime = (i64)now;
        if (!statFile(f, relogged)) { abandon(!transient); return false; }
    }

    // 4. meta file and fingerprint: fixed now, because the stream must be the same on every pass
    Json meta = Json::object();
    meta["tool"] = "VM2Backup " VM2B_VER_STR; meta["source"] = "esxi"; meta["server_type"] = m_vcenter ? "vcenter" : "esxi"; meta["host"] = m_host;
    meta["api_version"] = m_apiVersion; meta["full_name"] = m_fullName; meta["product"] = m_product; meta["api_type"] = m_apiType;
    meta["vm"] = vm; meta["vm_ref"] = m_vmRef; meta["uuid"] = after.uuid; meta["instance_uuid"] = after.instUuid; meta["datacenter"] = m_dc;
    meta["vmx"] = after.vmPath; meta["power_state"] = after.power; meta["created"] = now;
    { Str dsx, px; EsxiParseDsPath(after.vmPath, dsx, px); int sl = px.rfind('/');
      meta["datastore"] = dsx; meta["vm_dir"] = sl >= 0 ? px.substr(0, (size_t)sl) : Str(); meta["vmx_name"] = sl >= 0 ? px.substr((size_t)sl + 1) : px; }
    meta["snapshot_files"] = snapFiles;
    meta["snapshot"] = m_snapName; meta["snapshot_quiesce"] = m_quiesce && poweredOn; meta["reused_snapshot"] = m_reused;
    Json fl = Json::array();
    for (const FileItem& f : m_list) { Json o = Json::object(); o["name"] = f.name; o["path"] = f.arc; if (f.origArc != f.arc) o["orig_path"] = f.origArc; o["size"] = f.size; o["type"] = f.type; fl.push(o); }
    meta["files"] = fl; meta["disks"] = disksMeta; meta["ignored_files"] = ignored;
    m_meta = meta.dump(true); m_metaTime = (i64)now;
    m_prefix = EsxiArchivePrefix(vm);

    Str fp = "esxi|" + after.uuid + "|" + after.instUuid + "|" + m_snapRef + "|" + m_snapName + "\n";
    for (const FileItem& f : m_list) fp += f.arc + "|" + Str::fmt("%llu", (unsigned long long)f.size) + "\n";
    u8 h[32]; Sha256(fp.data(), fp.size(), h);
    fingerprint = "esxi:" + HexEncode(h, 32);
    token = m_snapRef + "|" + m_snapName;
    if (m_reused) { if (!warning.empty()) warning += "; "; warning += "continued with the snapshot of the interrupted run"; }
    m_begun = true;
    return true;
}

bool Impl::pack(TarWriter& tar, Progress* pr, Str& warning)
{
    if (!m_begun) return fail("esxi source: pack() without begin()");
    if (!tar.add_bytes("vm2backup-meta.json", m_meta.data(), m_meta.size(), m_metaTime)) return fail(tar.error());
    makeClients();
    Bytes buf; buf.resize(1 << 20);
    for (const FileItem& f : m_list) {
        bool relogged = false;                                                  // one transparent login per file
        if (pr && pr->aborted()) return fail(kAbortedMessage);
        Str arc = m_prefix + "/" + f.arc;
        if (pr) pr->onFile(arc);
        if (!tar.begin_file(arc, f.size, f.mtime)) return fail(tar.error());
        u64 skip = tar.skippable();
        if (skip) { if (!tar.skip_data(skip)) return fail(tar.error()); }
        u64 left = f.size - skip;
        u64 delivered = 0; bool shrank = false;
        if (left) {
            HttpResponse r; bool have = false;
            for (int attempt = 0; attempt < 4 && !have; ++attempt) {
                HttpHeaders h;
                if (skip) h.add("Range", Str::fmt("bytes=%llu-", (unsigned long long)skip));
                if (!FolderRequest(*m_files, "GET", EsxiFolderUrl(f.ds, f.path, m_dc), h, r, m_host, m_port)) {
                    if (m_files->aborted) return fail(kAbortedMessage);
                    return fail("reading " + f.name + ": " + m_files->err, m_files->transient);
                }
                if ((r.status == 401 || r.status == 403) && !relogged) {
                    relogged = true; m_files->close(); m_loggedIn = false;
                    if (!ensureSession()) return false;
                    continue;
                }
                if ((r.status == 503 || r.status == 500 || r.status == 502 || r.status == 504) && attempt < 2) { m_files->close(); Sleep(500); continue; }
                have = true;
            }
            if (r.status == 416) { shrank = true; m_files->close(); }
            else if (r.status != 200 && r.status != 206) {
                m_files->close();
                return fail(Str::fmt("reading %s: HTTP %d %s", f.name.c_str(), r.status, OneLine(r.reason).c_str()), false);      // (500, 502, 503 and 504 were tried 3 times above)
            } else {
                if (IsBinaryType(f.type) && r.headers.value("Content-Type").lower().find("text/html") >= 0) {
                    m_files->close();                                           // an error page of a proxy / a login page, not the file
                    return fail("reading " + f.name + ": the server answered with an HTML page instead of the file", true);
                }
                u64 discard = 0;
                if (skip) {
                    if (r.status == 206) {
                        Str cr = r.headers.value("Content-Range");              // "bytes 100-999/1000"
                        unsigned long long a = 0, b = 0, t = 0;
                        if (sscanf(cr.c_str(), "bytes %llu-%llu/%llu", &a, &b, &t) >= 2 && a <= skip) discard = skip - a;
                        else { m_files->close(); return fail("reading " + f.name + ": the server answered a different range"); }
                    } else discard = skip;                                      // no range support: read and drop
                }
                u64 toDrop = discard;
                while (toDrop) {
                    if (pr && pr->aborted()) { m_files->close(); return fail(kAbortedMessage); }
                    i64 k = r.body->read(buf.data(), toDrop > buf.size() ? buf.size() : (size_t)toDrop);
                    if (k < 0) { Str e = "reading " + f.name + ": " + r.body->err; bool t = r.body->transient; m_files->close(); return fail(e, t); }
                    if (k == 0) break;
                    toDrop -= (u64)k;
                }
                if (toDrop) shrank = true;
                while (!shrank && delivered < left) {
                    if (pr && pr->aborted()) { m_files->close(); return fail(kAbortedMessage); }
                    size_t want = (left - delivered) > buf.size() ? buf.size() : (size_t)(left - delivered);
                    i64 k = r.body->read(buf.data(), want);
                    if (k < 0) { Str e = "reading " + f.name + ": " + r.body->err; bool t = r.body->transient; m_files->close(); return fail(e, t); }
                    if (k == 0) { shrank = true; break; }
                    if (!tar.write(buf.data(), (size_t)k)) { m_files->close(); return fail(tar.error()); }
                    delivered += (u64)k;
                    if (pr) pr->onBytes((u64)k);
                    if (GetTickCount() - m_hbTick > 120000) markSnapshot(true);          // the mark of a working run stays fresh
                }
                if (delivered == left && !shrank) {
                    // more data than announced? (the file grew) the entry keeps its size; the connection cannot be reused
                    u8 probe; i64 k = r.body->read(&probe, 1);
                    if (k != 0) { m_files->close(); if (!warning.empty()) warning += "; "; warning += "the file grew while it was copied: " + f.name; }
                }
            }
        }
        if (left && delivered < left && IsFrozenType(f.type))                   // a frozen file cannot change: a short answer means damaged data, never a clean success
            return fail(Str::fmt("reading %s: the server delivered %llu of %llu bytes", f.name.c_str(), (unsigned long long)(skip + delivered), (unsigned long long)f.size), true);
        if (left && delivered < left) {                                         // the file shrank: the entry keeps its announced size
            memset(buf.data(), 0, buf.size());
            while (delivered < left) {
                size_t k = (left - delivered) > buf.size() ? buf.size() : (size_t)(left - delivered);
                if (!tar.write(buf.data(), k)) return fail(tar.error());
                delivered += k;
                if (pr) pr->onBytes(k);
            }
            if (!warning.empty()) warning += "; ";
            warning += "the file is shorter than expected (changed while it was copied): " + f.name;
        }
        if (!tar.end_file()) return fail(tar.error());
    }
    return true;
}

void Impl::end(bool keepForResume)
{
    InterlockedExchange(&m_abort, 0);                                          // the run is over: nothing below is skipped because of an abort
    if (!m_snapRef.empty() && m_soap) markSnapshot(false);                     // done or paused: no longer "live" (a later run may remove it at once)
    if (!keepForResume && !m_snapRef.empty() && m_soap) {
        InterlockedExchange(&m_abort, 0);                                      // a removal is never skipped because of an abort
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (attempt) Sleep(1000);
            if (ensureSession() && removeSnapshot(m_snapRef)) ok = true;
        }
        if (!ok) m_leaked = m_snapName;
    }
    m_snapRef.clear(); m_snapName.clear(); m_begun = false; m_list.clear();
    LiveRegister(m_runToken, false);
    logout();
}


// ---- restore to ESXi ---------------------------------------------------------------------------------------------------
struct InvDs { Str name, type; i64 free, capacity; bool accessible; InvDs() : free(0), capacity(0), accessible(true) {} };
struct InvFolder { Str id, path; };
struct InvPool { Str id, name, path, compute; };
struct InvHost { Str id, name, compute; };
struct Inventory {
    Str dcId, dcName, vmFolder; Vec<Str> dcNames;
    Vec<InvDs> ds; Vec<InvFolder> folders; Vec<InvPool> pools; Vec<InvHost> hosts; Vec<Str> vms; Vec<Str> computes, computeRoot;
    const InvDs* findDs(const Str& n) const { for (const InvDs& d : ds) if (d.name == n) return &d; return nullptr; }
};

static Str SafeFolderName(const Str& s)
{
    Str o;
    for (size_t i = 0; i < s.size(); ++i) { char c = s[i]; o += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20) ? '_' : c; }
    o = TruncateUtf8(o, 200);                                           // a host refuses a folder name over 255 bytes
    o = o.trim();
    while (o.ends_with(".")) o = o.substr(0, o.size() - 1);
    return o.empty() ? Str("restored-vm") : o;
}

static Str B64Str(const Str& s) { return Base64Encode(s.data(), s.size()); }
static Str UnB64(const Str& s) { Bytes b; if (!Base64Decode(s.data(), s.size(), b)) return Str(); return Str((const char*)b.data(), b.size()); }

class RestoreImpl : public RestoreWriter {
public:
    RestoreImpl(const Json& server, const EsxiRestoreOptions& o, EsxiRestoreResult* res) : m_o(o), m_res(res), m_cfgHost(server.get("host").str()), m_inited(false), m_registered(false) {
        Json cfg = server; cfg["expect100"] = true;
        if (!o.datacenter.empty()) cfg["datacenter"] = o.datacenter;
        m_s = new Impl(cfg);
        m_ident = server.get("type").str() + "|" + m_cfgHost + "|" + Str::fmt("%d", (int)server.get("port").integer(443)) + "|" + o.datacenter + "|" + o.vmName + "|" + o.datastore + "|" + o.folder + "|" + o.pool + "|" + o.host;
    }
    ~RestoreImpl() override { delete m_s; }
    const char* kind() const override { return "esxi"; }
    bool canResumeInsideFile() const override { return false; }              // HTTP PUT cannot append: a file always starts again from its first byte
    bool cleansUpOnFailure() const override { return true; }

    Str stateFile(const ReadOptions& ro, const Str& archive, const Str& root) override {
        Str dir = ro.stateDir;
        if (dir.empty()) { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); dir = FromWide(t); }
        Str id = archive + "|" + root + "|" + m_ident; u8 h[32]; Sha256(id.data(), id.size(), h);
        return JoinPath(dir, "vm2b-restore-esxi-" + HexEncode(h, 8) + ".json");
    }
    Json saveState() override {
        Json j = Json::object();
        j["name"] = m_name; j["dir"] = m_dir; j["dc"] = m_dcName; j["main_ds"] = m_mainDs; j["folder_id"] = m_folderId; j["pool_id"] = m_poolId; j["host_id"] = m_hostId;
        j["meta"] = B64Str(m_metaText); j["vmx"] = B64Str(m_vmxText); j["vmsd"] = B64Str(m_vmsdText);
        Json d = Json::array(); for (const Str& x : m_made) d.push(x); j["made"] = d;
        return j;
    }
    bool loadState(const Json& j) override {
        if (!j.is_object() || j.get("name").str().empty() || j.get("meta").str().empty()) return false;
        m_name = j.get("name").str(); m_dir = j.get("dir").str(); m_dcName = j.get("dc").str(); m_mainDs = j.get("main_ds").str();
        m_folderId = j.get("folder_id").str(); m_poolId = j.get("pool_id").str(); m_hostId = j.get("host_id").str();
        m_metaText = UnB64(j.get("meta").str()); m_vmxText = UnB64(j.get("vmx").str()); m_vmsdText = UnB64(j.get("vmsd").str());
        m_made.clear(); const Json& d = j.get("made"); for (size_t i = 0; i < d.size(); ++i) m_made.push_back(d.at(i).str());
        Str pe; if (!Json::parse(m_metaText, m_meta, pe)) return false;
        buildMap();
        m_inited = true;
        return true;
    }
    bool prepare(const Manifest& m, bool resuming) override {
        m_manifest = m;
        if (!m_s->ensureSession()) return failFrom();
        if (resuming && m_inited) {
            m_s->m_dcCfg = m_dcName;
            Inventory inv; if (!loadInventory(inv, false)) return failFrom();
            m_dcId = inv.dcId;
            if (!dirExists(m_mainDs, m_dir)) { m_inited = false; return fail("the folder of the unfinished restore is gone from the datastore: remove the restore state and start again"); }
        }
        return true;
    }
    // the name a file had on the host when the archive path was made safe (meta "orig_path"); the same text when nothing was changed
    Str origOf(const Str& arc) const { const Json& fl = m_meta.get("files"); for (size_t i = 0; i < fl.size(); ++i) if (fl.at(i).get("path").str() == arc) { Str o = fl.at(i).get("orig_path").str(); return o.empty() ? arc : o; } return arc; }
    bool makeDir(const Str&) override { return true; }                          // folders are made when a file needs them
    bool reconnect() override {
        m_s->logout();                                                          // a new session: the old one may have expired (the logout then fails, harmlessly)
        if (!m_s->ensureSession()) return failFrom();
        return true;
    }

    bool writeFile(const RestoreFile& f, Source& body, RestoreTick* tick) override;
    bool finish() override;
    void cleanup() override;
    bool targets(Json& out, Str& err);                                       // for the dialog: what can be chosen

private:
    bool failFrom() { return fail(m_s->err, m_s->transient); }
    bool loadInventory(Inventory& inv, bool full);
    bool dirExists(const Str& ds, const Str& path) {
        HttpHeaders h; HttpResponse r;
        if (!FolderRequest(*m_s->m_files, "GET", EsxiFolderUrl(ds, path, m_dcName), h, r, m_s->m_host, m_s->m_port)) return false;
        bool ok = r.status == 200;
        u8 junk[4096]; if (r.body) while (r.body->read(junk, sizeof(junk)) > 0) {}
        return ok;
    }
    bool initFromMeta();
    bool ensureDir(const Str& ds, const Str& dirPath, bool mustBeNew = false);                          // MakeDirectory with parents, remembered
    bool mapArc(const Str& arc, Str& ds, Str& path);
    void mapOne(const Str& arc, bool keepFolder, Str& ds, Str& path);
    void buildMap();
    Vec<Str> m_mapArc, m_mapDs, m_mapPath;
    bool putStream(const Str& ds, const Str& path, u64 size, Source& body, bool bodyIsArchive);
    bool putMem(const Str& ds, const Str& path, const Str& data) { MemSource ms(data.data(), data.size()); return putStream(ds, path, data.size(), ms, false); }
    static bool Slurp(Source& body, size_t limit, Str& out) {
        out.clear(); u8 buf[65536]; i64 k;
        while ((k = body.read(buf, sizeof(buf))) > 0) { if (out.size() + (size_t)k > limit) return false; out.append((const char*)buf, (size_t)k); }
        return k == 0;
    }

    EsxiRestoreOptions m_o; EsxiRestoreResult* m_res; Impl* m_s;
    Str m_ident, m_cfgHost, m_name, m_dir, m_dcName, m_dcId, m_mainDs, m_folderId, m_poolId, m_hostId, m_metaText, m_vmxText, m_vmsdText;
    Json m_meta; Manifest m_manifest; bool m_inited, m_registered; Vec<Str> m_made;       // "ds|dir": the folders this restore created
};

// the datacenter's objects; `full` also gives the lists a dialog needs (VM names, all folders / pools / hosts)
bool RestoreImpl::loadInventory(Inventory& inv, bool full)
{
    Impl& s = *m_s;
    Vec<Impl::Dc> dcs;
    {
        Str want = s.m_dcCfg; s.m_dcCfg.clear();                            // all of them: the dialog lists them, the chosen one is used
        bool ok = s.datacenters(dcs);
        s.m_dcCfg = want;
        if (!ok) { return false; }
        for (const Impl::Dc& d : dcs) inv.dcNames.push_back(d.name);
        int pick = 0;
        if (!want.empty()) { pick = -1; for (size_t i = 0; i < dcs.size(); ++i) if (dcs[i].name == want) pick = (int)i; if (pick < 0) { s.fail("datacenter not found: " + want); return false; } }
        if (pick > 0) { Impl::Dc d = dcs[(size_t)pick]; dcs.clear(); dcs.push_back(d); } else if (dcs.size() > 1) dcs.resize(1);
    }
    inv.dcId = dcs[0].id; inv.dcName = dcs[0].name;
    Vec<ObjData> objs;
    static const char* dcp[] = { "vmFolder" };
    if (!s.retrieve(Impl::PropSet("Datacenter", dcp, 1), Impl::ObjSelect("Datacenter", inv.dcId), objs)) return false;
    if (!objs.empty()) inv.vmFolder = objs[0].text("vmFolder");
    Str view;
    {   // datastores
        static const char* p[] = { "name", "summary.capacity", "summary.freeSpace", "summary.type", "summary.accessible" };
        if (!s.createView("Datacenter", inv.dcId, "Datastore", view)) return false;
        bool ok = s.retrieve(Impl::PropSet("Datastore", p, 5), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        for (const ObjData& o : objs) { InvDs d; d.name = o.text("name"); d.capacity = I64(o.text("summary.capacity")); d.free = I64(o.text("summary.freeSpace")); d.type = o.text("summary.type"); d.accessible = o.text("summary.accessible", "true") != "false"; inv.ds.push_back(d); }
    }
    {   // VM folders
        static const char* p[] = { "name", "parent", "childType" };
        if (!s.createView("Datacenter", inv.dcId, "Folder", view)) return false;
        bool ok = s.retrieve(Impl::PropSet("Folder", p, 3), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        Vec<Str> ids, names, parents; Vec<bool> vmf;
        for (const ObjData& o : objs) {
            bool isVm = false; const XmlNode* ct = o.get("childType"); if (ct) for (const XmlNode& k : ct->kids) if (k.text.trim() == "VirtualMachine") isVm = true;
            ids.push_back(o.id); names.push_back(o.text("name")); parents.push_back(o.text("parent")); vmf.push_back(isVm);
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            if (!vmf[i]) continue;
            Str path; Str cur = ids[i]; int guard = 0; bool root = false;
            while (guard++ < 40) {
                if (cur == inv.vmFolder) { root = true; break; }
                int k = -1; for (size_t j = 0; j < ids.size(); ++j) if (ids[j] == cur) k = (int)j;
                if (k < 0) break;
                path = path.empty() ? names[(size_t)k] : names[(size_t)k] + "/" + path;
                cur = parents[(size_t)k];
            }
            if (!root) continue;
            InvFolder f; f.id = ids[i]; f.path = path; inv.folders.push_back(f);
        }
        if (!inv.vmFolder.empty()) { InvFolder f; f.id = inv.vmFolder; inv.folders.push_back(f); }
    }
    {   // compute resources, pools, hosts
        static const char* pc[] = { "name", "resourcePool", "host" };
        if (!s.createView("Datacenter", inv.dcId, "ComputeResource", view)) return false;
        bool ok = s.retrieve(Impl::PropSet("ComputeResource", pc, 3), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        Vec<Str> cids, cnames;
        for (const ObjData& o : objs) { cids.push_back(o.id); cnames.push_back(o.text("name")); inv.computes.push_back(o.text("name")); inv.computeRoot.push_back(o.text("resourcePool")); }
        static const char* pp[] = { "name", "parent" };
        if (!s.createView("Datacenter", inv.dcId, "ResourcePool", view)) return false;
        ok = s.retrieve(Impl::PropSet("ResourcePool", pp, 2), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        Vec<Str> pids, pnames, pparents;
        for (const ObjData& o : objs) { pids.push_back(o.id); pnames.push_back(o.text("name")); const XmlNode* par = o.get("parent"); pparents.push_back(par ? par->text.trim() : Str()); }
        for (size_t i = 0; i < pids.size(); ++i) {
            Str path = pnames[i]; Str cur = pparents[i]; int guard = 0; Str comp;
            while (guard++ < 40 && !cur.empty()) {
                int k = -1; for (size_t j = 0; j < pids.size(); ++j) if (pids[j] == cur) k = (int)j;
                if (k >= 0) { path = pnames[(size_t)k] + "/" + path; cur = pparents[(size_t)k]; continue; }
                for (size_t j = 0; j < cids.size(); ++j) if (cids[j] == cur) { comp = cnames[j]; path = cnames[j] + "/" + path; }
                break;
            }
            InvPool p; p.id = pids[i]; p.name = pnames[i]; p.path = path; p.compute = comp; inv.pools.push_back(p);
        }
        static const char* ph[] = { "name", "parent" };
        if (!s.createView("Datacenter", inv.dcId, "HostSystem", view)) return false;
        ok = s.retrieve(Impl::PropSet("HostSystem", ph, 2), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        for (const ObjData& o : objs) { InvHost h; h.id = o.id; h.name = o.text("name"); const XmlNode* par = o.get("parent"); h.compute = par ? par->text.trim() : Str(); inv.hosts.push_back(h); }
    }
    {   // existing VM names (a restore never overwrites one)
        static const char* p[] = { "name" };
        if (!s.createView("Datacenter", inv.dcId, "VirtualMachine", view)) return false;
        bool ok = s.retrieve(Impl::PropSet("VirtualMachine", p, 1), Impl::ViewSelect(view), objs); s.destroyView(view);
        if (!ok) return false;
        for (const ObjData& o : objs) inv.vms.push_back(o.text("name"));
    }
    (void)full;
    m_dcName = inv.dcName; m_dcId = inv.dcId;
    return true;
}

bool RestoreImpl::targets(Json& out, Str& err)
{
    if (!m_s->ensureSession()) { err = m_s->err; return false; }
    Inventory inv;
    bool ok = loadInventory(inv, true);
    if (!ok) { err = m_s->err; m_s->logout(); return false; }
    out = Json::object();
    out["kind"] = m_s->m_vcenter ? "vcenter" : "esxi"; out["api_version"] = m_s->m_apiVersion; out["datacenter"] = inv.dcName; out["vm_folder"] = inv.vmFolder;
    Json dcs = Json::array(); for (const Str& n : inv.dcNames) dcs.push(n); out["datacenters"] = dcs;
    Json ds = Json::array();
    for (const InvDs& d : inv.ds) { Json o = Json::object(); o["name"] = d.name; o["free"] = (i64)d.free; o["capacity"] = (i64)d.capacity; o["type"] = d.type; o["accessible"] = d.accessible; ds.push(o); }
    out["datastores"] = ds;
    Json fo = Json::array(); for (const InvFolder& f : inv.folders) { Json o = Json::object(); o["id"] = f.id; o["path"] = f.path; fo.push(o); } out["folders"] = fo;
    Json po = Json::array(); for (const InvPool& p : inv.pools) { Json o = Json::object(); o["id"] = p.id; o["name"] = p.name; o["path"] = p.path; po.push(o); } out["pools"] = po;
    Json ho = Json::array(); for (const InvHost& h : inv.hosts) { Json o = Json::object(); o["id"] = h.id; o["name"] = h.name; ho.push(o); } out["hosts"] = ho;
    Json vm = Json::array(); for (const Str& v : inv.vms) vm.push(v); out["vms"] = vm;
    m_s->logout();
    return true;
}

bool RestoreImpl::ensureDir(const Str& ds, const Str& dirPath, bool mustBeNew)
{
    if (dirPath.empty()) return true;
    Str key = ds + "|" + dirPath;
    for (const Str& k : m_made) if (k == key) return true;
    const XmlNode* r;
    Str in = Ref("_this", "FileManager", m_s->m_fileMgr.empty() ? Str("ha-nfc-file-manager") : m_s->m_fileMgr) + Tag("name", "[" + ds + "] " + dirPath) + Ref("datacenter", "Datacenter", m_dcId) + "<createParentDirectories>true</createParentDirectories>";
    if (!m_s->call("MakeDirectory", in, r)) {
        if (m_s->m_hasFault && (m_s->m_faultType == "FileAlreadyExists" || m_s->m_faultMsg.find("already exists") >= 0)) {
            // the VM's own folder was free a moment ago: someone else made it meanwhile, it is not ours (and our cleanup must never delete it)
            if (mustBeNew) return fail("a datastore folder named \"" + dirPath + "\" appeared on the datastore meanwhile (another restore or another client): choose another name (nothing is overwritten)");
            m_made.push_back(key); return true;
        }
        if (m_s->m_hasFault) return fail("cannot create the folder [" + ds + "] " + dirPath + " on the datastore: " + m_s->err);
        return failFrom();
    }
    m_made.push_back(key);
    int sl = dirPath.find('/');
    if (sl > 0) { Str top = ds + "|" + dirPath.substr(0, (size_t)sl); bool have = false; for (const Str& k : m_made) if (k == top) have = true; if (!have) m_made.push_back(top); }
    return true;
}

// archive path -> (datastore, path below the datastore root). `keepFolder`: the clash-free form, with the datastore and the original folder kept in the name
void RestoreImpl::mapOne(const Str& arc0, bool keepFolder, Str& ds, Str& path)
{
    Str arc = origOf(arc0);
    Str rest = arc, origDs = m_mainDs, pre;
    if (arc.starts_with("_other-datastores/")) {
        Str r = arc.substr(18); int sl = r.find('/');
        if (sl < 0) { ds = m_mainDs; path = m_dir + "/" + r; return; }
        origDs = r.substr(0, (size_t)sl); rest = r.substr((size_t)sl + 1);
        Str od = m_meta.get("vm_dir").str();
        if (keepFolder) pre = "_" + origDs + "/";
        else if (!od.empty() && rest.starts_with((od + "/").c_str())) rest = rest.substr(od.size() + 1);
        else { int s2 = rest.find('/'); if (s2 > 0) rest = rest.substr((size_t)s2 + 1); }   // another folder name: only the file's place below it is kept
    }
    ds = m_o.datastore.empty() ? origDs : m_o.datastore;
    path = m_dir + "/" + pre + rest;
}

// Every file gets its place once, up front: folders other than the VM's own are folded into the VM's folder (a renamed VM keeps its disks in the old folder),
// but two files that would land on the same name (two disks called the same in different folders) must never overwrite one another.
void RestoreImpl::buildMap()
{
    m_mapArc.clear(); m_mapDs.clear(); m_mapPath.clear();
    const Json& fl = m_meta.get("files");
    Vec<Str> used;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < fl.size(); ++i) {
            Str a = fl.at(i).get("path").str();
            bool other = a.starts_with("_other-datastores/");
            if ((pass == 0) == other) continue;
            Str ds, path; mapOne(a, false, ds, path);
            Str key = (ds + "|" + path).lower(); bool clash = false;
            for (const Str& u : used) if (u == key) { clash = true; break; }
            if (clash) { mapOne(a, true, ds, path); key = (ds + "|" + path).lower(); }
            used.push_back(key); m_mapArc.push_back(a); m_mapDs.push_back(ds); m_mapPath.push_back(path);
        }
    }
}

bool RestoreImpl::mapArc(const Str& arc0, Str& ds, Str& path)
{
    for (size_t i = 0; i < m_mapArc.size(); ++i) if (m_mapArc[i] == arc0) { ds = m_mapDs[i]; path = m_mapPath[i]; return true; }
    mapOne(arc0, false, ds, path);
    return true;
}

bool RestoreImpl::initFromMeta()
{
    Str pe;
    if (!Json::parse(m_metaText, m_meta, pe) || m_meta.get("source").str() != "esxi") return fail("this backup was not made from an ESXi / vCenter VM (no vm2backup-meta.json of the esxi source): it cannot be restored to a host");
    if (m_meta.get("vmx_name").str().empty()) return fail("the backup's meta file is too old (no vmx_name): it cannot be restored to a host");
    if (!m_name.empty() && !m_made.empty() && !m_dir.empty()) { m_inited = true; return true; }      // a retry after a failure in the middle of this step: the folder is ours already
    m_s->m_dcCfg = m_o.datacenter.empty() ? m_s->m_dcCfg : m_o.datacenter;
    Inventory inv;
    if (!loadInventory(inv, true)) return failFrom();
    m_mainDs = m_o.datastore.empty() ? m_meta.get("datastore").str() : m_o.datastore;
    if (!inv.findDs(m_mainDs)) return fail("the datastore \"" + m_mainDs + "\" does not exist on this server: choose another datastore for the restore");
    if (!inv.findDs(m_mainDs)->accessible) return fail("the datastore \"" + m_mainDs + "\" is not accessible");
    // the name
    Str orig = m_meta.get("vm").str(); if (orig.empty()) orig = m_manifest.vm;
    // the datastores that get a folder of this name besides the main one (disks that lived on another datastore): a folder already there is somebody's, never merged into
    Vec<Str> otherDs;
    if (m_o.datastore.empty()) {
        const Json& fl = m_meta.get("files");
        for (size_t i = 0; i < fl.size(); ++i) {
            Str pth = origOf(fl.at(i).get("path").str());
            if (!pth.starts_with("_other-datastores/")) continue;
            Str r = pth.substr(18); int sl = r.find('/'); if (sl <= 0) continue;
            Str d = r.substr(0, (size_t)sl); bool have = d == m_mainDs;
            for (const Str& o : otherDs) if (o == d) have = true;
            if (!have && inv.findDs(d)) otherDs.push_back(d);
        }
    }
    auto taken = [&](const Str& n) -> bool {
        for (const Str& v : inv.vms) if (v == n) return true;
        if (dirExists(m_mainDs, SafeFolderName(n))) return true;
        for (const Str& d : otherDs) if (dirExists(d, SafeFolderName(n))) return true;
        return false;
    };
    Str name = m_o.vmName.empty() ? orig : m_o.vmName;
    if (taken(name)) {
        if (!m_o.vmName.empty()) return fail("a VM or a datastore folder named \"" + name + "\" already exists on the server: choose another name (nothing is overwritten)");
        name = orig + "-restored";
        if (taken(name)) return fail("a VM or a datastore folder named \"" + orig + "\" and one named \"" + name + "\" already exist on the server: choose another name (nothing is overwritten)");
    }
    m_name = name; m_dir = SafeFolderName(name);
    // folder, pool, host
    m_folderId = inv.vmFolder;
    if (!m_o.folder.empty()) {
        Str want = m_o.folder; while (want.starts_with("/")) want = want.substr(1);
        bool ok = false;
        for (const InvFolder& f : inv.folders) if (f.id == m_o.folder || (!f.path.empty() && f.path == want)) { m_folderId = f.id; ok = true; }
        if (!ok) return fail("the VM folder \"" + m_o.folder + "\" does not exist on the server");
    }
    if (!m_o.host.empty()) {
        bool ok = false;
        for (const InvHost& h : inv.hosts) if (h.id == m_o.host || h.name == m_o.host) { m_hostId = h.id; ok = true; }
        if (!ok) return fail("the host \"" + m_o.host + "\" does not exist on the server");
    }
    if (!m_o.pool.empty()) {
        int hits = 0;
        for (const InvPool& p : inv.pools) if (p.id == m_o.pool || p.path == m_o.pool) { m_poolId = p.id; hits = 1; break; }
        if (!hits) for (const InvPool& p : inv.pools) if (p.name == m_o.pool) { m_poolId = p.id; ++hits; }
        if (hits == 0) return fail("the resource pool \"" + m_o.pool + "\" does not exist on the server");
        if (hits > 1) return fail("several resource pools are named \"" + m_o.pool + "\": give the full path or the id");
    } else {
        // the root pool of the compute resource of the chosen host, else of the first one
        Str hostCompute; for (const InvHost& h : inv.hosts) if (h.id == m_hostId) hostCompute = h.compute;
        if (!inv.computeRoot.empty()) m_poolId = inv.computeRoot[0];
        (void)hostCompute;
    }
    buildMap();
    if (!ensureDir(m_mainDs, m_dir, true)) return false;
    m_inited = true;
    return true;
}

bool RestoreImpl::putStream(const Str& ds, const Str& path, u64 size, Source& body, bool bodyIsArchive)
{
    int sl = path.rfind('/');
    if (sl > 0 && !ensureDir(ds, path.substr(0, (size_t)sl))) return false;
    struct Probe : Source {
        Source& in; bool failed; RateLimiter* rl; HttpClient* http; Probe(Source& s, RateLimiter* l, HttpClient* h) : in(s), failed(false), rl(l), http(h) {}
        static bool Aborted(void* c) { return ((HttpClient*)c)->aborted; }
        i64 read(void* p, size_t n) override {
            if (n > (256u << 10)) n = 256u << 10;                             // small pieces keep the limit smooth
            i64 k = in.read(p, n); if (k < 0) { failed = true; return failFrom(&in); }
            if (k > 0 && rl && !rl->acquire((size_t)k, Aborted, http)) return fail(kAbortedMessage);
            return k;
        }
    } probe(body, m_o.limiter, m_s->m_files);
    HttpHeaders h; h.add("Content-Type", "application/octet-stream");
    HttpResponse r;
    Str shown = "[" + ds + "] " + path;
    if (!m_s->m_files->requestStream("PUT", EsxiFolderUrl(ds, path, m_dcName), h, &probe, (i64)size, r)) {
        if (probe.failed) return fail(body.err, body.transient);
        if (m_s->m_files->aborted) return fail(kAbortedMessage);
        return fail("uploading " + shown + ": " + m_s->m_files->err, m_s->m_files->transient);
    }
    int st = r.status;
    u8 junk[2048]; if (r.body) while (r.body->read(junk, sizeof(junk)) > 0) {}
    if (st == 200 || st == 201 || st == 204) {
        // the host's own idea of the file's size must be what was sent
        HttpHeaders h2; HttpResponse r2;
        if (FolderRequest(*m_s->m_files, "HEAD", EsxiFolderUrl(ds, path, m_dcName), h2, r2, m_s->m_host, m_s->m_port)) {
            if (r2.status == 404) return fail("uploading " + shown + ": the host accepted the upload but the file is not on the datastore", true);
            if (r2.status == 200 && r2.contentLength >= 0 && (u64)r2.contentLength != size)
                return fail(Str::fmt("uploading %s: the host holds %lld bytes, %llu were sent", shown.c_str(), (long long)r2.contentLength, (unsigned long long)size), true);
        }
        return true;
    }
    if (st == 401) { m_s->m_loggedIn = false; return fail("uploading " + shown + ": the session expired", true); }
    if (st == 507) return fail("uploading " + shown + ": the datastore \"" + ds + "\" has no room for it (HTTP 507)");
    if (st == 403) return fail("uploading " + shown + " was refused (HTTP 403): the host or its license does not allow writing to the datastore");
    if (st == 503) return fail("uploading " + shown + ": the host is busy (HTTP 503)", true);
    (void)bodyIsArchive;
    return fail(Str::fmt("uploading %s: HTTP %d %s", shown.c_str(), st, OneLine(r.reason).c_str()), st >= 500 && st != 501);
}

bool RestoreImpl::writeFile(const RestoreFile& f, Source& body, RestoreTick* tick)
{
    (void)tick;
    if (!m_s->ensureSession()) return failFrom();
    if (f.rel == "vm2backup-meta.json") {
        if (m_inited) { u8 junk[4096]; while (body.read(junk, sizeof(junk)) > 0) {} return body.err.empty() ? true : fail(body.err, body.transient); }     // known from the state
        if (!Slurp(body, 8u << 20, m_metaText)) return body.err.empty() ? fail("the meta file of the backup is too big") : fail(body.err, body.transient);
        return initFromMeta();
    }
    if (!m_inited) return fail("the backup has no vm2backup-meta.json first: it was not made by the ESXi source");
    int sl = f.rel.find('/');
    if (sl <= 0) { u8 junk[4096]; while (body.read(junk, sizeof(junk)) > 0) {} return true; }       // not a VM file
    Str arc = f.rel.substr((size_t)sl + 1);
    const Json& sf = m_meta.get("snapshot_files");
    for (size_t i = 0; i < sf.size(); ++i) if (sf.at(i).str() == arc) { u8 junk[65536]; while (body.read(junk, sizeof(junk)) > 0) {} return body.err.empty() ? true : fail(body.err, body.transient); }   // the VM2Backup snapshot's own files
    if (origOf(arc) == m_meta.get("vmx_name").str()) { if (!Slurp(body, 4u << 20, m_vmxText)) return body.err.empty() ? fail(".vmx too big") : fail(body.err, body.transient); return true; }
    if (origOf(arc).ends_with(".vmsd") && arc.find('/') < 0) { if (!Slurp(body, 4u << 20, m_vmsdText)) return body.err.empty() ? fail(".vmsd too big") : fail(body.err, body.transient); return true; }
    Str ds, path;
    if (!mapArc(arc, ds, path)) return fail("cannot place the file " + arc);
    return putStream(ds, path, f.size, body, true);
}

bool RestoreImpl::finish()
{
    if (!m_s->ensureSession()) return failFrom();
    if (!m_inited) return fail("nothing was restored");
    const Str vmxName = m_meta.get("vmx_name").str();
    // 1. the .vmx: the disks point at the base disks of the snapshot, no snapshot / change tracking lines, the new name
    Vec<Str> from, to;
    const Json& disks = m_meta.get("disks");
    for (size_t i = 0; i < disks.size(); ++i) {
        const Json& d = disks.at(i);
        Str rp = d.get("running_point").str(), sd = d.get("snapshot_descriptor").str();
        if (rp.empty() || sd.empty()) continue;
        Str ds0, p0; Str base = rp; bool outside = false;
        if (EsxiParseDsPath(rp, ds0, p0)) {
            base = p0; int sl0 = p0.rfind('/'); Str dir0 = sl0 >= 0 ? p0.substr(0, (size_t)sl0) : Str();
            outside = ds0 != m_meta.get("datastore").str() || dir0 != m_meta.get("vm_dir").str();
        }
        int k = base.rfind('/'); if (k >= 0) base = base.substr((size_t)k + 1);
        if (outside) base = rp;                                             // a disk of another folder / datastore: told apart by its whole path
        Str ds, path; if (!mapArc(sd, ds, path)) continue;
        Str val = (ds == m_mainDs && !sd.starts_with("_other-datastores/")) ? origOf(sd) : "[" + ds + "] " + path;
        from.push_back(base); to.push_back(val);
    }
    if (m_vmxText.empty()) return fail("the backup holds no .vmx file");
    Str vmx = EsxiRewriteVmx(m_vmxText, m_name, from, to);
    if (!putMem(m_mainDs, m_dir + "/" + vmxName, vmx)) return false;
    if (!m_vmsdText.empty()) {
        Str vmsd = EsxiRewriteVmsd(m_vmsdText, m_meta.get("snapshot").str());
        int sl = vmxName.rfind('.'); Str vname = sl > 0 ? vmxName.substr(0, (size_t)sl) : vmxName;
        if (!putMem(m_mainDs, m_dir + "/" + vname + ".vmsd", vmsd)) return false;
    }
    // 2. register (a registration that succeeded before a lost answer is found again by its path)
    Str vmxPath = "[" + m_mainDs + "] " + m_dir + "/" + vmxName;
    if (m_registered) return true;
    Str vmRef;
    {
        Str view; Vec<ObjData> objs;
        static const char* p[] = { "config.files.vmPathName" };
        if (m_s->createView("Datacenter", m_dcId, "VirtualMachine", view)) {
            if (m_s->retrieve(Impl::PropSet("VirtualMachine", p, 1), Impl::ViewSelect(view), objs)) for (const ObjData& o : objs) if (o.text("config.files.vmPathName") == vmxPath) vmRef = o.id;
            m_s->destroyView(view);
        } else return failFrom();
    }
    if (vmRef.empty()) {
        Impl::TaskResult tr;
        Str in = Ref("_this", "Folder", m_folderId) + Tag("path", vmxPath) + Tag("name", m_name) + "<asTemplate>false</asTemplate>";
        if (!m_poolId.empty()) in += Ref("pool", "ResourcePool", m_poolId);
        if (!m_hostId.empty()) in += Ref("host", "HostSystem", m_hostId);
        if (!m_s->runTask("RegisterVM_Task", in, 600, tr)) return failFrom();
        if (!tr.ok) return fail("registering the VM failed: " + (tr.faultMsg.empty() ? tr.faultType : tr.faultMsg));
        vmRef = tr.result;
    }
    m_registered = true;
    if (m_res) { m_res->vmName = m_name; m_res->vmRef = vmRef; m_res->vmxPath = vmxPath; m_res->datastore = m_mainDs; m_res->folder = m_folderId; }
    if (m_o.powerOn) {
        Impl::TaskResult tr;
        if (!m_s->runTask("PowerOnVM_Task", Ref("_this", "VirtualMachine", vmRef), 300, tr) || !tr.ok) {
            Str why = m_s->err; if (tr.ok == false && !tr.faultMsg.empty()) why = tr.faultMsg;
            if (m_res) m_res->warning = "the VM was restored and registered but could not be powered on: " + why;
        }
    }
    return true;
}

void RestoreImpl::cleanup()
{
    if (m_registered || !m_s) return;
    long a = m_s->m_abort; m_s->m_abort = 0;
    m_s->logout();
    if (m_s->ensureSession()) {
        // the folders this restore created (the top ones are enough: a delete takes the content with it)
        for (const Str& k : m_made) {
            int bar = k.find('|'); if (bar < 0) continue;
            Str ds = k.substr(0, (size_t)bar), dir = k.substr((size_t)bar + 1);
            if (dir.find('/') >= 0) continue;                                // a sub folder of one that is deleted anyway
            Impl::TaskResult tr;
            Str in = Ref("_this", "FileManager", m_s->m_fileMgr.empty() ? Str("ha-nfc-file-manager") : m_s->m_fileMgr) + Tag("name", "[" + ds + "] " + dir) + Ref("datacenter", "Datacenter", m_dcId);
            m_s->runTask("DeleteDatastoreFile_Task", in, 300, tr);
        }
    }
    m_s->m_abort = a;
    m_made.clear();
}

} // namespace

VmSource* NewEsxiSource(const Json& server, Str& err)
{
    Str type = server.get("type").str();
    if (type != "esxi" && type != "vcenter") { err = "not an ESXi / vCenter server: " + type; return nullptr; }
    if (server.get("host").str().empty()) { err = Str("the host of the ") + (type == "esxi" ? "ESXi host" : "vCenter") + " is not set"; return nullptr; }
    if (server.get("user").str().empty()) { err = "the user name of the server is not set"; return nullptr; }
    Str tr = server.get("transport").str("api");
    if (tr == "ssh") { err = "the SSH transport for ESXi is not available yet: use the API transport"; return nullptr; }
    if (tr != "api" && !tr.empty()) { err = "unknown transport: " + tr; return nullptr; }
    Str tm = server.get("tls_min").str("1.2");
    if (tm != "1.0" && tm != "1.1" && tm != "1.2" && tm != "1.3") { err = "bad tls_min: " + tm; return nullptr; }
    return new Impl(server);
}


// ---- pure rewriting of .vmx / .vmsd -------------------------------------------------------------------------------------
namespace {
static bool SplitLine(const Str& line, Str& key, Str& value)
{
    int eq = line.find('=');
    if (eq <= 0) return false;
    key = line.substr(0, (size_t)eq).trim();
    Str v = line.substr((size_t)eq + 1).trim();
    if (v.size() >= 2 && v[0] == '"' && v[v.size() - 1] == '"') v = v.substr(1, v.size() - 2);
    value = v;
    return true;
}
static bool IsDiskFileName(const Str& key)
{
    Str k = key.lower();
    if (!k.ends_with(".filename") || k.find(':') < 0) return false;
    const char* pre[] = { "scsi", "sata", "ide", "nvme" };
    for (const char* p : pre) { size_t n = strlen(p); if (k.starts_with(p) && k.size() > n && k[n] >= '0' && k[n] <= '9') return true; }
    return false;
}
static Str VmxUnescape(const Str& v)
{
    Str o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '|' && i + 2 < v.size() + 0 && isxdigit((unsigned char)v[i + 1]) && isxdigit((unsigned char)v[i + 2])) { char h[3] = { v[i + 1], v[i + 2], 0 }; o += (char)strtol(h, nullptr, 16); i += 2; }
        else o += v[i];
    }
    return o;
}
static Str BaseName(const Str& v0)
{
    Str v = VmxUnescape(v0);
    Str ds, p; Str b = v; if (EsxiParseDsPath(v, ds, p)) b = p;
    int k = b.rfind('/'); return k >= 0 ? b.substr((size_t)k + 1) : b;
}
static Vec<Str> Lines(const Str& t)
{
    Vec<Str> out; size_t s = 0;
    for (size_t i = 0; i <= t.size(); ++i) if (i == t.size() || t[i] == '\n') { Str l = t.substr(s, i - s); if (l.ends_with("\r")) l = l.substr(0, l.size() - 1); if (i < t.size() || !l.empty()) out.push_back(l); s = i + 1; }
    return out;
}
} // namespace

Str EsxiArchivePrefix(const Str& vm) { return SafeComponent(vm); }

// A value inside "..." of a .vmx: the quote, the bar and control characters (a line break would start a new line) are written as |XX hex escapes.
static Str VmxEscape(const Str& v)
{
    static const char* hex = "0123456789ABCDEF";
    Str o;
    for (size_t i = 0; i < v.size(); ++i) {
        unsigned char c = (unsigned char)v[i];
        if (c == '"' || c == '|' || c < 0x20 || c == 0x7F) { o += '|'; o += hex[c >> 4]; o += hex[c & 15]; }
        else o += (char)c;
    }
    return o;
}

Str EsxiRewriteVmx(const Str& vmx, const Str& newName, const Vec<Str>& fromBase, const Vec<Str>& toValue)
{
    Str out; bool nameDone = false, uuidAction = false;
    for (const Str& line : Lines(vmx)) {
        Str k, v;
        if (!SplitLine(line, k, v)) { out += line + "\n"; continue; }
        Str kl = k.lower();
        if (kl.starts_with("snapshot.") || kl.find("ctkenabled") >= 0 || kl.find("-ctk") >= 0 || kl.starts_with("sched.swap.derivedname")) continue;      // the restored VM has no snapshot and no change tracking state
        if (kl == "displayname") { out += "displayName = \"" + VmxEscape(newName) + "\"\n"; nameDone = true; continue; }
        if (kl == "uuid.action") { out += "uuid.action = \"keep\"\n"; uuidAction = true; continue; }
        if (IsDiskFileName(k)) {
            // an entry "[ds] folder/name.vmdk" names a disk outside the VM's folder exactly; a plain entry stands for a file in the VM's folder (by name).
            // Exact path first, then plain names, then the base names of the full paths: two disks called the same in different folders keep apart.
            Str b = BaseName(v); int hit = -1; size_t n = fromBase.size() < toValue.size() ? fromBase.size() : toValue.size();
            Str vds, vpath; bool full = EsxiParseDsPath(VmxUnescape(v), vds, vpath);
            if (full) for (size_t i = 0; i < n && hit < 0; ++i) { Str eds, ep; if (fromBase[i].starts_with("[") && EsxiParseDsPath(fromBase[i], eds, ep) && eds == vds && ep == vpath) hit = (int)i; }
            for (size_t i = 0; i < n && hit < 0; ++i) if (!fromBase[i].starts_with("[") && fromBase[i] == b) hit = (int)i;
            for (size_t i = 0; i < n && hit < 0; ++i) if (fromBase[i].starts_with("[") && BaseName(fromBase[i]) == b) hit = (int)i;
            if (hit >= 0) { out += k + " = \"" + VmxEscape(toValue[(size_t)hit]) + "\"\n"; continue; }
        }
        out += line + "\n";
    }
    if (!nameDone) out += "displayName = \"" + VmxEscape(newName) + "\"\n";
    if (!uuidAction) out += "uuid.action = \"keep\"\n";
    return out;
}

Str EsxiRewriteVmsd(const Str& vmsd, const Str& dropName)
{
    // blocks "snapshotN.*": the one whose displayName is dropName goes, the others are renumbered
    Vec<Str> lines = Lines(vmsd);
    Vec<int> idx; int maxN = -1;
    for (const Str& l : lines) {
        Str k, v; int n = -1;
        if (SplitLine(l, k, v) && k.starts_with("snapshot") && k.size() > 8 && k[8] >= '0' && k[8] <= '9') {
            size_t d = 8; while (d < k.size() && k[d] >= '0' && k[d] <= '9') ++d;
            if (d - 8 <= 4) { n = atoi(k.c_str() + 8); if (n > maxN) maxN = n; }                // a block number is small; a huge one is not a snapshot of ours (it must not size an array)
        }
        idx.push_back(n);
    }
    int drop = -1; Str dropUid;
    for (size_t i = 0; i < lines.size(); ++i) { Str k, v; if (idx[i] >= 0 && SplitLine(lines[i], k, v) && k.ends_with(".displayName") && v == dropName) drop = idx[i]; }
    for (size_t i = 0; i < lines.size(); ++i) { Str k, v; if (drop >= 0 && idx[i] == drop && SplitLine(lines[i], k, v) && k.ends_with(".uid")) dropUid = v; }
    Vec<int> remap; for (int n = 0; n <= maxN; ++n) remap.push_back(-1);
    int next = 0;
    for (int n = 0; n <= maxN; ++n) if (n != drop) remap[(size_t)n] = next++;
    int kept = next; Str out, lastUid;
    for (size_t i = 0; i < lines.size(); ++i) {
        Str k, v; bool ok = SplitLine(lines[i], k, v);
        if (idx[i] >= 0) {
            if (idx[i] == drop) continue;
            int nn = remap[(size_t)idx[i]]; int dot = k.find('.');
            if (dot < 0) continue;
            Str rest = k.substr((size_t)dot);
            if (rest == ".uid") lastUid = v;
            if (rest == ".parent" && v == dropUid) continue;
            out += Str::fmt("snapshot%d", nn) + rest + " = \"" + v + "\"\n";
            continue;
        }
        if (ok && k == "snapshot.numSnapshots") { out += Str::fmt("snapshot.numSnapshots = \"%d\"\n", kept); continue; }
        if (ok && k == "snapshot.current") continue;
        out += lines[i] + "\n";
    }
    if (kept > 0 && !lastUid.empty()) out += "snapshot.current = \"" + lastUid + "\"\n";
    return out;
}

RestoreWriter* NewEsxiRestoreWriter(const Json& server, const EsxiRestoreOptions& o, EsxiRestoreResult* result, Str& err)
{
    Str type = server.get("type").str();
    if (type != "esxi" && type != "vcenter") { err = "not an ESXi / vCenter server: " + type; return nullptr; }
    Str tr = server.get("transport").str("api");
    if (tr == "ssh") { err = "the SSH transport for ESXi is not available yet: use the API transport"; return nullptr; }
    if (server.get("host").str().empty() || server.get("user").str().empty()) { err = "the host or the user of the server is not set"; return nullptr; }
    return new RestoreImpl(server, o, result);
}

bool RestoreToEsxi(Target& target, const Str& archive, const Json& server, const EsxiRestoreOptions& o, const Str& password, const ReadOptions& ro, Progress* pr,
                   EsxiRestoreResult* result, Str& err)
{
    EsxiRestoreResult local; EsxiRestoreResult* res = result ? result : &local;
    *res = EsxiRestoreResult();
    // the limit counts what goes to the host (the upload); the reading of the archive is not limited again by the same bucket
    EsxiRestoreOptions eo = o; ReadOptions rd = ro; if (!eo.limiter) eo.limiter = ro.limiter; rd.limiter = nullptr;
    RestoreWriter* w = NewEsxiRestoreWriter(server, eo, res, err);
    if (!w) return false;
    bool ok = RestoreBackup(target, archive, *w, password, rd, pr, nullptr, err);
    delete w;
    return ok;
}

bool EsxiRestoreTargets(const Json& server, const Str& datacenter, Json& out, Str& err)
{
    EsxiRestoreOptions o; o.datacenter = datacenter;
    Str e; RestoreWriter* w = NewEsxiRestoreWriter(server, o, nullptr, e);
    if (!w) { err = e; return false; }
    bool ok = static_cast<RestoreImpl*>(w)->targets(out, err);
    delete w;
    return ok;
}

} // namespace vm2b
