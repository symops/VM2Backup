// Autotests of the RestoreWriter abstraction of the engine, with a fake in-memory writer (byte-resumable and non-resumable flavours),
// a target that loses its connection while a volume is read, and a writer that fails on purpose.
#include "autotest.h"
#include "engine.h"
#include "winutil.h"

namespace vm2b {

namespace {

struct RTmp {
    Str path; bool keep;
    RTmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path); Str e; MakeDirs(path, e);
    }
    ~RTmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

struct RFile { Str rel; Bytes data; };

static void PutFile(const Str& path, const Bytes& b)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    WriteWholeFile(path.c_str(), b.data(), b.size(), e);
}

// a random VM folder; returns the files
static void MakeFolder(Ctx& c, const Str& root, const Str& vm, Vec<RFile>& files)
{
    int n = c.range(2, 9);
    for (int i = 0; i < n; ++i) {
        RFile f; f.rel = Str::fmt("%sf%d.%s", c.coin(30) ? "sub/" : "", i, c.coin() ? "vmdk" : "vmx");
        size_t sz = c.coin(15) ? 0 : c.coin(30) ? (size_t)c.range(100000, 900000) : (size_t)c.range(1, 40000);
        f.data.resize(sz); if (c.coin()) c.fill(f.data.data(), sz); else for (size_t k = 0; k < sz; ++k) f.data[k] = (u8)((k / 7) & 0x3f);
        PutFile(JoinPath(JoinPath(root, vm), f.rel.replace_all("/", "\\")), f.data);
        files.push_back(f);
    }
}

// the destination of the fake writer: lives outside the writer, so that it survives "runs" (a datastore)
struct Store { Vec<Str> names; Vec<Bytes> data; Vec<Str> dirs;
    int find(const Str& n) const { for (size_t i = 0; i < names.size(); ++i) if (names[i] == n) return (int)i; return -1; }
    Bytes& get(const Str& n) { int i = find(n); if (i < 0) { names.push_back(n); data.push_back(Bytes()); i = (int)names.size() - 1; } return data[(size_t)i]; } };

struct FakeWriter : RestoreWriter {
    Store& st; bool resumable; Str stateDir; Str key;
    int failures; u64 gapMin, gapMax; Ctx& c; u64 moved, nextAt;           // transient failures after random numbers of bytes written
    int reconnects, opens, continues, cleaned, finished, checkpoints; bool permanentAtEnd, cleanup_; int marker;
    FakeWriter(Ctx& cc, Store& s, bool r, const Str& dir, const Str& k, int nfail, u64 mn, u64 mx)
        : st(s), resumable(r), stateDir(dir), key(k), failures(nfail), gapMin(mn), gapMax(mx), c(cc), moved(0), nextAt(0), reconnects(0), opens(0), continues(0), cleaned(0), finished(0), checkpoints(0),
          permanentAtEnd(false), cleanup_(false), marker(4711) { sched(); }
    void sched() { nextAt = moved + gapMin + (gapMax > gapMin ? c.next() % (gapMax - gapMin) : 0); }
    const char* kind() const override { return "fake"; }
    bool canResumeInsideFile() const override { return resumable; }
    Str stateFile(const ReadOptions&, const Str&, const Str&) override { return JoinPath(stateDir, "fake-" + key + ".json"); }
    Json saveState() override { Json j = Json::object(); j["marker"] = marker; return j; }
    bool loadState(const Json& j) override { return j.get("marker").integer() == marker; }
    bool prepare(const Manifest&, bool) override { return true; }
    bool makeDir(const Str& rel) override { st.dirs.push_back(rel); return true; }
    bool canContinue(const RestoreFile& f) override { if (!f.resumeAt) return true; int i = st.find(f.rel); return resumable && i >= 0 && st.data[(size_t)i].size() >= f.resumeAt; }
    bool writeFile(const RestoreFile& f, Source& body, RestoreTick* tick) override {
        ++opens;
        Bytes& d = st.get(f.rel);
        if (f.resumeAt) { ++continues; if (d.size() < f.resumeAt) return fail("fake: cannot continue"); d.resize((size_t)f.resumeAt); } else d.clear();
        u8 buf[7000]; i64 k;
        while ((k = body.read(buf, sizeof(buf))) > 0) {
            size_t o = d.size(); d.resize(o + (size_t)k); memcpy(d.data() + o, buf, (size_t)k);
            moved += (u64)k;
            if (tick) tick->chunk((size_t)k);
            if (failures > 0 && moved >= nextAt) { --failures; sched(); return fail("fake: connection lost", true); }
        }
        if (k < 0) return fail(body.err, body.transient);
        return true;
    }
    void checkpoint() override { ++checkpoints; }
    bool reconnect() override { ++reconnects; return true; }
    bool finish() override { ++finished; if (permanentAtEnd) return fail("fake: refused at the end"); return true; }
    bool cleansUpOnFailure() const override { return cleanup_; }
    void cleanup() override { ++cleaned; st.names.clear(); st.data.clear(); }
};

struct RPr : Progress {
    u64 bytes; int retries, resumes; bool abortAt, keep; u64 limit;
    RPr() : bytes(0), retries(0), resumes(0), abortAt(false), keep(false), limit(0) {}
    void onArchiveBytes(u64 n) override { bytes += n; }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    void onResume(int, u64) override { ++resumes; }
    bool aborted() override { return abortAt && bytes >= limit; }
    bool keepOnAbort() override { return keep; }
};

static RetryPolicy FastR(int a = 12) { RetryPolicy r; r.attempts = a; r.baseDelayMs = 1; r.maxDelayMs = 3; return r; }

static bool SameStore(Ctx& c, Store& st, const Str& vm, const Vec<RFile>& files)
{
    bool ok = true;
    for (const RFile& f : files) {
        int i = st.find(vm + "/" + f.rel);
        if (i < 0) { c.note("missing %s", f.rel.c_str()); ok = false; continue; }
        const Bytes& d = st.data[(size_t)i];
        if (d.size() != f.data.size() || (d.size() && memcmp(d.data(), f.data.data(), d.size()))) { c.note("differs %s (%d vs %d)", f.rel.c_str(), (int)d.size(), (int)f.data.size()); ok = false; }
    }
    return ok;
}

static bool MakeBackup(Ctx& c, RTmp& tmp, const Str& vm, Vec<RFile>& files, BackupOptions& o, Manifest& m, Target*& t)
{
    MakeFolder(c, tmp.sub("vms"), vm, files);
    o.kdfIterations = 3000; o.retry = FastR(); o.codec.level = 1; o.verify = false;
    o.segmentBytes = (u64)512 * (u64)c.range(64, 700);
    if (c.coin(30)) o.password = "pw-\xd0\xbf";
    Json tc = Json::object(); tc["type"] = "local"; tc["path"] = tmp.sub("store"); Str e; t = MakeTarget(tc, e);
    VmSource* src = NewDirectorySource(tmp.sub("vms"));
    Str warn, err; bool ok = BackupVm(*src, vm, *t, o, nullptr, m, warn, err);
    delete src;
    if (!ok) c.note("backup: %s", err.c_str());
    return ok;
}

} // namespace

VM2B_TEST("restore", writer_fake_flavours_survive_failures)
{
    for (int flavour = 0; flavour < 2; ++flavour) {
        RTmp tmp(c_, "rw");
        Vec<RFile> files; BackupOptions o; Manifest m; Target* t = nullptr;
        T_CHECK(MakeBackup(c_, tmp, "vm one", files, o, m, t)); if (!t) return false;
        Store st; bool resumable = flavour == 0;
        int planned = c_.range(2, 5);
        FakeWriter w(c_, st, resumable, tmp.path, "a", planned, 30000, 300000);
        ReadOptions ro; ro.retry = FastR(); RPr pr; Str err;
        bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
        if (!ok) c_.note("restore: %s", err.c_str());
        T_CHECK(ok);
        T_CHECK(SameStore(c_, st, "vm one", files));
        int fired = planned - w.failures;
        T_CHECK(w.reconnects == fired); T_CHECK(pr.retries == fired); T_CHECK(w.finished == 1);
        if (!resumable) T_CHECK(w.continues == 0);               // a writer that cannot append is never asked to continue a file
        else c_.note("resumable: %d files opened %d times, %d continued, %d checkpoints", (int)files.size(), w.opens, w.continues, w.checkpoints);
        T_CHECK(!FileExists(JoinPath(tmp.path, "fake-a.json")));
        delete t;
    }
    return true;
}

VM2B_TEST("restore", writer_state_survives_a_pause_and_a_new_run)
{
    for (int flavour = 0; flavour < 2; ++flavour) {
        RTmp tmp(c_, "rw2");
        Vec<RFile> files; BackupOptions o; Manifest m; Target* t = nullptr;
        T_CHECK(MakeBackup(c_, tmp, "vm two", files, o, m, t)); if (!t) return false;
        Store st; bool resumable = flavour == 0;
        ReadOptions ro; ro.retry = FastR(); Str err;
        u64 total = 0; for (const Segment& s : m.segments) total += s.size;
        {   // run 1: paused half way
            FakeWriter w(c_, st, resumable, tmp.path, "b", 0, 0, 0); w.cleanup_ = true;
            RPr pr; pr.abortAt = true; pr.keep = true; pr.limit = total / 2 + 1;
            bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
            if (m.segments.size() > 2 && (ok || err != kAbortedMessage)) c_.note("pause: ok=%d err='%s' segments=%d total=%llu", (int)ok, err.c_str(), (int)m.segments.size(), (unsigned long long)total);
            if (m.segments.size() > 2) { T_CHECK(!ok); T_CHECK(err == kAbortedMessage); T_CHECK(w.cleaned == 0); }
        }
        if (FileExists(JoinPath(tmp.path, "fake-b.json"))) {   // run 2: continues
            FakeWriter w(c_, st, resumable, tmp.path, "b", 0, 0, 0);
            RPr pr; bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
            if (!ok) c_.note("run 2: %s", err.c_str());
            T_CHECK(ok); T_CHECK(pr.resumes == 1); T_CHECK(SameStore(c_, st, "vm two", files));
            if (!resumable) T_CHECK(w.continues == 0);
            T_CHECK(!FileExists(JoinPath(tmp.path, "fake-b.json")));
        }
        // a writer whose saved state does not fit makes the restore start over (no resume)
        {
            Store st2; FakeWriter w1(c_, st2, resumable, tmp.path, "c", 0, 0, 0);
            RPr pr1; pr1.abortAt = true; pr1.keep = true; pr1.limit = total / 2 + 1;
            RestoreBackup(*t, m.archive, w1, o.password, ro, &pr1, nullptr, err);
            if (FileExists(JoinPath(tmp.path, "fake-c.json"))) {
                FakeWriter w2(c_, st2, resumable, tmp.path, "c", 0, 0, 0); w2.marker = 1;
                RPr pr2; T_CHECK(RestoreBackup(*t, m.archive, w2, o.password, ro, &pr2, nullptr, err)); T_CHECK(pr2.resumes == 0); T_CHECK(SameStore(c_, st2, "vm two", files));
            }
        }
        delete t;
    }
    return true;
}

VM2B_TEST("restore", writer_cleanup_rules)
{
    RTmp tmp(c_, "rw3");
    Vec<RFile> files; BackupOptions o; Manifest m; Target* t = nullptr;
    T_CHECK(MakeBackup(c_, tmp, "vm three", files, o, m, t)); if (!t) return false;
    ReadOptions ro; ro.retry = FastR(2); Str err;
    u64 total = 0; for (const Segment& s : m.segments) total += s.size;
    {   // cancel (abort without keep): the writer cleans up, the state is gone
        Store st; FakeWriter w(c_, st, false, tmp.path, "d", 0, 0, 0); w.cleanup_ = true;
        RPr pr; pr.abortAt = true; pr.limit = total / 2 + 1;
        bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
        if (m.segments.size() > 2) { T_CHECK(!ok); T_CHECK(w.cleaned == 1); T_CHECK(!FileExists(JoinPath(tmp.path, "fake-d.json"))); T_CHECK(st.names.empty()); }
    }
    {   // a permanent failure at the end (the register step of ESXi): cleaned up as well
        Store st; FakeWriter w(c_, st, false, tmp.path, "e", 0, 0, 0); w.cleanup_ = true; w.permanentAtEnd = true;
        RPr pr; bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
        T_CHECK(!ok); T_CHECK(err == "fake: refused at the end"); T_CHECK(w.cleaned == 1); T_CHECK(!FileExists(JoinPath(tmp.path, "fake-e.json")));
    }
    {   // the connection never comes back: retries used up -> the state stays for a later run, nothing is cleaned
        Store st; FakeWriter w(c_, st, false, tmp.path, "f", 500, 100, 101); w.cleanup_ = true;
        RPr pr; bool ok = RestoreBackup(*t, m.archive, w, o.password, ro, &pr, nullptr, err);
        u64 tot = 0; for (const RFile& f : files) tot += f.data.size();
        // the fake writer loses the connection at its first call and then about every 100 bytes: with little data the retries (2) may
        // or may not be enough, so either outcome is legal; the invariants must hold in both
        if (ok) { T_CHECK(w.cleaned == 0); T_CHECK(SameStore(c_, st, "vm three", files)); }
        else {
            T_CHECK(w.cleaned == 0); T_CHECK(FileExists(JoinPath(tmp.path, "fake-f.json")));
            FakeWriter w2(c_, st, false, tmp.path, "f", 0, 0, 0);
            RPr pr2; bool ok2 = RestoreBackup(*t, m.archive, w2, o.password, ro, &pr2, nullptr, err); if (!ok2) c_.note("second run failed: %s (first run: %llu bytes of data)", err.c_str(), (unsigned long long)tot); T_CHECK(ok2); T_CHECK(SameStore(c_, st, "vm three", files)); }
    }
    {   // a wrong password is permanent and cleans up
        if (!o.password.empty()) { Store st; FakeWriter w(c_, st, false, tmp.path, "g", 0, 0, 0); w.cleanup_ = true; RPr pr;
            T_CHECK(!RestoreBackup(*t, m.archive, w, "wrong", ro, &pr, nullptr, err)); T_CHECK(err == "wrong password"); }
    }
    delete t;
    return true;
}

} // namespace vm2b
