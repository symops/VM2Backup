#include "svccore.h"
#include "netbrowse.h"
#if defined(VM2B_HAVE_ESXI)
#include "esxi.h"
#endif
#include "applog.h"
#include "schedule.h"
#include "winutil.h"
#include "smb.h"
#include "version.h"
#include <shlobj.h>

namespace vm2b {

static const char* const kStatuses[] = { "Success", "Warning", "Failed", "Abort", "Disabled", "Waiting", "Running", "Paused" };

static bool WebDefaultOn(const ServiceOptions& o)
{
    if (o.webDefault >= 0) return o.webDefault != 0;
    wchar_t v[8]; DWORD n = GetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", v, 8);       // a test hook that can only switch the default OFF
    return !(n == 1 && v[0] == L'0');
}

static Str PathSafe(const Str& s)
{
    Str r;
    for (size_t i = 0; i < s.size(); ++i) { char c = s[i]; r += (strchr("\\/:*?\"<>|", c) || (u8)c < 32) ? '_' : c; }
    r = TruncateUtf8(r, kMaxNameBytes);
    while (r.ends_with(".") || r.ends_with(" ")) r = r.substr(0, r.size() - 1);
    if (r.empty()) return "vm";
    size_t dot = 0; while (dot < r.size() && r[dot] != '.') ++dot;                 // CON, NUL, COM1 ... (also "nul.txt") are devices on Windows
    if (dot == 3 || dot == 4) {
        char b[5]; for (size_t i = 0; i < dot; ++i) b[i] = (char)((r[i] >= 'a' && r[i] <= 'z') ? r[i] - 32 : r[i]); b[dot] = 0;
        bool dev = dot == 3 ? (!strcmp(b, "CON") || !strcmp(b, "PRN") || !strcmp(b, "AUX") || !strcmp(b, "NUL")) : ((!strncmp(b, "COM", 3) || !strncmp(b, "LPT", 3)) && b[3] >= '0' && b[3] <= '9');
        if (dev) r = "_" + r;
    }
    return r;
}

// ---- fs.browse: the folders of THIS computer for the web site's folder picker (admin only) -------------------------------
// path "" = the drives; "X:\dir" = its sub-folders (and files with `files`).  Only local absolute paths: no UNC, no \\?\ or \\.\ forms, no "..", no wildcards.
static bool FsPathOk(const Str& p, Str& err)
{
    if (p.size() < 3 || p.size() > 1000) { err = "enter a full path such as C:\\Folder"; return false; }
    if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':' || p[2] != '\\') { err = "enter a full path such as C:\\Folder (network paths are browsed with the network picker)"; return false; }
    for (size_t i = 0; i < p.size(); ++i) { unsigned char c = (unsigned char)p[i]; if (c < 0x20 || c == 0x7f || strchr("*?<>|\"", c) || (c == ':' && i != 1)) { err = "the path contains characters that are not allowed"; return false; } }
    size_t from = 3;
    for (size_t i = 3; i <= p.size(); ++i) if (i == p.size() || p[i] == '\\') {
        Str part = p.substr(from, i - from); from = i + 1;
        if (part.empty()) { if (i == p.size()) continue; err = "the path has an empty folder name"; return false; }
        if (part == "." || part == ".." || part.ends_with(".") || part.ends_with(" ")) { err = "the path contains a name that is not allowed"; return false; }
        Str base = part; int dot = base.find('.'); if (dot >= 0) base = base.substr(0, (size_t)dot); base = base.lower();
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" || ((base.starts_with("com") || base.starts_with("lpt")) && base.size() == 4 && base[3] >= '1' && base[3] <= '9')) { err = "the path contains a device name"; return false; }
    }
    return true;
}

static bool FsBrowse(const Str& pathIn, bool withFiles, Json& r, Str& err)
{
    r = Json::object(); Json entries = Json::array();
    Str path = pathIn.trim(); for (size_t i = 0; i < path.size(); ++i) if (path[i] == '/') path[i] = '\\';
    if (path.empty()) {
        wchar_t buf[512]; DWORD n = GetLogicalDriveStringsW(511, buf);
        for (const wchar_t* d = buf; n && *d; d += wcslen(d) + 1) {
            Str root = FromWide(d); UINT ty = GetDriveTypeW(d); if (ty == DRIVE_NO_ROOT_DIR || ty == DRIVE_UNKNOWN) continue;
            Json e = Json::object(); e["name"] = root.substr(0, 2); e["path"] = root; e["kind"] = "drive"; e["drive_type"] = ty == DRIVE_FIXED ? "fixed" : ty == DRIVE_REMOTE ? "network" : ty == DRIVE_REMOVABLE ? "removable" : ty == DRIVE_CDROM ? "cdrom" : "other";
            entries.push(e);
        }
        r["path"] = ""; r["parent"] = ""; r["entries"] = entries; r["truncated"] = false; return true;
    }
    if (!FsPathOk(path, err)) return false;
    if (!path.ends_with("\\")) path += "\\";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(path + "*"), &fd);
    if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); err = (e == ERROR_FILE_NOT_FOUND ? Str("the folder is empty or does not exist") : Str("cannot read the folder: ") + WinErrorText(e)); if (e == ERROR_FILE_NOT_FOUND && GetFileAttributesW(FilePath(path)) != INVALID_FILE_ATTRIBUTES) { r["path"] = path; r["parent"] = ""; r["entries"] = entries; r["truncated"] = false; goto parent; } return false; }
    {
        bool trunc = false;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (!dir && !withFiles) continue;
            if (entries.size() >= 5000) { trunc = true; break; }
            Str name = FromWide(fd.cFileName); Json e = Json::object(); e["name"] = name; e["path"] = path + name; e["kind"] = dir ? "dir" : "file"; entries.push(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        // sorted: folders first, then by name (case-insensitive)
        Vec<Json> v; for (size_t i = 0; i < entries.size(); ++i) v.push_back(entries.at(i));
        for (size_t i = 1; i < v.size(); ++i) { Json x = v[i]; size_t j = i; auto less = [](const Json& a, const Json& b) { bool da = a.get("kind").str() == "dir", db = b.get("kind").str() == "dir"; if (da != db) return da; return a.get("name").str().lower() < b.get("name").str().lower(); };
            while (j > 0 && less(x, v[j - 1])) { v[j] = v[j - 1]; --j; } v[j] = x; }
        entries = Json::array(); for (size_t i = 0; i < v.size(); ++i) entries.push(v[i]);
        r["path"] = path; r["entries"] = entries; r["truncated"] = trunc;
    }
parent:
    {
        Str t = path.substr(0, path.size() - 1); int cut = -1; for (size_t i = t.size(); i-- > 0;) if (t[i] == '\\') { cut = (int)i; break; }
        r["parent"] = cut <= 2 ? (t.size() <= 2 ? Str() : t.substr(0, 3)) : t.substr(0, (size_t)cut);
    }
    return true;
}

Str ServiceCore::DefaultDataDir()
{
    PWSTR p = nullptr; Str dir = "C:\\ProgramData";
    static const GUID kProgramData = { 0x62ab5d82, 0xfdc1, 0x4dc3, { 0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97 } };
    if (SUCCEEDED(SHGetKnownFolderPath(kProgramData, 0, nullptr, &p)) && p) { dir = FromWide(p); CoTaskMemFree(p); }
    return JoinPath(dir, "VM2Backup");
}

// ---- a job -----------------------------------------------------------------------------------------------------
class Job : public Progress {
public:
    Job(ServiceCore* s) : svc(s), abortFlag(0), pauseFlag(0), raw(0), arch(0), volumes(0), volumesTotal(-1), elementMbit(0), started(0), ended(0),
                          thread(nullptr), finished(0), waitingForWindow(false) { InitializeCriticalSection(&cs); }
    ~Job() { if (thread) CloseHandle(thread); DeleteCriticalSection(&cs); }
    ServiceCore* svc;
    Str id, kind, name, trigger, taskId, status, phase, vm, file, retryText, message;
    Json element, args, taskJson, results;
    volatile LONG abortFlag, pauseFlag; volatile LONG64 raw, arch; volatile LONG volumes, volumesTotal; double elementMbit;
    double started, ended; HANDLE thread; volatile LONG finished; bool waitingForWindow;
    RateLimiter limiter; mutable CRITICAL_SECTION cs;
    // timings for the speed shown in the window
    double sampleT = 0; i64 sampleRaw = 0, sampleArch = 0; double rate = 0;

    void set(Str Job::*f, const Str& v) { EnterCriticalSection(&cs); this->*f = v; LeaveCriticalSection(&cs); }
    // Progress
    void onBytes(u64 n) override { InterlockedExchangeAdd64(&raw, (LONG64)n); }
    void onArchiveBytes(u64 n) override { InterlockedExchangeAdd64(&arch, (LONG64)n); }
    void onFile(const Str& f) override { set(&Job::file, f); }
    void onVolume(int done, int total) override { volumes = done; if (total >= 0) volumesTotal = total; }
    void onRetry(const Str& what, int attempt, int max, int delayMs, const Str& reason) override {
        set(&Job::retryText, Str::fmt("%s: %s - try %d of %d in %d s", what.c_str(), reason.c_str(), attempt, max, (delayMs + 999) / 1000));
    }
    void onResume(int v, u64 rawDone) override { set(&Job::message, Str::fmt("continuing after %d finished volumes (%s)", v, FormatSize(rawDone).c_str())); }
    bool aborted() override { return abortFlag || pauseFlag || svc->m_stop; }
    bool keepOnAbort() override { return !abortFlag && (pauseFlag || svc->m_stop); }

    Json live() {
        double now = UnixNow();
        EnterCriticalSection(&cs);
        Json j = Json::object();
        j["id"] = id; j["kind"] = kind; j["name"] = name; j["trigger"] = trigger; j["task_id"] = taskId; j["status"] = status; j["phase"] = phase;
        j["vm"] = vm; j["file"] = file; j["retry"] = retryText; j["message"] = message; j["started"] = started;
        j["raw_bytes"] = (i64)raw; j["archive_bytes"] = (i64)arch; j["volumes"] = (i64)volumes;
        double dt = now - sampleT; i64 cur = (i64)raw + (i64)arch;
        if (dt >= 1.0) { rate = (double)(cur - sampleRaw - sampleArch) / dt; sampleT = now; sampleRaw = (i64)raw; sampleArch = (i64)arch; }
        j["rate"] = rate; j["limit_mbit"] = limiter.bytesPerSecond() / 125000.0;
        LeaveCriticalSection(&cs);
        return j;
    }
};

// ---- the service -----------------------------------------------------------------------------------------------
ServiceCore::ServiceCore() : m_sched(nullptr), m_stop(0), m_slots(nullptr), m_slotCount(0), m_lastTick(0), m_statePersisted(0), m_started(false) { InitializeCriticalSection(&m_cs); m_catalog = Json::array(); }
ServiceCore::~ServiceCore() { stop(); DeleteCriticalSection(&m_cs); }

bool ServiceCore::start(const ServiceOptions& o, Str& err)
{
    m_opt = o;
    Str dir = o.dataDir.empty() ? DefaultDataDir() : o.dataDir;
    if (!m_cfg.open(dir, err)) return false;
    // what was running when the service stopped is started again (it continues from its journal)
    Json tasks = m_cfg.tasks();
    for (size_t i = 0; i < tasks.size(); ++i) {
        Str id = tasks.at(i).get("id").str(); Json st = m_cfg.taskState(id);
        if (st.get("last_status").str() == "Running" || st.get("last_status").str() == "Paused") { st["pending"] = true; st["last_status"] = "Paused"; m_cfg.setTaskState(id, st); }
    }
    Bytes b; Str e;
    if (ReadWholeFile(JoinPath(dir, "catalog.json").c_str(), b, e, 16 << 20)) { Json c; if (Json::parse((const char*)b.data(), b.size(), c, e) && c.is_array()) m_catalog = c; }
    m_stop = 0; m_lastTick = now();
    m_sched = CreateThread(nullptr, 0, SchedulerThread, this, 0, nullptr);
    if (!m_ipc.start(o.pipeName.empty() ? Str(kDefaultPipeName) : o.pipeName, this, err, o.pipeSddl)) { InterlockedExchange(&m_stop, 1); WaitForSingleObject(m_sched, 5000); CloseHandle(m_sched); m_sched = nullptr; return false; }
    m_started = true;
    { Str werr; if (!m_web.start(this, m_cfg.dataDir(), werr, WebDefaultOn(o))) Log(L"web administration not started: %s", Wide(werr).c_str()); }
    Log(L"service started, data in %s", Wide(dir).c_str());
    return true;
}

void ServiceCore::stop()
{
    if (!m_started) return;
    m_started = false;
    InterlockedExchange(&m_stop, 1);                                 // running jobs stop at the next opportunity and keep their journals
    m_web.stop();
    m_ipc.stop();
    if (m_sched) { WaitForSingleObject(m_sched, 10000); CloseHandle(m_sched); m_sched = nullptr; }
    m_cfg.flushState();                                              // the scheduler's last_check values (kept in memory between writes)
    for (int i = 0; i < 600 && runningJobs() > 0; ++i) Sleep(100);
    EnterCriticalSection(&m_cs);
    for (size_t i = 0; i < m_jobs.size(); ++i) { Job* j = m_jobs[i]; if (j->thread) WaitForSingleObject(j->thread, 5000); delete j; }
    m_jobs.clear();
    LeaveCriticalSection(&m_cs);
    Log(L"service stopped");
}

int ServiceCore::runningJobs() const
{
    int n = 0; EnterCriticalSection(&m_cs);
    for (size_t i = 0; i < m_jobs.size(); ++i) if (!m_jobs[i]->finished) ++n;
    LeaveCriticalSection(&m_cs);
    return n;
}

Job* ServiceCore::findJob(const Str& id)
{
    Job* r = nullptr; EnterCriticalSection(&m_cs);
    for (size_t i = 0; i < m_jobs.size(); ++i) if (m_jobs[i]->id == id) r = m_jobs[i];
    LeaveCriticalSection(&m_cs);
    return r;
}

// Finished jobs stay in the list for a minute so that the program can still see how they ended.
void ServiceCore::reapJobs()
{
    EnterCriticalSection(&m_cs);
    double t = UnixNow();
    for (size_t i = m_jobs.size(); i-- > 0;) {
        Job* j = m_jobs[i];
        if (j->finished && t - j->ended > 60) { if (j->thread) WaitForSingleObject(j->thread, 2000); delete j; m_jobs.erase(i); }
    }
    LeaveCriticalSection(&m_cs);
}

// ---- building sources and targets ------------------------------------------------------------------------------
Str ServiceCore::expandDirectory(const Json& server, const Str& tmpl, const Str& vm, double when)
{
    Str t = tmpl;
    if (t.empty()) {
        bool local = server.get("type").str() == "local";
        Str base = server.get("directory").str(local ? "" : ".");
        t = JoinPath(base.empty() ? m_cfg.settings().get("default_local_path").str() : base, "{VM}");
        if (!local) t = t.replace_all("\\", "/");
    }
    Str stamp = LocalIso(when).replace_all("T", "_").replace_all(":", "-");
    return t.replace_all("{VM}", PathSafe(vm).c_str()).replace_all("{DATETIME}", stamp.c_str());
}

bool ServiceCore::makeTarget(const Str& serverId, const Str& directory, const Str& vm, double when, Target*& out, Str& err)
{
    Json s;
    if (!m_cfg.server(serverId, true, s)) { err = "the target server no longer exists"; return false; }
    Str dir = expandDirectory(s, directory, vm, when);
    Json cfg = s; cfg["directory"] = dir;
    if (s.get("type").str() == "local") cfg["path"] = dir;
    out = MakeTarget(cfg, err);
    return out != nullptr;
}

void ServiceCore::remember(const Str& serverId, const Str& directory)
{
    EnterCriticalSection(&m_cs);
    bool have = false;
    for (size_t i = 0; i < m_catalog.size(); ++i) if (m_catalog.at(i).get("server").str() == serverId && m_catalog.at(i).get("directory").str() == directory) have = true;
    if (!have) {
        Json e = Json::object(); e["server"] = serverId; e["directory"] = directory; m_catalog.push(e);
        Str js = m_catalog.dump(true), er; WriteWholeFile(JoinPath(m_cfg.dataDir(), "catalog.json").c_str(), js.data(), js.size(), er);
    }
    LeaveCriticalSection(&m_cs);
}

double ServiceCore::effectiveMbit(Job* j, i64 nowUnix)
{
    double v = 0;
    auto lim = [&](double x) { if (x > 0 && (v <= 0 || x < v)) v = x; };
    lim(m_cfg.settings().get("speed_limit_mbit").num(0));
    lim(j->elementMbit);
    if (!j->taskJson.is_null()) lim(WindowsAt(j->taskJson.get("windows"), UnixToLocalMin(nowUnix)).mbit);
    return v;
}

// ---- running a backup element ----------------------------------------------------------------------------------
static const char* WorstOf(const Json& results)
{
    bool failed = false, abort = false, warn = false, any = false, paused = false;
    for (size_t i = 0; i < results.size(); ++i) {
        Str s = results.at(i).get("status").str();
        if (s == "Disabled") continue;
        any = true;
        if (s == "Failed") failed = true; else if (s == "Abort") abort = true; else if (s == "Warning") warn = true; else if (s == "Paused") paused = true;
    }
    return failed ? "Failed" : paused ? "Paused" : abort ? "Abort" : warn ? "Warning" : any ? "Success" : "Disabled";
}

// Is `child` the same folder as `parent` or below it? (a plain comparison of the paths: slashes, case, trailing separators)
static bool PathInside(const Str& child, const Str& parent)
{
    auto norm = [](Str p) { p = p.replace_all("/", "\\").lower(); while (p.size() > 3 && p.ends_with("\\")) p = p.substr(0, p.size() - 1); return p; };
    Str c = norm(child), q = norm(parent);
    return !q.empty() && (c == q || c.starts_with((q + "\\").c_str()));
}

void ServiceCore::backupElement(Job* j, const Json& el, const Str& taskName, Json& results, bool& stopTask)
{
    (void)taskName;
    Json sj, tj; Str err;
    const Json& vms = el.get("source").get("vms");
    if (!el.get("enabled").boolean(true)) { for (size_t i = 0; i < vms.size(); ++i) { Json r = Json::object(); r["vm"] = vms.at(i).str(); r["status"] = "Disabled"; results.push(r); } return; }
    Json settings = m_cfg.settings();
    bool haveSrc = m_cfg.server(el.get("source").get("server").str(), true, sj);
    if (haveSrc && el.get("snapshot").is_object()) sj["snapshot"] = el.get("snapshot");      // quiesce / memory options of an ESXi source
    if (haveSrc && !el.get("source").get("datacenter").str().empty()) sj["datacenter"] = el.get("source").get("datacenter");   // vCenter: one datacenter (the Snapshot tab)
    for (size_t i = 0; i < vms.size(); ++i) {
        Str vm = vms.at(i).str();
        Json r = Json::object(); r["vm"] = vm; r["status"] = "Failed"; r["started"] = UnixNow();
        double t0 = UnixNow();
        j->set(&Job::vm, vm); j->set(&Job::phase, "backup"); j->set(&Job::retryText, "");
        if (stopTask || j->abortFlag || j->pauseFlag || m_stop) {
            r["status"] = (j->pauseFlag || m_stop) && !j->abortFlag ? "Paused" : "Abort"; r["message"] = "not started"; results.push(r); continue;
        }
        if (!haveSrc) { r["message"] = "the source server no longer exists"; results.push(r); if (!el.get("on_error_next").boolean(true)) stopTask = true; continue; }
        VmSource* src = MakeSource(sj, err);
        Target* tgt = nullptr; Str terr;
        if (!src) { r["message"] = err; results.push(r); if (!el.get("on_error_next").boolean(true)) stopTask = true; continue; }
        if (!makeTarget(el.get("target").get("server").str(), el.get("target").get("directory").str(), vm, t0, tgt, terr)) { r["message"] = terr; delete src; results.push(r); if (!el.get("on_error_next").boolean(true)) stopTask = true; continue; }
        {   // a backup folder inside the folder of the VM: the next backup would contain the previous ones (growing every run)
            Json tj1; m_cfg.server(el.get("target").get("server").str(), false, tj1);
            if (sj.get("type").str() == "directory" && tj1.get("type").str() == "local") {
                Str vmDir = JoinPath(sj.get("path").str(), vm), tdir = expandDirectory(tj1, el.get("target").get("directory").str(), vm, t0);
                if (PathInside(tdir, vmDir)) {
                    r["message"] = "the backup folder (" + tdir + ") is inside the folder of the VM (" + vmDir + "): every backup would contain the previous ones; choose another folder";
                    delete src; delete tgt; results.push(r); if (!el.get("on_error_next").boolean(true)) stopTask = true; continue;
                }
            }
        }
        { Json tj0; m_cfg.server(el.get("target").get("server").str(), false, tj0); remember(el.get("target").get("server").str(), expandDirectory(tj0, el.get("target").get("directory").str(), vm, t0)); }
        BackupOptions o;
        o.codec = CodecOptions((int)el.get("compress").get("level").integer(0), (int)settings.get("threads").integer(0));
        if (el.get("compress").get("level").integer(0) == 0) o.codec.level = (int)settings.get("level").integer(0);
        if (el.get("encrypt").get("enabled").boolean()) o.password = el.get("encrypt").get("password").str();
        o.resume = el.get("resume").boolean(true); o.keep = (int)el.get("keep").integer(0); o.verify = el.get("verify").boolean(true); o.allowLive = el.get("allow_live").boolean(false);
        o.kdfIterations = (u32)settings.get("kdf_iterations").integer(kEncIterationsDefault);
        i64 seg = el.has("segment_mib") && !el.get("segment_mib").is_null() ? el.get("segment_mib").integer() : settings.get("segment_mib").integer(kDefaultSegmentMiB);
        o.segmentBytes = (u64)seg << 20;
        o.retry.attempts = (int)settings.get("retry_attempts").integer(8); o.retry.baseDelayMs = (int)settings.get("retry_base_sec").integer(2) * 1000; o.retry.maxDelayMs = (int)settings.get("retry_max_sec").integer(60) * 1000;
        if (settings.get("retry_base_ms").integer(0) > 0) o.retry.baseDelayMs = (int)settings.get("retry_base_ms").integer();       // for tests
        o.limiter = &j->limiter;
        Manifest m; Str warn, berr;
        // Another job that backs up the same VM into the same folder (two tasks that fire together) would adopt or delete the journal and the
        // volumes of this one: wait until it is done.
        Json tsv; m_cfg.server(el.get("target").get("server").str(), false, tsv);
        Str claim = el.get("target").get("server").str() + "|" + expandDirectory(tsv, el.get("target").get("directory").str(), vm, t0) + "|" + vm;
        bool claimed = false;
        for (bool announced = false;; Sleep(100)) {
            EnterCriticalSection(&m_cs);
            bool taken = false; for (const Str& k : m_claims) if (k == claim) taken = true;
            if (!taken) { m_claims.push_back(claim); claimed = true; }
            LeaveCriticalSection(&m_cs);
            if (claimed) break;
            if (!announced) { j->set(&Job::phase, "waiting for another backup of this VM"); announced = true; }
            if (j->abortFlag || j->pauseFlag || m_stop) break;
        }
        if (!claimed) {
            r["status"] = (j->pauseFlag || m_stop) && !j->abortFlag ? "Paused" : "Abort"; r["message"] = "not started"; results.push(r); delete src; delete tgt; continue;
        }
        j->set(&Job::phase, "backup");
        bool ok = BackupVm(*src, vm, *tgt, o, j, m, warn, berr);
        EnterCriticalSection(&m_cs);
        for (size_t q = 0; q < m_claims.size(); ++q) if (m_claims[q] == claim) { m_claims.erase(q); break; }
        LeaveCriticalSection(&m_cs);
        r["duration"] = UnixNow() - t0;
        if (ok) {
            r["status"] = warn.empty() ? "Success" : "Warning"; r["archive"] = m.archive; r["size"] = m.size; r["raw"] = m.rawSize; r["volumes"] = (u64)m.segments.size();
            r["codec"] = m.codec; r["encrypted"] = m.encrypted; if (!warn.empty()) r["message"] = warn;
            r["rate"] = (double)m.rawSize / (UnixNow() - t0 > 0.001 ? UnixNow() - t0 : 0.001);
        } else {
            bool paused = (berr == kAbortedMessage) && (j->pauseFlag || m_stop) && !j->abortFlag;
            r["status"] = berr == kAbortedMessage ? (paused ? "Paused" : "Abort") : "Failed"; r["message"] = berr;
            if (berr != kAbortedMessage && !el.get("on_error_next").boolean(true)) stopTask = true;
        }
        results.push(r);
        delete tgt; delete src;
    }
}

DWORD WINAPI ServiceCore::JobThread(LPVOID p)
{
    Job* j = (Job*)p;
    j->svc->runJob(j);
    return 0;
}

void ServiceCore::runJob(Job* j)
{
    // wait for a free place: at most `max_parallel` jobs run at the same time
    j->set(&Job::status, "Waiting");
    for (;;) {
        int maxp = (int)m_cfg.settings().get("max_parallel").integer(2), running = 0;
        EnterCriticalSection(&m_cs);
        for (size_t i = 0; i < m_jobs.size(); ++i) if (m_jobs[i] != j && !m_jobs[i]->finished && m_jobs[i]->status == "Running") ++running;
        if (running < maxp || m_stop || j->abortFlag) { j->status = "Running"; LeaveCriticalSection(&m_cs); break; }
        LeaveCriticalSection(&m_cs);
        Sleep(100);
    }
    j->started = UnixNow(); j->sampleT = j->started;
    Json results = Json::array();
    bool stopTask = false;
    Str taskName = j->name;
    if (j->kind == "task" || j->kind == "backup") {
        const Json& els = j->kind == "task" ? j->taskJson.get("elements") : j->args.get("elements");
        for (size_t i = 0; i < els.size(); ++i) backupElement(j, els.at(i), taskName, results, stopTask);
    } else if (j->kind == "verify" || j->kind == "restore" || j->kind == "restore_esxi") {
        Json r = Json::object(); r["vm"] = j->args.get("archive").str(); r["status"] = "Failed"; Str err;
        Target* t = nullptr;
        Json srv; m_cfg.server(j->args.get("server").str(), true, srv);
        Json cfg = srv; Str dir = j->args.get("directory").str(); cfg["directory"] = dir; if (srv.get("type").str() == "local") cfg["path"] = dir;
        t = MakeTarget(cfg, err);
        if (!t) r["message"] = err;
        else {
            Json settings = m_cfg.settings();
            ReadOptions ro; ro.retry.attempts = (int)settings.get("retry_attempts").integer(8); ro.retry.baseDelayMs = (int)settings.get("retry_base_sec").integer(2) * 1000; ro.retry.maxDelayMs = (int)settings.get("retry_max_sec").integer(60) * 1000;
            if (settings.get("retry_base_ms").integer(0) > 0) ro.retry.baseDelayMs = (int)settings.get("retry_base_ms").integer();
            ro.limiter = &j->limiter; j->set(&Job::phase, j->kind);
            Manifest m; bool ok;
            EsxiRestoreResult eres; bool isEsxi = j->kind == "restore_esxi";
            if (isEsxi) {
                // restore TO an ESXi host / vCenter: args.esxi_server = the server record, args.options = {vm_name, datacenter, datastore, folder, pool, host, power_on}
                Json es; Str ee;
                if (!m_cfg.server(j->args.get("esxi_server").str(), true, es)) { ok = false; err = "the ESXi / vCenter server was not found"; }
                else {
                    const Json& op = j->args.get("options"); EsxiRestoreOptions eo;
                    eo.vmName = op.get("vm_name").str(); eo.datacenter = op.get("datacenter").str(); eo.datastore = op.get("datastore").str(); eo.folder = op.get("folder").str();
                    eo.pool = op.get("pool").str(); eo.host = op.get("host").str(); eo.powerOn = op.get("power_on").boolean(false);
                    ro.stateDir = JoinPath(m_cfg.dataDir(), "restore-state");
                    { Str se; MakeDirs(ro.stateDir, se); }       // the engine writes the state file there and does not make the folder: without it a paused / restarted restore could never continue
                    ok = RestoreToEsxi(*t, j->args.get("archive").str(), es, eo, j->args.get("password").str(), ro, j, &eres, err);
                    m.archive = j->args.get("archive").str(); m.vm = eres.vmName;
                }
            }
            else if (j->kind == "verify") ok = VerifyBackup(*t, j->args.get("archive").str(), j->args.get("password").str(), ro, j, &m, err);
            else ok = RestoreBackup(*t, j->args.get("archive").str(), j->args.get("dest").str(), j->args.get("password").str(), ro, j, &m, err);
            if (ok) {
                r["status"] = "Success"; r["archive"] = m.archive; r["size"] = m.size; r["raw"] = m.rawSize; r["vm"] = m.vm;
                if (isEsxi) { r["restored_vm"] = eres.vmName; r["vmx"] = eres.vmxPath; if (!eres.warning.empty()) { r["status"] = "Warning"; r["message"] = eres.warning; } }
            }
            else { bool paused = err == kAbortedMessage && (j->pauseFlag || m_stop) && !j->abortFlag; r["status"] = err == kAbortedMessage ? (paused ? "Paused" : "Abort") : "Failed"; r["message"] = err; }
            delete t;
        }
        results.push(r);
    }
    j->ended = UnixNow();
    j->results = results;
    Json rec = Json::object();
    rec["id"] = j->id; rec["kind"] = j->kind; rec["name"] = j->name; rec["task_id"] = j->taskId; rec["trigger"] = j->trigger;
    rec["started"] = j->started; rec["ended"] = j->ended; rec["version"] = VM2B_VER_STR;
    rec["status"] = WorstOf(results); rec["results"] = results;
    rec["raw_bytes"] = (i64)j->raw; rec["archive_bytes"] = (i64)j->arch;
    j->set(&Job::status, rec.get("status").str());
    // a paused job (a closed window, the service stopping) is not a history entry: it continues
    bool paused = rec.get("status").str() == "Paused";
    if (!paused) m_cfg.addHistory(rec);
    if (!j->taskId.empty()) {
        Json st = m_cfg.taskState(j->taskId);
        st["last_end"] = j->ended; st["last_start"] = j->started; st["last_status"] = rec.get("status").str(); st["pending"] = paused && !j->abortFlag;
        m_cfg.setTaskState(j->taskId, st);
    }
    InterlockedExchange(&j->finished, 1);
}

Str ServiceCore::startTaskJob(const Str& taskId, const char* trigger, Str& err)
{
    EnterCriticalSection(&m_cs);
    for (size_t i = 0; i < m_jobs.size(); ++i) if (!m_jobs[i]->finished && m_jobs[i]->taskId == taskId) { LeaveCriticalSection(&m_cs); err = "the task is already running"; return ""; }
    LeaveCriticalSection(&m_cs);
    Json t;
    if (!m_cfg.task(taskId, true, t)) { err = "task not found"; return ""; }
    Job* j = new Job(this);
    j->id = m_cfg.newId(); j->kind = "task"; j->name = t.get("name").str(); j->trigger = trigger; j->taskId = taskId; j->taskJson = t;
    j->status = "Waiting"; j->started = UnixNow();
    Json st = m_cfg.taskState(taskId); st["last_start"] = j->started; st["last_status"] = "Running"; st["pending"] = false; m_cfg.setTaskState(taskId, st);
    j->limiter.setMbit(effectiveMbit(j, now()));
    EnterCriticalSection(&m_cs); m_jobs.push_back(j); LeaveCriticalSection(&m_cs);
    j->thread = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
    return j->id;
}

Str ServiceCore::startElementJob(const Json& element, const Str& name, const char* trigger, Str& err)
{
    Json el = element;
    if (!Config::ValidateElement(el, m_cfg, err)) return "";
    Json enc = el.get("encrypt");
    Job* j = new Job(this);
    j->id = m_cfg.newId(); j->kind = "backup"; j->name = name.empty() ? Str("Backup") : name; j->trigger = trigger; j->status = "Waiting";
    Json a = Json::object(); Json els = Json::array(); els.push(el); a["elements"] = els; j->args = a;
    j->elementMbit = el.get("speed_mbit").num(0);
    j->limiter.setMbit(effectiveMbit(j, now()));
    EnterCriticalSection(&m_cs); m_jobs.push_back(j); LeaveCriticalSection(&m_cs);
    j->thread = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
    return j->id;
}

Str ServiceCore::startOpJob(const char* kind, const Json& args, Str& err)
{
    Json srv; if (!m_cfg.server(args.get("server").str(), false, srv)) { err = "server not found"; return ""; }
    if (args.get("archive").str().empty()) { err = "choose a backup"; return ""; }
    if (Str(kind) == "restore" && args.get("dest").str().empty()) { err = "choose the folder to restore to"; return ""; }
    if (Str(kind) == "restore") { Str d = SmbDriveUnavailableMessage(args.get("dest").str()); if (!d.empty()) { err = d; return ""; } }          // a drive letter the service cannot see
    if (Str(kind) == "restore_esxi" && args.get("esxi_server").str().empty()) { err = "choose the ESXi / vCenter server to restore to"; return ""; }
    Job* j = new Job(this);
    j->id = m_cfg.newId(); j->kind = kind; j->name = args.get("archive").str(); j->trigger = "manual"; j->status = "Waiting"; j->args = args;
    j->limiter.setMbit(effectiveMbit(j, now()));
    EnterCriticalSection(&m_cs); m_jobs.push_back(j); LeaveCriticalSection(&m_cs);
    j->thread = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
    return j->id;
}

// ---- the scheduler ---------------------------------------------------------------------------------------------
DWORD WINAPI ServiceCore::SchedulerThread(LPVOID p) { ((ServiceCore*)p)->schedulerLoop(); return 0; }

void ServiceCore::schedulerLoop()
{
    while (!m_stop) {
        tick();
        for (int waited = 0; waited < m_opt.tickMs && !m_stop; waited += 20) Sleep(20);
    }
}

void ServiceCore::tick()
{
    i64 t = now();
    reapJobs();
    // bandwidth limits follow the windows; a job whose window has closed is paused (it keeps its finished volumes)
    EnterCriticalSection(&m_cs);
    for (size_t i = 0; i < m_jobs.size(); ++i) {
        Job* j = m_jobs[i]; if (j->finished) continue;
        j->limiter.setMbit(effectiveMbit(j, t));
        if (j->kind == "task" && j->trigger == "schedule" && !j->pauseFlag) {
            WindowState w = WindowsAt(j->taskJson.get("windows"), UnixToLocalMin(t));
            if (!w.allowed) { InterlockedExchange(&j->pauseFlag, 1); Log(L"task paused: its time window has closed"); }
        }
    }
    int active = 0; for (size_t i = 0; i < m_jobs.size(); ++i) if (!m_jobs[i]->finished) ++active;
    LeaveCriticalSection(&m_cs);
    // "Prevent sleep mode while the scheduler is enabled / a job runs": the flag belongs to this thread, so it is set from here
    SetThreadExecutionState(ES_CONTINUOUS | ((m_cfg.settings().get("prevent_sleep").boolean() && (active || m_cfg.schedulerEnabled())) ? ES_SYSTEM_REQUIRED : 0));
    if (!m_cfg.schedulerEnabled()) { m_lastTick = t; return; }
    Json settings = m_cfg.settings();
    i64 grace = settings.get("misfire_grace_min").integer(15) * 60;
    Json tasks = m_cfg.tasks();
    bool persisted = false;
    for (size_t i = 0; i < tasks.size(); ++i) {
        const Json& tk = tasks.at(i);
        if (!tk.get("enabled").boolean(true)) continue;
        Str id = tk.get("id").str();
        Json st = m_cfg.taskState(id);
        bool running = false;
        EnterCriticalSection(&m_cs);
        for (size_t k = 0; k < m_jobs.size(); ++k) if (!m_jobs[k]->finished && m_jobs[k]->taskId == id) running = true;
        LeaveCriticalSection(&m_cs);
        if (running) continue;
        i64 last = st.has("last_check") ? st.get("last_check").integer() : m_lastTick;
        bool due = st.get("pending").boolean(false), wasPending = due, important = false;
        // The newest slot since the last check decides: a job that overran several slots must not hide a newer slot that is still within the grace time
        // behind an older, too late one (slots older than the grace time are dropped anyway, so the search starts there).
        i64 next = 0, from = last;
        if (t - grace - 1 > from) from = t - grace - 1;
        bool any = false;
        for (int it = 0; it < 5000; ++it) { i64 n; if (!ScheduleNext(tk.get("schedule"), any ? next : from, n) || n > t) break; next = n; any = true; }
        if (any) due = true;
        else if (from > last) { i64 n; if (ScheduleNext(tk.get("schedule"), last, n) && n <= t) Log(L"a scheduled run was missed (too late to start it)"); }
        if (due) {
            WindowState w = WindowsAt(tk.get("windows"), UnixToLocalMin(t));
            if (w.allowed) {
                Str err; Str jid = startTaskJob(id, "schedule", err);
                if (jid.empty()) Log(L"cannot start a scheduled task: %s", Wide(err).c_str());
                else { st["pending"] = false; important = true; }
            } else st["pending"] = true;                                      // waits for its window
        }
        if (st.get("pending").boolean(false) != wasPending) important = true;
        st["last_check"] = t;
        // last_check moves every second: written to state.json (a flush + write-through of the whole file) only when something real changed or every 30 s,
        // not once per task per tick
        bool stale = t - m_statePersisted >= 30 || t < m_statePersisted;
        m_cfg.setTaskState(id, st, important || stale);
        if (important || stale) persisted = true;
    }
    if (persisted) m_statePersisted = t;
    m_lastTick = t;
}

// ---- the commands ----------------------------------------------------------------------------------------------
static Json JobsJson(ServiceCore* s, Vec<Job*>& jobs, CRITICAL_SECTION& cs)
{
    (void)s;
    Json a = Json::array();
    EnterCriticalSection(&cs);
    for (size_t i = 0; i < jobs.size(); ++i) a.push(jobs[i]->live());
    LeaveCriticalSection(&cs);
    return a;
}

bool ServiceCore::listTarget(const Json& args, Json& result, Str& err)
{
    Str serverId = args.get("server").str(), dir = args.get("directory").str();
    Json found = Json::array(), resumable = Json::array();
    Vec<Str> dirs;
    Json srv;
    if (!m_cfg.server(serverId, true, srv)) { err = "server not found"; return false; }
    if (!dir.empty()) dirs.push_back(dir);
    else {
        EnterCriticalSection(&m_cs);
        for (size_t i = 0; i < m_catalog.size(); ++i) if (m_catalog.at(i).get("server").str() == serverId) dirs.push_back(m_catalog.at(i).get("directory").str());
        LeaveCriticalSection(&m_cs);
        if (srv.get("type").str() == "local") {                                          // the default folder and its sub-folders (one per VM)
            Str base = srv.get("directory").str();
            dirs.push_back(base);
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(base, "*")), &fd);
            if (h != INVALID_HANDLE_VALUE) { do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) dirs.push_back(JoinPath(base, FromWide(fd.cFileName))); } while (FindNextFileW(h, &fd)); FindClose(h); }
        }
    }
    for (size_t d = 0; d < dirs.size(); ++d) {
        bool dup = false; for (size_t k = 0; k < d; ++k) if (dirs[k] == dirs[d]) dup = true;
        if (dup) continue;
        Json cfg = srv; cfg["directory"] = dirs[d]; if (srv.get("type").str() == "local") cfg["path"] = dirs[d];
        Str e; Target* t = MakeTarget(cfg, e);
        if (!t) continue;
        Vec<Manifest> ms; Vec<ResumeInfo> rs; Str le;
        if (t->connect() && ListBackups(*t, nullptr, ms, le)) {
            for (const Manifest& m : ms) { Json o = m.toJson(); o.remove("segments"); o["volumes"] = (u64)m.segments.size(); o["server"] = serverId; o["directory"] = dirs[d]; found.push(o); }
            ListResumable(*t, nullptr, rs, le);
            for (const ResumeInfo& r : rs) { Json o = Json::object(); o["vm"] = r.vm; o["archive"] = r.archive; o["stamp"] = r.stamp; o["created"] = r.created; o["volumes_done"] = r.volumesDone; o["stored_bytes"] = r.storedBytes; o["raw_bytes"] = r.rawBytes; o["server"] = serverId; o["directory"] = dirs[d]; resumable.push(o); }
        } else if (!dir.empty()) { err = le.empty() ? t->err : le; delete t; return false; }
        delete t;
    }
    result = Json::object(); result["backups"] = found; result["resumable"] = resumable;
    return true;
}

bool ServiceCore::handle(const Str& cmd, const Json& a, Json& r, Str& err)
{
    r = Json::object();
    if (cmd == "state") {
        r["version"] = VM2B_VER_STR; r["cpus"] = (i64)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); r["data_dir"] = m_cfg.dataDir(); r["scheduler"] = m_cfg.schedulerEnabled(); r["now"] = (i64)now();
        r["jobs"] = JobsJson(this, m_jobs, m_cs); r["settings"] = m_cfg.settings(); return true;
    }
    if (cmd == "service.shutdown") { if (!m_opt.onShutdown) { err = "this service cannot be stopped from here"; return false; } m_opt.onShutdown(); return true; }
    if (cmd == "scheduler.set") { m_cfg.setSchedulerEnabled(a.get("enabled").boolean()); r["scheduler"] = m_cfg.schedulerEnabled(); return true; }
    if (cmd == "settings.get") { r = m_cfg.settings(); return true; }
    if (cmd == "settings.set") { if (!m_cfg.setSettings(a, err)) return false; r = m_cfg.settings(); return true; }
    if (cmd == "servers.list") { r = m_cfg.servers(); return true; }
    if (cmd == "servers.save") { Json s; if (!m_cfg.saveServer(a.get("server"), s, err)) return false; r = s; return true; }
    if (cmd == "servers.delete") return m_cfg.deleteServer(a.get("id").str(), err);
    if (cmd == "servers.test" || cmd == "servers.vms") {
        Json s;
        if (a.has("id") && !a.get("id").str().empty()) { if (!m_cfg.server(a.get("id").str(), true, s)) { err = "server not found"; return false; } }
        else {                                                   // an unsaved server from the wizard; its stored password is used when the client sends the mask
            s = a.get("server"); Str id = s.get("id").str(); Json old;
            if (!id.empty() && m_cfg.server(id, true, old) && old.get("type").str() == s.get("type").str()) {      // like servers.save: what the form did not send comes from the stored record
                const char* f[] = { "password", "key_passphrase", "key_text" }; for (const char* k : f) if (s.get(k).str() == kMask) s[k] = old.get(k);
                for (size_t i = 0; i < old.size(); ++i) if (!s.has(old.key_at(i).c_str()) && !old.key_at(i).starts_with("has_")) s[old.key_at(i).c_str()] = old.value_at(i);
            }
        }
        Str type = s.get("type").str(); Str e2;
        if (type == "directory" || type == "esxi" || type == "vcenter") {
            VmSource* src = MakeSource(s, e2);
            if (!src) { err = e2; return false; }
            Vec<VmInfo> vms; bool ok = src->list(vms);
            if (!ok) { err = src->err; delete src; return false; }
            Json arr = Json::array(); for (const VmInfo& v : vms) { Json o = Json::object(); o["name"] = v.name; o["state"] = v.state; o["size"] = v.size; if (!v.extra.empty()) o["extra"] = v.extra; arr.push(o); }
            r["vms"] = arr; r["message"] = Str::fmt("%d virtual machine(s) found", (int)vms.size());
            delete src; return true;
        }
        Json cfg = s; if (type == "local") { cfg["path"] = s.get("directory").str(); }
        Target* t = MakeTarget(cfg, e2);
        if (!t) { err = e2; return false; }
        bool ok = t->connect(); Vec<Str> names; if (ok) ok = t->list(names);
        if (!ok) { err = t->err; delete t; return false; }
        r["message"] = "connected"; r["files"] = (u64)names.size();
        if (type == "smb") {                                                  // a network folder: prove that we can write and delete, and tell the free space
            static const char* kProbe = ".vm2b-test.tmp";
            if (!t->put_bytes(kProbe, "VM2Backup", 9)) { err = "the connection works, but a file cannot be created in the folder: " + t->err; delete t; return false; }
            t->remove(kProbe);
            u64 fb = 0; Str msg = "connected, a test file was written and deleted";
            if (t->free_space(fb)) msg += "; free space: " + FormatSize(fb);
            r["message"] = msg;
        }
        delete t; return true;
    }
    if (cmd == "fs.browse") return FsBrowse(a.get("path").str(), a.get("files").boolean(), r, err);     // admin only (the web role table)
    if (cmd == "net.drives") return NetListDrives(r, err);                      // admin only (the web role table); see netbrowse.h
    if (cmd == "net.browse") return NetBrowse(a.get("path").str(), a.get("user").str(), a.get("password").str(), r, err);
    if (cmd == "tasks.list") { r = m_cfg.tasks(); return true; }
    if (cmd == "tasks.save") { Json t; if (!m_cfg.saveTask(a.get("task"), t, err)) return false; r = t; return true; }
    if (cmd == "tasks.delete") { if (!m_cfg.deleteTask(a.get("id").str(), err)) return false; return true; }
    if (cmd == "tasks.run") { Str id; if (!m_cfg.findTask(a.get("id").str(), id)) { err = "task not found"; return false; } Str jid = startTaskJob(id, "manual", err); if (jid.empty()) return false; r["job"] = jid; return true; }
    if (cmd == "tasks.copy") { Json saved; if (!m_cfg.copyTask(a.get("id").str(), a.get("name").str(), saved, err)) return false; r = saved; return true; }
    if (cmd == "tasks.next") {
        Json sched; i64 n;
        if (a.get("schedule").is_object()) {                                          // a schedule that is being edited (not saved yet): validated, then the next start is computed
            sched = a.get("schedule"); if (!ScheduleValidate(sched, err)) return false;
        } else {
            Str id; if (!m_cfg.findTask(a.get("id").str(), id)) { err = "task not found"; return false; }
            Json t; m_cfg.task(id, false, t); sched = t.get("schedule");
        }
        if (ScheduleNext(sched, now(), n)) { r["next"] = n; r["text"] = FormatLocalMin(UnixToLocalMin(n)); } else r["next"] = Json();
        r["description"] = ScheduleDescribe(sched); return true;
    }
    if (cmd == "backup.run") { Str jid = startElementJob(a.get("element"), a.get("name").str(), "manual", err); if (jid.empty()) return false; r["job"] = jid; return true; }
    if (cmd == "jobs.list") { r = JobsJson(this, m_jobs, m_cs); return true; }
    if (cmd == "jobs.abort") { Job* j = findJob(a.get("id").str()); if (!j) { err = "no such job"; return false; } InterlockedExchange(&j->abortFlag, 1); return true; }
    if (cmd == "history.list") { r = m_cfg.history((int)a.get("limit").integer(200)); return true; }
    if (cmd == "history.get") { if (!m_cfg.historyRecord(a.get("id").str(), r)) { err = "no such record"; return false; } return true; }
    if (cmd == "history.clear") { m_cfg.clearHistory(); return true; }
    if (cmd == "backups.list") return listTarget(a, r, err);
    if (cmd == "backups.verify" || cmd == "backups.restore" || cmd == "backups.restore_esxi") {
        Str jid = startOpJob(cmd == "backups.verify" ? "verify" : cmd == "backups.restore" ? "restore" : "restore_esxi", a, err); if (jid.empty()) return false; r["job"] = jid; return true;
    }
    if (cmd == "esxi.restore_options") {            // what a restore dialog can offer: datastores, folders, pools, hosts, existing VM names (see esxi.h)
        Json es; if (!m_cfg.server(a.get("server").str(), true, es)) { err = "server not found"; return false; }
        return EsxiRestoreTargets(es, a.get("datacenter").str(), r, err);
    }
    if (cmd == "backups.delete" || cmd == "resumable.discard") {
        Json srv; if (!m_cfg.server(a.get("server").str(), true, srv)) { err = "server not found"; return false; }
        Json cfg = srv; Str dir = a.get("directory").str(); cfg["directory"] = dir; if (srv.get("type").str() == "local") cfg["path"] = dir;
        Target* t = MakeTarget(cfg, err); if (!t) return false;
        bool ok = cmd == "backups.delete" ? DeleteBackup(*t, a.get("archive").str(), err) : DiscardResumable(*t, a.get("archive").str(), err);
        delete t; return ok;
    }
    if (cmd.size() > 4 && memcmp(cmd.c_str(), "web.", 4) == 0) return m_web.handleAdmin(cmd, a, r, err);
    err = "unknown command: " + cmd;
    return false;
}

} // namespace vm2b
