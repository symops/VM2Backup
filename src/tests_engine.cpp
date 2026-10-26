// Autotests of the engine: backup / verify / restore of random VM folders with xz, encryption, volumes, retention,
// abort, and RESUMING after lost connections (a target that fails on purpose).
#include "autotest.h"
#include "engine.h"
#include "winutil.h"

namespace vm2b {

struct TmpDir {
    Str path; bool keep;
    TmpDir(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path);                                          // a fresh folder: leftovers of an earlier (crashed) run must not leak in
        Str e; MakeDirs(path, e);
    }
    ~TmpDir() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
    Str sub(const Str& n) const { return JoinPath(path, n); }
};

static bool WriteFileBytes(const Str& path, const void* p, size_t n)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    HANDLE h = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = n == 0 || (WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n);
    CloseHandle(h); return ok;
}

static Bytes Content(Ctx& c, size_t n, int kind)
{
    Bytes b; b.resize(n);
    if (kind == 0) return b;
    if (kind == 1) { c.fill(b.data(), n); return b; }
    size_t i = 0; static const char* w[] = { "disk ", "flat ", "vmdk ", "config ", "esxi\n", "snapshot " };
    while (i < n) { const char* s = w[c.rnd(6)]; for (; *s && i < n; ++s, ++i) b[i] = (u8)*s; }
    return b;
}

struct FileRec { Str rel; Bytes data; };

// Creates <root>/<vm>/... with a random set of files; `maxBig` bounds the size of the big ones.
static void MakeVm(Ctx& c, const Str& root, const Str& vm, Vec<FileRec>& files, int maxBig = 3000000)
{
    int n = c.range(1, 12);
    for (int i = 0; i < n; ++i) {
        Str rel;
        if (c.coin(30)) rel = Str::fmt("sub%u/", c.rnd(3)) + (c.coin(30) ? Str("deeper/") : Str());
        static const char* names[] = { "disk", "\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba", "config file", "nvram", "\xe6\x97\xa5\xe6\x9c\xac" };
        rel += Str::fmt("%s-%d.%s", names[c.rnd(5)], i, c.coin() ? "vmdk" : "vmx");
        size_t sz = c.coin(15) ? 0 : c.coin(30) ? (size_t)c.range(1, maxBig) : (size_t)c.range(1, 60000);
        FileRec f; f.rel = rel; f.data = Content(c, sz, (int)c.rnd(3));
        WriteFileBytes(JoinPath(JoinPath(root, vm), rel.replace_all("/", "\\")), f.data.data(), f.data.size());
        files.push_back(f);
    }
}

static void AddBig(Ctx& c, const Str& root, const Str& vm, Vec<FileRec>& files, size_t n)
{
    FileRec f; f.rel = "zz-big.vmdk"; f.data = Content(c, n, 1);
    WriteFileBytes(JoinPath(JoinPath(root, vm), f.rel), f.data.data(), f.data.size()); files.push_back(f);
}

static bool SameFile(const Str& path, const Bytes& want)
{
    Bytes got; Str e;
    if (!ReadWholeFile(path.c_str(), got, e, (size_t)1 << 30)) return false;
    return got.size() == want.size() && (want.empty() || memcmp(got.data(), want.data(), want.size()) == 0);
}

static bool SameTree(Ctx& c_, const Str& dest, const Str& vm, const Vec<FileRec>& files)
{
    bool ok = true;
    for (const FileRec& f : files) {
        Str p = JoinPath(JoinPath(dest, vm), f.rel.replace_all("/", "\\"));
        if (!SameFile(p, f.data)) { c_.note("differs: %s", f.rel.c_str()); ok = false; }
    }
    return ok;
}

struct CountProgress : Progress {
    u64 bytes, archiveBytes; bool abortAt; u64 limit; int retries, resumes, volumes; int resumedVolumes; u64 resumedRaw;
    CountProgress() : bytes(0), archiveBytes(0), abortAt(false), limit(0), retries(0), resumes(0), volumes(0), resumedVolumes(0), resumedRaw(0) {}
    void onBytes(u64 n) override { bytes += n; }
    void onArchiveBytes(u64 n) override { archiveBytes += n; }
    void onVolume(int, int) override { ++volumes; }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    void onResume(int v, u64 raw) override { ++resumes; resumedVolumes = v; resumedRaw = raw; }
    bool aborted() override { return abortAt && (bytes >= limit || archiveBytes >= limit); }
};

static Target* LocalTargetAt(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }

// ---- a target that breaks on purpose ---------------------------------------------------------------------------
// Wraps a target; after a random number of bytes written / read the "connection" dies: the current operation fails with a
// transient error, and every call fails the same way until reconnect() is called.
struct FlakyTarget : Target {
    Target* in; Ctx* c; bool dead; int faultsLeft; u64 nextFaultAt, moved; int faults, reconnects; u64 minGap, maxGap; bool onWrite, onRead, onOps; int opCount; int opFaultEvery;
    FlakyTarget(Target* t, Ctx* ctx, int nfaults, u64 minG, u64 maxG) : in(t), c(ctx), dead(false), faultsLeft(nfaults), nextFaultAt(0), moved(0), faults(0), reconnects(0), minGap(minG), maxGap(maxG), onWrite(true), onRead(true), onOps(false), opCount(0), opFaultEvery(0) { schedule(); }
    ~FlakyTarget() { delete in; }
    void schedule() { nextFaultAt = moved + minGap + (maxGap > minGap ? c->next() % (maxGap - minGap) : 0); }
    bool strike() { if (faultsLeft <= 0) return false; if (moved < nextFaultAt) return false; --faultsLeft; ++faults; dead = true; schedule(); return true; }
    bool deadFail() { err = "connection reset by peer (test)"; transient = true; return false; }
    bool opFault() { if (!onOps || !opFaultEvery || faultsLeft <= 0) return false; if (++opCount % opFaultEvery) return false; --faultsLeft; ++faults; dead = true; return true; }
    const char* kind() const override { return "flaky"; }
    bool connect() override { if (dead) return deadFail(); transient = false; if (!in->connect()) { err = in->err; transient = in->transient; return false; } return true; }
    bool reconnect() override { dead = false; ++reconnects; return connect(); }
    Sink* open_write(const Str& name) override;
    Source* open_read(const Str& name) override;
    bool list(Vec<Str>& names) override { if (dead || opFault()) { deadFail(); return false; } if (!in->list(names)) { err = in->err; transient = in->transient; return false; } return true; }
    bool remove(const Str& name) override { if (dead) return deadFail(); if (!in->remove(name)) { err = in->err; transient = in->transient; return false; } return true; }
    bool exists(const Str& name, bool& yes) override { if (dead) return deadFail(); return in->exists(name, yes); }
};

struct FlakySink : Sink {
    FlakyTarget* t; Sink* in;
    FlakySink(FlakyTarget* ft, Sink* s) : t(ft), in(s) {}
    ~FlakySink() { delete in; }
    bool write(const void* p, size_t n) override {
        if (t->dead) { in->abort(); return fail("connection reset by peer (test)", true); }
        t->moved += n;
        if (t->onWrite && t->strike()) { in->abort(); return fail("connection reset by peer (test)", true); }
        if (!in->write(p, n)) return failFrom(in);
        return true;
    }
    bool finish() override { if (t->dead) { in->abort(); return fail("connection reset by peer (test)", true); } if (!in->finish()) return failFrom(in); return true; }
    void abort() override { in->abort(); }
};

struct FlakySource : Source {
    FlakyTarget* t; Source* in;
    FlakySource(FlakyTarget* ft, Source* s) : t(ft), in(s) {}
    ~FlakySource() { in->close(); delete in; }
    i64 read(void* p, size_t n) override {
        if (t->dead) return fail("connection reset by peer (test)", true);
        i64 k = in->read(p, n);
        if (k < 0) return failFrom(in);
        t->moved += (u64)k;
        if (t->onRead && t->strike()) return fail("connection reset by peer (test)", true);
        return k;
    }
    void close() override { in->close(); }
};

Sink* FlakyTarget::open_write(const Str& name) { if (dead) { deadFail(); return nullptr; } Sink* s = in->open_write(name); if (!s) { err = in->err; transient = in->transient; return nullptr; } return new FlakySink(this, s); }
Source* FlakyTarget::open_read(const Str& name) { if (dead) { deadFail(); return nullptr; } Source* s = in->open_read(name); if (!s) { err = in->err; transient = in->transient; return nullptr; } return new FlakySource(this, s); }

static RetryPolicy FastRetry(int attempts = 12) { RetryPolicy r; r.attempts = attempts; r.baseDelayMs = 1; r.maxDelayMs = 4; return r; }

static BackupOptions SmallVolumes(Ctx& c)
{
    BackupOptions o; o.kdfIterations = 3000; o.retry = FastRetry();
    o.codec.level = 1; (void)c;
    o.segmentBytes = (u64)512 * (u64)c.range(64, 1500);                  // 32 KiB .. 750 KiB: many volumes
    return o;
}

static size_t CountNames(Target& t, const char* suffix = nullptr, const char* contains = nullptr)
{
    Vec<Str> names; t.list(names); size_t n = 0;
    for (const Str& s : names) { if (suffix && !s.ends_with(suffix)) continue; if (contains && s.find(contains) < 0) continue; ++n; }
    return n;
}

// ---- tests -----------------------------------------------------------------------------------------------------
VM2B_TEST("engine", backup_restore_xz_levels)
{
    TmpDir tmp(c_, "eng");
    Str root = tmp.sub("vms");
    for (int round = 0; round < 6; ++round) {
        Str vm = Str::fmt("vm%d \xd0\x92\xd0\x9c", round);
        Vec<FileRec> files; MakeVm(c_, root, vm, files);
        BackupOptions o;
        o.codec.level = c_.coin() ? 1 : c_.range(1, 3); o.codec.threads = c_.range(0, 2);
        bool enc = c_.coin(50); if (enc) o.password = Str::fmt("p@ss-%u-\xd0\xbf\xd0\xb0\xd1\x80", c_.rnd(1000));
        o.verify = c_.coin(70); o.keep = 0; o.kdfIterations = 3000;
        if (c_.coin(50)) o.segmentBytes = (u64)512 * (u64)c_.range(100, 4000);
        Target* t = LocalTargetAt(tmp.sub("store")); T_CHECK(t != nullptr); if (!t) return false;
        VmSource* src = NewDirectorySource(root);
        Manifest m; Str warn, err; CountProgress pr;
        bool ok = BackupVm(*src, vm, *t, o, &pr, m, warn, err);
        if (!ok) c_.note("backup: %s", err.c_str());
        T_CHECK(ok);
        if (ok) {
            T_CHECK(m.vm == vm); T_CHECK(m.encrypted == enc); T_CHECK(m.codec == "xz"); T_CHECK(m.level == CodecEffectiveLevel(o.codec)); T_CHECK(!m.segments.empty());
            T_CHECK(m.archive.ends_with(enc ? ".tar.xz.enc" : ".tar.xz"));
            u64 total = 0; for (const FileRec& f : files) total += f.data.size();
            T_CHECK(pr.bytes == total); T_CHECK(m.rawSize >= total);
            u64 st = 0, rw = 0; for (const Segment& s : m.segments) { st += s.size; rw += s.raw; T_CHECK(s.raw <= o.segmentBytes); }
            T_CHECK(st == m.size); T_CHECK(rw == m.rawSize);
            T_CHECK(CountNames(*t, nullptr, (m.archive + ".0").c_str()) == m.segments.size());
            T_CHECK(CountNames(*t, ".resume") == 0);                                   // the journal is gone after success
            Str ve; T_CHECK(VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve));
            if (enc) { Str ve2; T_CHECK(VerifyBackup(*t, m.archive, "", nullptr, nullptr, ve2)); }          // hash-only check without the password
            Str dest = tmp.sub(Str::fmt("restore%d", round));
            Manifest rm; Str re;
            ok = RestoreBackup(*t, m.archive, dest, o.password, nullptr, &rm, re);
            if (!ok) c_.note("restore: %s", re.c_str());
            T_CHECK(ok);
            T_CHECK(SameTree(c_, dest, vm, files));
            T_CHECK(!FileExists(JoinPath(dest, ".vm2b-restore.json")));
            ReadOptions fr; fr.retry = FastRetry(3);
            if (enc) { Str we; T_CHECK(!RestoreBackup(*t, m.archive, tmp.sub("wrongpw"), "wrong", fr, nullptr, nullptr, we)); T_CHECK(we == "wrong password"); }
            if (enc) { Str ne; T_CHECK(!RestoreBackup(*t, m.archive, tmp.sub("nopw"), "", fr, nullptr, nullptr, ne)); }
        }
        delete src; delete t;
    }
    return true;
}

VM2B_TEST("engine", tamper_is_detected)
{
    TmpDir tmp(c_, "tamper");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 400000);
    for (int k = 0; k < 4; ++k) {
        BackupOptions o = SmallVolumes(c_); o.codec.level = 1; o.verify = false;
        if (k % 2) o.password = "secret";
        Str store = tmp.sub(Str::fmt("s%d", k));
        Target* t = LocalTargetAt(store); VmSource* src = NewDirectorySource(root);
        Manifest m; Str warn, err; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, warn, err));
        T_CHECK(m.segments.size() >= 1);
        size_t vi = c_.rnd((u32)m.segments.size());
        Str path = JoinPath(store, m.volumeName(vi)); Str e;
        Bytes b; T_CHECK(ReadWholeFile(path.c_str(), b, e, (size_t)1 << 30)); T_CHECK(!b.empty());
        if (b.empty()) { delete src; delete t; continue; }
        Bytes orig = b;
        b[b.size() / 2] ^= 0x10; WriteWholeFile(path.c_str(), b.data(), b.size(), e);        // a flipped bit in a volume
        ReadOptions fr; fr.retry = FastRetry(4);
        Str ve; T_CHECK(!VerifyBackup(*t, m.archive, o.password, fr, nullptr, nullptr, ve));
        if (b.size() > 8) { b.resize(b.size() - 7); WriteWholeFile(path.c_str(), b.data(), b.size(), e); }   // a truncated volume
        Str ve3; T_CHECK(!VerifyBackup(*t, m.archive, o.password, fr, nullptr, nullptr, ve3));
        Str re; T_CHECK(!RestoreBackup(*t, m.archive, tmp.sub("rest"), o.password, fr, nullptr, nullptr, re));
        WriteWholeFile(path.c_str(), orig.data(), orig.size(), e);
        Str ve4; T_CHECK(VerifyBackup(*t, m.archive, o.password, fr, nullptr, nullptr, ve4));                 // undone: good again
        if (m.segments.size() >= 2) {
            Str other = JoinPath(store, m.volumeName((vi + 1) % m.segments.size())); Bytes ob; ReadWholeFile(other.c_str(), ob, e, (size_t)1 << 30);
            WriteWholeFile(path.c_str(), ob.data(), ob.size(), e);                                         // two volumes swapped / duplicated
            bool same = ob.size() == orig.size() && (ob.empty() || memcmp(ob.data(), orig.data(), ob.size()) == 0);   // two byte-identical volumes: the swap changes nothing
            Str ve5; bool v5 = VerifyBackup(*t, m.archive, o.password, fr, nullptr, nullptr, ve5);
            if (same) c_.note("the two volumes are identical: the swap is not a change"); else T_CHECK(!v5);
            WriteWholeFile(path.c_str(), orig.data(), orig.size(), e);
            T_CHECK(t->remove(m.volumeName(vi)));                                                           // a missing volume
            Str ve6; T_CHECK(!VerifyBackup(*t, m.archive, o.password, fr, nullptr, nullptr, ve6));
        }
        delete src; delete t;
    }
    return true;
}

VM2B_TEST("engine", encrypted_backup_cut_short)
{
    // Cutting the LAST volumes off an encrypted backup (and fixing the manifest to match) is detected through the "last
    // volume" flag that is part of the authenticated data of every block.
    TmpDir tmp(c_, "cut");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 600000); AddBig(c_, root, "vm", files, 300000);
    BackupOptions o = SmallVolumes(c_); o.password = "pw"; o.verify = false; o.segmentBytes = 512 * 64;
    Str store = tmp.sub("s"); Target* t = LocalTargetAt(store); VmSource* src = NewDirectorySource(root);
    Manifest m; Str w, err; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err));
    T_CHECK(m.segments.size() >= 3);
    if (m.segments.size() >= 3) {
        Manifest cut = m; cut.segments.resize(m.segments.size() - 1);
        cut.size = 0; cut.rawSize = 0; for (const Segment& s : cut.segments) { cut.size += s.size; cut.rawSize += s.raw; }
        Bytes all; for (const Segment& s : cut.segments) { Bytes h; HexDecode(s.sha256.data(), 64, h); for (size_t i = 0; i < 32; ++i) all.push_back(h[i]); }
        u8 d[32]; Sha256(all.data(), all.size(), d); cut.sha256 = HexEncode(d, 32);
        Str js = cut.toJson().dump(true); T_CHECK(t->put_bytes(m.archive + ".json", js.data(), js.size()));
        ReadOptions fr; fr.retry = FastRetry(4);
        Str ve; T_CHECK(!VerifyBackup(*t, m.archive, "pw", fr, nullptr, nullptr, ve));
        Str ve2; bool r = VerifyBackup(*t, m.archive, "", fr, nullptr, nullptr, ve2); T_CHECK(r);       // without the password only the hashes can be checked, and they are consistent
    }
    delete src; delete t;
    return true;
}

VM2B_TEST("engine", retention_and_naming)
{
    TmpDir tmp(c_, "keep");
    Str root = tmp.sub("vms"); Vec<FileRec> f1, f2; MakeVm(c_, root, "alpha", f1, 100000); MakeVm(c_, root, "beta", f2, 100000);
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    int keep = c_.range(1, 3);
    for (int i = 0; i < 5; ++i) {
        BackupOptions o = SmallVolumes(c_); o.codec.level = 1; o.keep = keep; o.verify = false;
        Manifest m; Str w, err; T_CHECK(BackupVm(*src, i % 2 ? "beta" : "alpha", *t, o, nullptr, m, w, err));
        T_CHECK(BackupVm(*src, "alpha", *t, o, nullptr, m, w, err));                       // same second: a new name, nothing overwritten
    }
    Vec<Manifest> a, b; Str err; Str alpha = "alpha", beta = "beta";
    T_CHECK(ListBackups(*t, &alpha, a, err)); T_CHECK(a.size() == (size_t)keep);
    T_CHECK(ListBackups(*t, &beta, b, err)); T_CHECK(b.size() <= (size_t)keep && !b.empty());
    for (size_t i = 1; i < a.size(); ++i) T_CHECK(a[i - 1].created <= a[i].created);
    // no orphans: every file in the folder belongs to a kept backup
    Vec<Str> names; t->list(names); Vec<Manifest> all; ListBackups(*t, nullptr, all, err);
    size_t expect = 0; for (const Manifest& m : all) expect += 1 + m.segments.size();
    T_CHECK(names.size() == expect);
    Vec<Str> rem; T_CHECK(PruneBackups(*t, "alpha", 1, rem, err)); ListBackups(*t, &alpha, a, err); T_CHECK(a.size() == 1);
    delete src; delete t;
    return true;
}

VM2B_TEST("engine", abort_leaves_nothing)
{
    TmpDir tmp(c_, "abort");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 200000);
    Bytes big = Content(c_, 8 << 20, 1); WriteFileBytes(JoinPath(JoinPath(root, "vm"), "big.vmdk"), big.data(), big.size());
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    BackupOptions o = SmallVolumes(c_); o.segmentBytes = 512 * 2048; o.codec.level = 1; if (c_.coin()) o.password = "pw";
    CountProgress pr; pr.abortAt = true; pr.limit = 1 + c_.rnd(6 << 20);
    Manifest m; Str w, err;
    T_CHECK(!BackupVm(*src, "vm", *t, o, &pr, m, w, err)); T_CHECK(err == kAbortedMessage);
    Vec<Str> names; t->list(names); T_CHECK(names.empty());                                    // volumes, journal: all gone after a user abort
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(tmp.sub("store"), "*.part")), &fd);
    T_CHECK(h == INVALID_HANDLE_VALUE); if (h != INVALID_HANDLE_VALUE) FindClose(h);
    delete src; delete t;
    return true;
}

VM2B_TEST("engine", running_vm_is_refused)
{
    TmpDir tmp(c_, "live");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 100000);
    WriteFileBytes(JoinPath(JoinPath(root, "vm"), "vm.vmx.lck"), "x", 1);
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    BackupOptions o; o.codec.level = 1; o.retry = FastRetry(); Manifest m; Str w, err;
    T_CHECK(!BackupVm(*src, "vm", *t, o, nullptr, m, w, err)); T_CHECK(err.find("running") >= 0);
    Vec<VmInfo> vms; T_CHECK(src->list(vms)); T_CHECK(vms.size() == 1 && vms[0].state == "running");
    o.allowLive = true; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err)); T_CHECK(!w.empty());
    Str dest = tmp.sub("r"); T_CHECK(RestoreBackup(*t, m.archive, dest, "", nullptr, nullptr, err));
    T_CHECK(!FileExists(JoinPath(JoinPath(dest, "vm"), "vm.vmx.lck")));                    // the lock file is not part of the copy
    delete src; delete t;
    return true;
}

static bool PutCraftedArchive(Target& t, const char* name, const Vec<FileRec>& entries)
{
    MemSink ms; TarWriter w(&ms);
    for (const FileRec& f : entries) w.add_bytes(f.rel, f.data.data(), f.data.size(), 1);
    w.finish();
    MemSink cs; { CodecOptions co; co.level = 1; Sink* x = CodecMakeSink(co, &cs); if (!x || !x->write(ms.data.data(), ms.data.size()) || !x->finish()) { delete x; return false; } delete x; }       // the archive is an xz like every other
    Manifest m; m.vm = "crafted"; m.archive = name; Segment s; s.size = cs.data.size(); s.raw = ms.data.size(); u8 h[32]; Sha256(cs.data.data(), cs.data.size(), h); s.sha256 = HexEncode(h, 32);
    m.segments.push_back(s); m.size = s.size; m.rawSize = s.raw; { Bytes hb; for (int i = 0; i < 32; ++i) hb.push_back(h[i]); u8 root[32]; Sha256(hb.data(), 32, root); m.sha256 = HexEncode(root, 32); }
    if (!t.put_bytes(m.volumeName(0), cs.data.data(), cs.data.size())) return false;
    Str js = m.toJson().dump(); return t.put_bytes(Str(name) + ".json", js.data(), js.size());
}

VM2B_TEST("engine", hostile_archive_paths)
{
    // an archive that tries to write outside the destination must be refused (the extractor is the last line of defence)
    TmpDir tmp(c_, "evil");
    Target* t = LocalTargetAt(tmp.sub("store")); t->connect();
    const char* bad[] = { "../escape.txt", "vm/../../escape.txt", "/abs.txt", "C:/win.txt", "vm\\..\\x.txt" };
    for (const char* p : bad) {
        Vec<FileRec> es; FileRec f; f.rel = p; f.data.resize(4); memcpy(f.data.data(), "evil", 4); es.push_back(f); T_CHECK(PutCraftedArchive(*t, "evil.tar", es));
        Str re; T_CHECK(!RestoreBackup(*t, "evil.tar", tmp.sub("dest"), "", nullptr, nullptr, re));
        T_CHECK(!FileExists(JoinPath(tmp.path, "escape.txt")));
    }
    delete t;
    return true;
}

VM2B_TEST("engine", default_kdf_cost)
{
    // the shipped setting must stay expensive enough (an attacker guesses passwords from a stolen archive)
    TmpDir tmp(c_, "kdf");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 50000);
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    BackupOptions o; o.codec.level = 1; o.password = "secret"; o.verify = true;
    T_CHECK(o.kdfIterations >= 600000);
    Manifest m; Str w, err; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err));
    Bytes b; T_CHECK(t->get_bytes(m.volumeName(0), b)); T_CHECK(b.size() > 36);
    u32 iter = (u32)b[24] << 24 | (u32)b[25] << 16 | (u32)b[26] << 8 | b[27]; T_CHECK(iter == o.kdfIterations);
    delete src; delete t;
    return true;
}

// ---- resuming --------------------------------------------------------------------------------------------------
VM2B_TEST("resume", backup_survives_connection_loss)
{
    for (int round = 0; round < 3; ++round) {
        TmpDir tmp(c_, "flaky");
        Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 800000); AddBig(c_, root, "vm", files, 2500000);
        BackupOptions o = SmallVolumes(c_); if (c_.coin()) o.password = "pw";
        FlakyTarget* ft = new FlakyTarget(LocalTargetAt(tmp.sub("store")), &c_, c_.range(1, 4), 5000, 400000);
        VmSource* src = NewDirectorySource(root);
        Manifest m; Str w, err; CountProgress pr;
        bool ok = BackupVm(*src, "vm", *ft, o, &pr, m, w, err);
        if (!ok) c_.note("backup: %s (faults %d, retries %d, reconnects %d, vol %d)", err.c_str(), ft->faults, pr.retries, ft->reconnects, pr.volumes);
        T_CHECK(ok);
        T_CHECK(ft->faults > 0 || ft->faultsLeft > 0); T_CHECK(ft->reconnects >= ft->faults);
        T_CHECK(pr.retries >= ft->faults);
        T_CHECK(CountNames(*ft->in, ".part") == 0); T_CHECK(CountNames(*ft->in, ".resume") == 0);
        if (ok) {
            Str ve; T_CHECK(VerifyBackup(*ft->in, m.archive, o.password, nullptr, nullptr, ve));
            Str re; T_CHECK(RestoreBackup(*ft->in, m.archive, tmp.sub("rest"), o.password, nullptr, nullptr, re)); T_CHECK(SameTree(c_, tmp.sub("rest"), "vm", files));
        }
        delete src; delete ft;
    }
    return true;
}

VM2B_TEST("resume", backup_resumes_in_a_later_run)
{
    TmpDir tmp(c_, "later");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 800000); AddBig(c_, root, "vm", files, 3 << 20);
    BackupOptions o = SmallVolumes(c_); o.retry = FastRetry(0); o.segmentBytes = 512 * 100;     // 51 KiB volumes; no retries: the first run fails and leaves the journal
    if (c_.coin()) o.password = "pw";
    o.verify = true;
    Str storePath = tmp.sub("store");
    {
        FlakyTarget* ft = new FlakyTarget(LocalTargetAt(storePath), &c_, 1, 150000, 1200000);
        VmSource* src = NewDirectorySource(root); Manifest m; Str w, err;
        T_CHECK(!BackupVm(*src, "vm", *ft, o, nullptr, m, w, err)); T_CHECK(ft->faults == 1);
        Vec<ResumeInfo> rs; Str e2; T_CHECK(ListResumable(*ft->in, nullptr, rs, e2)); T_CHECK(rs.size() == 1);
        if (!rs.empty()) { T_CHECK(rs[0].volumesDone >= 1); T_CHECK(rs[0].vm == "vm"); }
        Vec<Manifest> ms; ListBackups(*ft->in, nullptr, ms, e2); T_CHECK(ms.empty());           // an unfinished backup is not a backup
        delete src; delete ft;
    }
    // a later run, a healthy connection: it continues and reads from the VM only what is missing
    Target* t = LocalTargetAt(storePath); VmSource* src = NewDirectorySource(root);
    Vec<ResumeInfo> rs; Str e2; T_CHECK(ListResumable(*t, nullptr, rs, e2)); int had = rs.empty() ? 0 : rs[0].volumesDone; u64 hadRaw = rs.empty() ? 0 : rs[0].rawBytes;
    Manifest m; Str w, err; CountProgress pr;
    bool ok = BackupVm(*src, "vm", *t, o, &pr, m, w, err);
    if (!ok) c_.note("resume: %s", err.c_str());
    T_CHECK(ok); T_CHECK(pr.resumes == 1); T_CHECK(pr.resumedVolumes == had);
    u64 total = 0; for (const FileRec& f : files) total += f.data.size();
    T_CHECK(pr.bytes < total);                                                                  // not everything was read again
    T_CHECK(pr.bytes + hadRaw >= total);
    T_CHECK(CountNames(*t, ".resume") == 0);
    Vec<Manifest> ms; ListBackups(*t, nullptr, ms, e2); T_CHECK(ms.size() == 1);                // one backup, not two
    if (ok) {
        Str re; T_CHECK(RestoreBackup(*t, m.archive, tmp.sub("rest"), o.password, nullptr, nullptr, re)); T_CHECK(SameTree(c_, tmp.sub("rest"), "vm", files));
    }
    delete src; delete t;
    return true;
}

VM2B_TEST("resume", changed_vm_or_settings_start_over)
{
    TmpDir tmp(c_, "changed");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 1500000);
    Bytes big = Content(c_, 2 << 20, 1); WriteFileBytes(JoinPath(JoinPath(root, "vm"), "zz-big.vmdk"), big.data(), big.size());
    BackupOptions o = SmallVolumes(c_); o.retry = FastRetry(0); o.codec.level = 1; o.segmentBytes = 512 * 128;
    Str storePath = tmp.sub("store");
    for (int variant = 0; variant < 2; ++variant) {
        {   // an interrupted backup
            FlakyTarget* ft = new FlakyTarget(LocalTargetAt(storePath), &c_, 1, 150000, 600000);
            VmSource* src = NewDirectorySource(root); Manifest m; Str w, err;
            T_CHECK(!BackupVm(*src, "vm", *ft, o, nullptr, m, w, err));
            Vec<ResumeInfo> rs; Str e2; T_CHECK(ListResumable(*ft->in, nullptr, rs, e2)); T_CHECK(rs.size() == 1);
            delete src; delete ft;
        }
        BackupOptions o2 = o; o2.retry = FastRetry();
        if (variant == 0) { Bytes more = Content(c_, 5000, 1); WriteFileBytes(JoinPath(JoinPath(root, "vm"), "zz-new-file.vmx"), more.data(), more.size()); }   // the VM changed
        else o2.password = "now-encrypted";                                                                                                                 // other settings
        Target* t = LocalTargetAt(storePath); VmSource* src = NewDirectorySource(root);
        Manifest m; Str w, err; CountProgress pr;
        bool ok = BackupVm(*src, "vm", *t, o2, &pr, m, w, err);
        if (!ok) c_.note("start over: %s", err.c_str());
        T_CHECK(ok); T_CHECK(pr.resumes == 0);                                                 // it did not continue the old one
        T_CHECK(CountNames(*t, ".resume") == 0);
        Vec<Str> names; t->list(names); size_t mine = 0; for (const Str& n : names) if (n.starts_with(m.archive.c_str())) ++mine;
        T_CHECK(mine == 1 + m.segments.size());                                                // the old partial backup's volumes were removed
        T_CHECK(CountNames(*t) == mine);
        Str re; T_CHECK(RestoreBackup(*t, m.archive, tmp.sub(Str::fmt("rest%d", variant)), o2.password, nullptr, nullptr, re));
        delete src; delete t;
        Str d; DeleteBackup(*LocalTargetAt(storePath), m.archive, d);
    }
    return true;
}

VM2B_TEST("resume", verify_and_restore_survive_connection_loss)
{
    TmpDir tmp(c_, "readloss");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 800000); AddBig(c_, root, "vm", files, 2500000);
    BackupOptions o = SmallVolumes(c_); if (c_.coin()) o.password = "pw";
    Target* good = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    Manifest m; Str w, err; T_CHECK(BackupVm(*src, "vm", *good, o, nullptr, m, w, err));
    for (int round = 0; round < 3; ++round) {
        FlakyTarget* ft = new FlakyTarget(LocalTargetAt(tmp.sub("store")), &c_, c_.range(1, 4), 3000, 300000);
        ft->onWrite = false; ft->onOps = true; ft->opFaultEvery = 7;
        ReadOptions ro; ro.retry = FastRetry();
        CountProgress pr;
        Str ve; bool ok = VerifyBackup(*ft, m.archive, o.password, ro, &pr, nullptr, ve);
        if (!ok) c_.note("verify: %s", ve.c_str());
        T_CHECK(ok); T_CHECK(ft->faults > 0);
        ft->faultsLeft = c_.range(1, 4); ft->moved = 0; ft->schedule();
        Str dest = tmp.sub(Str::fmt("rest%d", round)); Str re;
        ok = RestoreBackup(*ft, m.archive, dest, o.password, ro, &pr, nullptr, re);
        if (!ok) c_.note("restore: %s (faults %d, left %d, retries %d, reconnects %d, moved %llu)", re.c_str(), ft->faults, ft->faultsLeft, pr.retries, ft->reconnects, (unsigned long long)ft->moved);
        T_CHECK(ok); T_CHECK(pr.retries > 0);
        T_CHECK(SameTree(c_, dest, "vm", files));
        // the retry budget really is a budget
        FlakyTarget* ft2 = new FlakyTarget(LocalTargetAt(tmp.sub("store")), &c_, 1000, 1000, 2000);
        ReadOptions ro2; ro2.retry = FastRetry(2);
        Str ve2; T_CHECK(!VerifyBackup(*ft2, m.archive, o.password, ro2, nullptr, nullptr, ve2)); T_CHECK(ve2.find("connection") >= 0);
        delete ft; delete ft2;
    }
    delete src; delete good;
    return true;
}

VM2B_TEST("resume", restore_continues_in_a_later_run)
{
    TmpDir tmp(c_, "restore2");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 3000000);
    Bytes big = Content(c_, 4 << 20, 1); WriteFileBytes(JoinPath(JoinPath(root, "vm"), "zz-big.vmdk"), big.data(), big.size()); { FileRec f; f.rel = "zz-big.vmdk"; f.data = big; files.push_back(f); }
    BackupOptions o = SmallVolumes(c_); o.segmentBytes = 512 * 256; if (c_.coin()) o.password = "pw";
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    Manifest m; Str w, err; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err));
    Str dest = tmp.sub("dest");
    // 1st run: stopped after a part of the data (a user abort / a crash leaves the state file)
    CountProgress p1; p1.abortAt = true; p1.limit = 1 + c_.rnd((u32)(m.size / 2 + 1));
    Str e1; T_CHECK(!RestoreBackup(*t, m.archive, dest, o.password, &p1, nullptr, e1)); T_CHECK(e1 == kAbortedMessage);
    bool state = FileExists(JoinPath(dest, ".vm2b-restore.json"));
    // 2nd run: continues
    CountProgress p2; Str e2;
    bool ok = RestoreBackup(*t, m.archive, dest, o.password, &p2, nullptr, e2);
    if (!ok) c_.note("restore: %s", e2.c_str());
    T_CHECK(ok); T_CHECK(SameTree(c_, dest, "vm", files));
    T_CHECK(!FileExists(JoinPath(dest, ".vm2b-restore.json")));
    if (state) T_CHECK(p2.resumes == 1);
    // a state file of ANOTHER backup is ignored
    Bytes junk = Content(c_, 100, 1); WriteFileBytes(JoinPath(dest, ".vm2b-restore.json"), junk.data(), junk.size());
    Str e3; T_CHECK(RestoreBackup(*t, m.archive, tmp.sub("dest2"), o.password, nullptr, nullptr, e3));
    Str e4; T_CHECK(RestoreBackup(*t, m.archive, dest, o.password, nullptr, nullptr, e4)); T_CHECK(SameTree(c_, dest, "vm", files));
    delete src; delete t;
    return true;
}

VM2B_TEST("engine", bandwidth_limit)
{
    TmpDir tmp(c_, "rate");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 10);
    Bytes big = Content(c_, 1 << 20, 1); WriteFileBytes(JoinPath(JoinPath(root, "vm"), "zz-big.vmdk"), big.data(), big.size());
    BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.verify = false; o.retry = FastRetry();
    RateLimiter rl; rl.setBytesPerSecond(1000000); o.limiter = &rl;                       // 1 MB/s
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    DWORD t0 = GetTickCount(); Manifest m; Str w, err;
    T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err));
    DWORD dt = GetTickCount() - t0;
    T_CHECK(dt >= 600);                                                                   // 1 MiB at 1 MB/s: about a second, less the 0.25 s burst the limiter allows
    T_CHECK(dt < 15000);
    rl.setBytesPerSecond(0);                                                              // unlimited again, changed while it is in use
    DWORD t1 = GetTickCount(); Manifest m2; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m2, w, err)); T_CHECK(GetTickCount() - t1 < dt);
    // reading is limited too
    rl.setBytesPerSecond(2000000); ReadOptions ro; ro.limiter = &rl; ro.retry = FastRetry();
    DWORD t2 = GetTickCount(); Str ve; T_CHECK(VerifyBackup(*t, m.archive, "", ro, nullptr, nullptr, ve)); T_CHECK(GetTickCount() - t2 >= 200);
    // an abort interrupts the waiting
    rl.setBytesPerSecond(10000); CountProgress pr; pr.abortAt = true; pr.limit = 1; pr.archiveBytes = 5;
    DWORD t3 = GetTickCount(); Str ve2; T_CHECK(!VerifyBackup(*t, m.archive, "", ro, &pr, nullptr, ve2)); T_CHECK(GetTickCount() - t3 < 5000);
    delete src; delete t;
    return true;
}

// ---- bug hunt additions: xz + encryption + tiny volumes, state-file corruption, destinations, many files, throttle ---------------
VM2B_TEST("resume", xz_encrypted_tiny_volumes_survive_connection_loss)
{
    for (int round = 0; round < 3; ++round) {
        TmpDir tmp(c_, "xzflaky");
        Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 400000); AddBig(c_, root, "vm", files, 700000);
        BackupOptions o; o.kdfIterations = 3000; o.retry = FastRetry(); o.codec.level = c_.range(1, 4); o.codec.threads = c_.range(0, 3);
        o.segmentBytes = (u64)512 * (u64)c_.range(8, 120);                       // 4 .. 60 KiB: hundreds of volumes, each its own xz stream
        if (c_.coin(70)) o.password = "pw-\xd0\xbf";
        o.verify = c_.coin();
        bool later = c_.coin();                                                  // lose the connection for good, finish in a second run
        if (later) o.retry = FastRetry(0);
        Str store = tmp.sub("store");
        Manifest m; Str w, err; CountProgress pr; bool ok;
        if (later) {
            FlakyTarget* ft = new FlakyTarget(LocalTargetAt(store), &c_, 1, 20000, 200000);
            VmSource* src = NewDirectorySource(root); T_CHECK(!BackupVm(*src, "vm", *ft, o, nullptr, m, w, err)); T_CHECK(ft->faults == 1); delete src; delete ft;
            o.retry = FastRetry();
        }
        FlakyTarget* ft = new FlakyTarget(LocalTargetAt(store), &c_, later ? 0 : c_.range(1, 4), 5000, 150000);
        VmSource* src = NewDirectorySource(root);
        ok = BackupVm(*src, "vm", *ft, o, &pr, m, w, err);
        if (!ok) c_.note("backup: %s (faults %d, retries %d, vol %d)", err.c_str(), ft->faults, pr.retries, pr.volumes);
        T_CHECK(ok); T_CHECK(CountNames(*ft->in, ".part") == 0); T_CHECK(CountNames(*ft->in, ".resume") == 0);
        if (later) T_CHECK(pr.resumes == 1);
        if (ok) {
            T_CHECK(m.codec == "xz"); T_CHECK(m.segments.size() > 3);
            Str ve; T_CHECK(VerifyBackup(*ft->in, m.archive, o.password, nullptr, nullptr, ve));
            ft->faultsLeft = c_.range(1, 3); ft->moved = 0; ft->onWrite = false; ft->schedule();
            ReadOptions ro; ro.retry = FastRetry();
            Str re; bool r = RestoreBackup(*ft, m.archive, tmp.sub("rest"), o.password, ro, nullptr, nullptr, re);
            if (!r) c_.note("restore: %s", re.c_str());
            T_CHECK(r); T_CHECK(SameTree(c_, tmp.sub("rest"), "vm", files));
            // a restore stopped half way (xz volumes, maybe encrypted) continues in the next run
            Str dest2 = tmp.sub("rest2");
            CountProgress p1; p1.abortAt = true; p1.limit = 1 + c_.rnd((u32)(m.size / 2 + 1)); Str e1;
            T_CHECK(!RestoreBackup(*ft->in, m.archive, dest2, o.password, &p1, nullptr, e1));
            CountProgress p2; Str e2; bool r2 = RestoreBackup(*ft->in, m.archive, dest2, o.password, &p2, nullptr, e2);
            if (!r2) c_.note("continued restore: %s", e2.c_str());
            T_CHECK(r2); T_CHECK(SameTree(c_, dest2, "vm", files));
        }
        delete src; delete ft;
    }
    return true;
}

VM2B_TEST("engine", put_and_get_bytes_keep_the_transient_flag)
{
    // regression: Target::put_bytes / get_bytes dropped `transient`, so a connection lost while the journal / manifest was written was
    // treated as a permanent failure and the backup was not retried
    TmpDir tmp(c_, "putget");
    FlakyTarget ft(LocalTargetAt(tmp.sub("store")), &c_, 1, 1, 2);
    T_CHECK(ft.connect());
    Str data = "some journal text"; bool ok = ft.put_bytes("x.resume", data.data(), data.size());
    T_CHECK(!ok); T_CHECK(ft.transient);
    T_CHECK(ft.reconnect()); ft.faultsLeft = 0; T_CHECK(ft.put_bytes("x.resume", data.data(), data.size()));
    ft.faultsLeft = 1; ft.moved = 0; ft.minGap = ft.maxGap = 1; ft.schedule(); ft.reconnect();
    Bytes got; T_CHECK(!ft.get_bytes("x.resume", got)); T_CHECK(ft.transient);
    return true;
}

// a one-volume archive of the given (path, bytes) entries, stored as <name>.json + <name>.00000 in the target
VM2B_TEST("engine", restore_destination_path_edge_cases)
{
    TmpDir tmp(c_, "paths");
    Target* t = LocalTargetAt(tmp.sub("store")); t->connect();
    // names Windows would turn into devices (the data would vanish silently) or into other files: refused, nothing is written
    const char* bad[] = { "vm/CON", "vm/nul", "vm/NUL.txt", "vm/Aux.vmdk", "vm/prn", "vm/COM1", "vm/com9.log", "vm/lpt3", "vm/sub/con.d/x.vmx", "vm/trail.", "vm/trail ", "vm/a:b", "vm/q?x", "vm/dir/../../x" };
    int k = 0;
    for (const char* p : bad) {
        Vec<FileRec> es; FileRec f; f.rel = p; f.data = Content(c_, 100, 1); es.push_back(f);
        Str name = Str::fmt("bad%d.tar", k++); T_CHECK(PutCraftedArchive(*t, name.c_str(), es));
        Str dest = tmp.sub(Str::fmt("dbad%d", k).c_str()); Str re;
        bool ok = RestoreBackup(*t, name, dest, "", nullptr, nullptr, re);
        if (ok) c_.note("accepted %s", p);
        T_CHECK(!ok);
    }
    // names that merely look similar are fine; so are unicode names and very long paths
    Vec<FileRec> good; Str longDir;
    for (int i = 0; i < 6; ++i) { longDir += Str::fmt("d%d-", i); for (int q = c_.range(30, 50); q > 0; --q) longDir += 'l'; longDir += "/"; }
    const char* names[] = { "vm/CONFIG.vmx", "vm/console.log", "vm/com.vmx", "vm/COM10", "vm/null.txt", "vm/auxiliary", "vm/lpt", "vm/\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba \xd1\x84.vmdk", "vm/\xe6\x97\xa5\xe6\x9c\xac/\xf0\x9f\x92\xbe.bin", "vm/.hidden", "vm/a b.vmx" };
    for (const char* p : names) { FileRec f; f.rel = p; f.data = Content(c_, (size_t)c_.range(0, 3000), 1); good.push_back(f); }
    { FileRec f; f.rel = "vm/" + longDir + "deep-file.vmdk"; f.data = Content(c_, 5000, 1); good.push_back(f); c_.note("long path of %d chars", (int)f.rel.size()); }
    T_CHECK(PutCraftedArchive(*t, "good.tar", good));
    Str dest = tmp.sub("dgood"); Str re; bool ok = RestoreBackup(*t, "good.tar", dest, "", nullptr, nullptr, re);
    if (!ok) c_.note("restore: %s", re.c_str());
    T_CHECK(ok);
    for (const FileRec& f : good) { Str p = JoinPath(dest, f.rel.replace_all("/", "\\")); if (!SameFile(p, f.data)) { c_.note("differs: %s", f.rel.c_str()); T_CHECK(false); } }
    delete t;
    return true;
}

namespace {
// a VM made of N small files, produced on the fly (no disk needed for the source)
struct SynthSource : VmSource {
    u32 n; explicit SynthSource(u32 count) : n(count) {}
    static void Body(u32 i, Bytes& b) { size_t len = (i * 2654435761u >> 7) % 700; b.resize(len); u32 x = i * 747796405u + 1; for (size_t k = 0; k < len; ++k) { x = x * 1664525u + 1013904223u; b[k] = (u8)(x >> 24); } }
    const char* kind() const override { return "synth"; }
    bool list(Vec<VmInfo>& vms) override { VmInfo v; v.name = "synth"; vms.push_back(v); return true; }
    bool begin(const Str&, bool, const Str&, Str& fp, Str& token, Str&) override { fp = "synth"; token.clear(); return true; }
    bool pack(TarWriter& tar, Progress* pr, Str&) override {
        Bytes b;
        for (u32 i = 0; i < n; ++i) {
            if (pr && pr->aborted()) return fail(kAbortedMessage);
            Body(i, b);
            if (!tar.add_bytes(Str::fmt("synth/d%u/f%u.vmx", i / 500, i), b.data(), b.size(), 1000 + i)) return fail(tar.error());
        }
        return true;
    }
};
struct CheckWriter : RestoreWriter {
    u32 files, bad, dirs; u32 n;
    CheckWriter(u32 count) : files(0), bad(0), dirs(0), n(count) {}
    const char* kind() const override { return "check"; }
    bool canResumeInsideFile() const override { return false; }
    Str stateFile(const ReadOptions&, const Str&, const Str&) override { return Str(); }
    bool prepare(const Manifest&, bool) override { return true; }
    bool makeDir(const Str&) override { ++dirs; return true; }
    bool writeFile(const RestoreFile& f, Source& body, RestoreTick*) override {
        int at = f.rel.rfind('f'); u32 i = (u32)strtoul(f.rel.c_str() + at + 1, nullptr, 10);
        Bytes want, got; SynthSource::Body(i, want); u8 buf[1024]; i64 k;
        while ((k = body.read(buf, sizeof(buf))) > 0) { size_t o = got.size(); got.resize(o + (size_t)k); memcpy(got.data() + o, buf, (size_t)k); }
        if (k < 0) return fail(body.err, body.transient);
        if (got.size() != want.size() || (want.size() && memcmp(got.data(), want.data(), want.size())) || f.mtime != (i64)(1000 + i)) ++bad;
        ++files; return true;
    }
};
}

VM2B_TEST("engine", many_small_files)
{
    // 20000 files by default (VM2B_MANY_FILES=100000 for the big one): the tar, the volumes, the restore index must stay linear and exact
    u32 n = 20000; if (const char* e = getenv("VM2B_MANY_FILES")) n = (u32)strtoul(e, nullptr, 10);
    TmpDir tmp(c_, "many");
    BackupOptions o; o.kdfIterations = 3000; o.retry = FastRetry(); o.codec.level = 1; o.segmentBytes = (u64)512 * (u64)c_.range(2000, 20000);
    if (c_.coin()) o.password = "pw"; o.verify = true;
    Target* t = LocalTargetAt(tmp.sub("store")); SynthSource src(n);
    DWORD t0 = GetTickCount(); Manifest m; Str w, err;
    bool ok = BackupVm(src, "synth", *t, o, nullptr, m, w, err); if (!ok) c_.note("backup: %s", err.c_str()); T_CHECK(ok);
    CheckWriter cw(n); ReadOptions ro; ro.retry = FastRetry(); Str re;
    ok = RestoreBackup(*t, m.archive, cw, o.password, ro, nullptr, nullptr, re); if (!ok) c_.note("restore: %s", re.c_str()); T_CHECK(ok);
    c_.note("%u files, %u volumes, %lu ms", n, (unsigned)m.segments.size(), (unsigned long)(GetTickCount() - t0));
    T_CHECK(cw.files == n); T_CHECK(cw.bad == 0);
    delete t;
    return true;
}

VM2B_TEST("resume", restore_state_file_corruption)
{
    TmpDir tmp(c_, "badstate");
    Str root = tmp.sub("vms"); Vec<FileRec> files; MakeVm(c_, root, "vm", files, 600000); AddBig(c_, root, "vm", files, 1500000);
    BackupOptions o = SmallVolumes(c_); o.segmentBytes = 512 * 64; if (c_.coin()) o.password = "pw";
    Target* t = LocalTargetAt(tmp.sub("store")); VmSource* src = NewDirectorySource(root);
    Manifest m; Str w, err; T_CHECK(BackupVm(*src, "vm", *t, o, nullptr, m, w, err));
    for (int variant = 0; variant < 16; ++variant) {
        Str dest = tmp.sub(Str::fmt("dest%d", variant));
        CountProgress p1; p1.abortAt = true; p1.limit = 1 + (u64)c_.range(100000, 1500000);
        Str e1; T_CHECK(!RestoreBackup(*t, m.archive, dest, o.password, &p1, nullptr, e1));
        Str sp = JoinPath(dest, ".vm2b-restore.json"); Bytes sb; Str se;
        if (!ReadWholeFile(sp.c_str(), sb, se)) { c_.note("variant %d: no state file", variant); continue; }
        Json j; bool parsed = Json::parse((const char*)sb.data(), sb.size(), j, se); T_CHECK(parsed); if (!parsed) continue;
        bool mustStartOver = true;                    // every damage: the state carries a checksum, so a clean restart is the only right answer
        Str out;
        switch (variant) {
        case 0: { Bytes g = Content(c_, (size_t)c_.range(1, 4000), 1); WriteFileBytes(sp, g.data(), g.size()); break; }
        case 1: { size_t cut = (size_t)c_.range(0, (int)sb.size() - 1); WriteFileBytes(sp, sb.data(), cut); break; }
        case 2: WriteFileBytes(sp, "", 0); break;
        case 3: j["archive"] = "other.tar"; out = j.dump(); break;
        case 4: j["root"] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; out = j.dump(); break;
        case 5: j["segment"] = (u64)1000000000; out = j.dump(); break;
        case 6: out = "[1,2,3]"; break;
        case 7: out = "null"; break;
        case 8: { Bytes z = sb; for (size_t i = 0; i < z.size(); i += 1 + (size_t)c_.rnd(7)) z[i] = (u8)c_.rnd(256); WriteFileBytes(sp, z.data(), z.size()); break; }
        default: break;
        }
        if (!out.empty()) WriteFileBytes(sp, out.data(), out.size());
        if (variant >= 9) {                           // the state parses and names the right backup, but its position data is nonsense
            Json tj = j.get("tar");
            switch (variant) {
            case 9:  j["skip"] = (u64)1 << 62; break;
            case 10: j["skip"] = (u64)c_.range(1, 100000); break;
            case 11: tj["left"] = (u64)1 << 62; j["tar"] = tj; break;
            case 12: tj["in_entry"] = true; { Json cur = Json::object(); cur["path"] = "../escape.txt"; cur["size"] = 10; cur["mtime"] = 1; cur["mode"] = 420; cur["dir"] = false; cur["skipped"] = false; tj["cur"] = cur; } tj["left"] = 5; j["tar"] = tj; break;
            case 13: tj["pad"] = 600; j["tar"] = tj; break;
            case 14: j["segment"] = (u64)0; j["skip"] = (u64)c_.range(1, 30000); break;
            default: j["tar"] = Json(); break;
            }
            Str js = j.dump(); WriteFileBytes(sp, js.data(), js.size());
        }
        CountProgress p2; Str e2;
        bool ok = RestoreBackup(*t, m.archive, dest, o.password, &p2, nullptr, e2);
        if (mustStartOver) {
            if (!ok) c_.note("variant %d: %s", variant, e2.c_str());
            T_CHECK(ok); T_CHECK(p2.resumes == 0);
        }
        if (ok) { if (!SameTree(c_, dest, "vm", files)) { c_.note("variant %d restored WRONG data", variant); T_CHECK(false); } }
        else {                                        // a refusal is acceptable for position nonsense, but it must be an error, and a second run must succeed
            T_CHECK(!e2.empty());
            CountProgress p3; Str e3; DeleteFileW(FilePath(sp));
            T_CHECK(RestoreBackup(*t, m.archive, dest, o.password, &p3, nullptr, e3)); T_CHECK(SameTree(c_, dest, "vm", files));
        }
        if (!ok && !mustStartOver) c_.note("variant %d refused: %s", variant, e2.c_str());
    }
    delete src; delete t;
    return true;
}

VM2B_TEST("engine", throttle_accuracy_and_lifting)
{
    // small rate: 100 000 B/s, 60 000 bytes in 1 000-byte chunks -> 0.6 s less the 0.25 s burst
    {
        RateLimiter rl; rl.setBytesPerSecond(100000);
        double t0 = (double)GetTickCount64(); for (int i = 0; i < 60; ++i) T_CHECK(rl.acquire(1000, nullptr, nullptr));
        double dt = ((double)GetTickCount64() - t0) / 1000.0;
        if (dt < 0.25 || dt > 1.5) c_.note("small rate: %.3f s", dt);
        T_CHECK(dt >= 0.25); T_CHECK(dt < 1.5);
    }
    // large rate: 200 MB/s, 100 MB in 64 KiB chunks (1600 tiny waits): about 0.5 s - 0.25 s; the schedule is absolute, so Sleep's
    // granularity must not pile up
    {
        RateLimiter rl; rl.setBytesPerSecond(200u * 1000u * 1000u);
        double t0 = (double)GetTickCount64(); for (int i = 0; i < 1600; ++i) T_CHECK(rl.acquire(65536, nullptr, nullptr));
        double dt = ((double)GetTickCount64() - t0) / 1000.0;
        if (dt < 0.2 || dt > 1.5) c_.note("large rate: %.3f s", dt);
        T_CHECK(dt >= 0.2); T_CHECK(dt < 1.5);
    }
    // a limit that is lifted while a transfer waits on its slot (a schedule window ends) must release it
    {
        RateLimiter rl; rl.setBytesPerSecond(1000);
        T_CHECK(rl.acquire(200000, nullptr, nullptr));                            // reserves ~200 s ahead
        struct L { RateLimiter* r; static DWORD WINAPI Run(void* p) { Sleep(200); ((RateLimiter*)p)->setBytesPerSecond(0); return 0; } };
        HANDLE th = CreateThread(nullptr, 0, L::Run, &rl, 0, nullptr);
        double t0 = (double)GetTickCount64(); T_CHECK(rl.acquire(10, nullptr, nullptr));
        double dt = ((double)GetTickCount64() - t0) / 1000.0;
        WaitForSingleObject(th, 5000); CloseHandle(th);
        if (dt > 5) c_.note("waited %.1f s after the limit was lifted", dt);
        T_CHECK(dt < 5);
    }
    return true;
}

} // namespace vm2b
