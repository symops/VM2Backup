// Autotests of hostile / damaged input: manifests, journals, encrypted volumes, tar / xz edge cases, resume with damaged
// volumes.  Rule under test: bad data from a file or the network gives an error, never a crash, hang, out-of-memory or a
// backup that looks complete but is not.
#include "autotest.h"
#include "engine.h"
#include "winutil.h"
#include "config.h"

namespace vm2b {

namespace {

struct HTmp {
    Str path; bool keep;
    HTmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-h%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path); Str e; MakeDirs(path, e);
    }
    ~HTmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
    Str sub(const Str& n) const { return JoinPath(path, n); }
};

Bytes Prefix(const Bytes& b, size_t n) { Bytes r; for (size_t i = 0; i < n && i < b.size(); ++i) r.push_back(b[i]); return r; }

bool PutFile(const Str& path, const void* p, size_t n)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    HANDLE h = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = n == 0 || (WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n);
    CloseHandle(h); return ok;
}

Target* LocalAt(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }

RetryPolicy Fast(int attempts = 6) { RetryPolicy r; r.attempts = attempts; r.baseDelayMs = 1; r.maxDelayMs = 4; return r; }

// Peak working set of this process in MiB (0 when unavailable).
double PeakMiB()
{
    struct PMC { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, a, b, c, d, e, PagefileUsage, PeakPagefileUsage; } pmc = {};
    typedef BOOL (WINAPI *Fn)(HANDLE, PMC*, DWORD);
    Fn f = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    if (!f) return 0;
    pmc.cb = sizeof(pmc);
    if (!f(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (double)pmc.PeakPagefileUsage / (1 << 20);
}

Bytes Noise(Ctx& c, size_t n, int kind)
{
    Bytes b; b.resize(n);
    if (kind == 1) c.fill(b.data(), n);
    else if (kind == 2) for (size_t i = 0; i < n; ++i) b[i] = (u8)("disk flat vmdk\n"[(i / 3 + c.rnd(2)) % 15]);
    return b;
}

// a VM folder with a few files; returns total bytes
u64 MakeSimpleVm(Ctx& c, const Str& root, const Str& vm, size_t big)
{
    u64 tot = 0; int n = c.range(1, 5);
    for (int i = 0; i < n; ++i) {
        Bytes d = Noise(c, (size_t)c.range(0, 50000), (int)c.rnd(3));
        PutFile(JoinPath(JoinPath(root, vm), Str::fmt("f%d.vmdk", i)), d.data(), d.size()); tot += d.size();
    }
    if (big) { Bytes d = Noise(c, big, 1); PutFile(JoinPath(JoinPath(root, vm), "zz-big.vmdk"), d.data(), d.size()); tot += d.size(); }
    return tot;
}

struct Pr : Progress { };

BackupOptions Opts(Ctx& c, bool enc)
{
    BackupOptions o; o.kdfIterations = 3000; o.retry = Fast(); o.codec.level = 1;
    o.segmentBytes = (u64)512 * (u64)c.range(16, 200); o.verify = true;
    if (enc) o.password = "hostile-pw";
    return o;
}

bool Backup(Target& t, const Str& root, const BackupOptions& o, Manifest& m, Str& err, Progress* pr = nullptr)
{
    VmSource* s = NewDirectorySource(root); Str w;
    bool ok = BackupVm(*s, "vm", t, o, pr, m, w, err);
    delete s; return ok;
}

// The result of every operation on a damaged archive must be "returns" (any answer); a good answer must really be good.
void Poke(Target& t, const Str& archive, const Str& pw, const Str& scratch, bool* verifyOk = nullptr, bool* restoreOk = nullptr)
{
    ReadOptions ro; ro.retry = Fast(2); ro.resume = false;
    Str e; Manifest m;
    bool v = VerifyBackup(t, archive, pw, ro, nullptr, &m, e);
    if (verifyOk) *verifyOk = v;
    Str dest = JoinPath(scratch, "r"); RemoveTree(dest);
    bool r = RestoreBackup(t, archive, dest, pw, ro, nullptr, nullptr, e);
    if (restoreOk) *restoreOk = r;
    RemoveTree(dest);
}

// ---- JSON mutation helpers ----------------------------------------------------------------------------------------------
Json Hostile(Ctx& c, const Json& orig)
{
    switch (c.rnd(14)) {
    case 0: return Json();
    case 1: return Json((i64)-1);
    case 2: return Json((i64)0);
    case 3: return Json((i64)0x7fffffffffffffffll);
    case 4: return Json(1e30);
    case 5: return Json(Str());
    case 6: { Str big; for (int i = 0; i < 100000; ++i) big += 'A'; return Json(big); }
    case 7: return Json::array();
    case 8: return Json::object();
    case 9: return Json(true);
    case 10: return Json((i64)4294967296ll);
    case 11: return Json(Str("../../x"));
    case 12: return Json((i64)-9223372036854775807ll);
    default: return orig;
    }
}

// Mutates one random scalar / member somewhere in the tree.
void MutateOne(Ctx& c, Json& j)
{
    if (j.is_object() && j.size()) {
        size_t i = c.rnd((u32)j.size()); Str k = j.key_at(i);
        Json& child = j[k.c_str()];
        if ((child.is_object() || child.is_array()) && child.size() && c.coin(70)) MutateOne(c, child);
        else if (c.coin(10)) { Json n = Json::object(); for (size_t q = 0; q < j.size(); ++q) if (q != i) n[j.key_at(q).c_str()] = j.value_at(q); j = n; }   // a missing field
        else child = Hostile(c, child);
    } else if (j.is_array() && j.size()) {
        size_t i = c.rnd((u32)j.size());
        Json a = Json::array();
        for (size_t q = 0; q < j.size(); ++q) {
            if (q == i) {
                Json x = j.at(q);
                if ((x.is_object() || x.is_array()) && x.size() && c.coin(70)) { MutateOne(c, x); a.push(x); }
                else if (c.coin(15)) {}                                                       // dropped
                else if (c.coin(30)) { a.push(x); a.push(x); }                                // duplicated
                else a.push(Hostile(c, x));
            } else a.push(j.at(q));
        }
        j = a;
    }
}

} // namespace

// Every field of a manifest mutated: sizes, counts, negative / huge values, wrong types, duplicated or dropped volumes.
VM2B_TEST("engine", hostile_manifest_fuzz)
{
    HTmp tmp(c_, "mf");
    Str root = tmp.sub("vms"); MakeSimpleVm(c_, root, "vm", 60000);
    double before = PeakMiB();
    for (int variant = 0; variant < 3; ++variant) {
        bool enc = variant == 1; BackupOptions o = Opts(c_, enc); 
        Str store = tmp.sub(Str::fmt("s%d", variant)); Target* t = LocalAt(store);
        Manifest m; Str err; bool ok = Backup(*t, root, o, m, err); T_CHECK(ok); if (!ok) { c_.note("%s", err.c_str()); delete t; continue; }
        Str mpath = JoinPath(store, m.archive + ".json"); Bytes orig; Str e; T_CHECK(ReadWholeFile(mpath.c_str(), orig, e));
        Json base; T_CHECK(Json::parse((const char*)orig.data(), orig.size(), base, e));
        int accepted = 0;
        for (int t2 = 0; t2 < 60; ++t2) {
            Json j = base; int k = c_.range(1, 3); for (int q = 0; q < k; ++q) MutateOne(c_, j);
            Str js = j.dump(c_.coin());
            if (c_.coin(15) && !js.empty()) js.resize(c_.rnd((u32)js.size()));                // plus byte-level damage
            T_CHECK(PutFile(mpath, js.data(), js.size()));
            Vec<Manifest> ms; Str le; ListBackups(*t, nullptr, ms, le);
            bool vOk = false, rOk = false;
            Poke(*t, m.archive, o.password, tmp.sub("scratch"), &vOk, &rOk);
            if (vOk) ++accepted;
            // a restore that says OK on a mutated manifest must have produced exactly the original files
            if (rOk) { T_CHECK(vOk); }
        }
        // the original comes back: everything good again
        T_CHECK(PutFile(mpath, orig.data(), orig.size()));
        bool vOk = false, rOk = false; Poke(*t, m.archive, o.password, tmp.sub("scratch"), &vOk, &rOk); T_CHECK(vOk && rOk);
        delete t;
    }
    double after = PeakMiB();
    if (after > 0) { T_CHECK(after - before < 1500); if (after - before > 300) c_.note("peak memory grew by %.0f MiB", after - before); }
    return true;
}

// A journal (<archive>.resume) that is damaged in any way never wedges the next backup: it succeeds, or fails once and then the
// following run succeeds; a finished backup always verifies.
VM2B_TEST("resume", hostile_journal_fuzz)
{
    HTmp tmp(c_, "jr");
    Str root = tmp.sub("vms"); MakeSimpleVm(c_, root, "vm", 400000);
    for (int round = 0; round < 6; ++round) {
        BackupOptions o = Opts(c_, c_.coin(40)); o.segmentBytes = 512 * 64;
        Str store = tmp.sub(Str::fmt("s%d", round)); Target* t = LocalAt(store);
        // an interrupted run: abort after some bytes, keep the volumes
        struct PauseAt : Progress { u64 n, lim; PauseAt(u64 l) : n(0), lim(l) {} void onBytes(u64 b) override { n += b; } bool aborted() override { return n >= lim; } bool keepOnAbort() override { return true; } };
        PauseAt pa(150000 + c_.rnd(200000)); Manifest m; Str err;
        T_CHECK(!Backup(*t, root, o, m, err, &pa));
        Vec<Str> names; t->list(names); Str jname;
        for (const Str& n : names) if (n.ends_with(".resume")) jname = n;
        T_CHECK(!jname.empty()); if (jname.empty()) { delete t; continue; }
        Str jpath = JoinPath(store, jname); Bytes orig; Str e; T_CHECK(ReadWholeFile(jpath.c_str(), orig, e));
        Json base; T_CHECK(Json::parse((const char*)orig.data(), orig.size(), base, e));
        Json j = base; int k = c_.range(1, 2); for (int q = 0; q < k; ++q) MutateOne(c_, j);
        Str js = j.dump(); if (c_.coin(20) && !js.empty()) js.resize(c_.rnd((u32)js.size()));
        if (c_.coin(10)) js = Str((const char*)orig.data(), orig.size()).replace_all("segments", "segmentz");
        T_CHECK(PutFile(jpath, js.data(), js.size()));
        Manifest m2; Str err2; bool ok = Backup(*t, root, o, m2, err2);
        if (!ok) { c_.note("run 1 failed (allowed): %s", err2.c_str()); Manifest m3; Str err3; ok = Backup(*t, root, o, m3, err3); if (!ok) c_.note("run 2: %s", err3.c_str()); T_CHECK(ok); m2 = m3; }
        if (ok) { bool vOk = false, rOk = false; Poke(*t, m2.archive, o.password, tmp.sub("scratch"), &vOk, &rOk); T_CHECK(vOk && rOk); }
        delete t;
    }
    return true;
}

// A journal that lists volumes which are no longer there (a user cleaned the folder) must not produce a "complete" backup.
VM2B_TEST("resume", resume_with_lost_or_damaged_volumes)
{
    HTmp tmp(c_, "lv");
    Str root = tmp.sub("vms"); MakeSimpleVm(c_, root, "vm", 500000);
    for (int round = 0; round < 6; ++round) {
        BackupOptions o = Opts(c_, c_.coin(40)); o.segmentBytes = 512 * 64;
        int how = round % 3;                                      // 0 = a volume deleted, 1 = truncated, 2 = a bit flipped
        o.verify = how != 0 ? true : c_.coin();                   // a lost file is detectable without verify; damage needs it
        Str store = tmp.sub(Str::fmt("s%d", round)); Target* t = LocalAt(store);
        struct PauseAt : Progress { u64 n, lim; PauseAt(u64 l) : n(0), lim(l) {} void onBytes(u64 b) override { n += b; } bool aborted() override { return n >= lim; } bool keepOnAbort() override { return true; } };
        PauseAt pa(200000 + c_.rnd(250000)); Manifest m; Str err;
        T_CHECK(!Backup(*t, root, o, m, err, &pa));
        Vec<ResumeInfo> rs; Str e; T_CHECK(ListResumable(*t, nullptr, rs, e)); T_CHECK(rs.size() == 1);
        if (rs.empty() || rs[0].volumesDone < 1) { delete t; continue; }
        Str vol = rs[0].archive + Str::fmt(".%05d", 1 + (int)c_.rnd((u32)rs[0].volumesDone));
        Str vpath = JoinPath(store, vol); Bytes b; T_CHECK(ReadWholeFile(vpath.c_str(), b, e)); T_CHECK(!b.empty()); if (b.empty()) { delete t; continue; }
        if (how == 0) T_CHECK(t->remove(vol));
        else if (how == 1) { b.resize(b.size() / 2); PutFile(vpath, b.data(), b.size()); }
        else { b[b.size() / 2] ^= 1; PutFile(vpath, b.data(), b.size()); }
        Manifest m2; Str err2; bool ok = Backup(*t, root, o, m2, err2);
        if (ok) { bool vOk = false, rOk = false; Poke(*t, m2.archive, o.password, tmp.sub("scratch"), &vOk, &rOk); if (!(vOk && rOk)) c_.note("round %d how %d: backup reported OK but the result does not verify (verify=%d)", round, how, (int)o.verify); T_CHECK(vOk && rOk); }
        else { c_.note("failed (allowed): %s", err2.c_str()); Manifest m3; Str err3; T_CHECK(Backup(*t, root, o, m3, err3)); }
        delete t;
    }
    return true;
}


// Long VM / folder names: a path over MAX_PATH (260) must work everywhere (\\?\ prefix), names up to what NTFS allows (255 UTF-16 units).
VM2B_TEST("engine", long_names_and_paths)
{
    HTmp tmp(c_, "ln");
    size_t lens[] = { 20, 90, 120, 150, 200 };
    for (size_t li = 0; li < 5; ++li) {
        Str vm; while (vm.size() < lens[li]) vm += c_.coin() ? "\xd0\xb1" : "xy";
        if (vm.size() > lens[li] + 1) vm = vm.substr(0, lens[li]);
        if (!Utf8Valid(vm.data(), vm.size())) vm = vm.substr(0, vm.size() - 1);
        Str root = tmp.sub(Str::fmt("vms%zu", li)); MakeSimpleVm(c_, root, vm, 0);
        Str deep = tmp.sub("store"); for (int k = 0; k < (int)li * 2; ++k) deep = JoinPath(deep, "level-with-some-length-0123456789");
        Target* t = LocalAt(deep); BackupOptions o = Opts(c_, c_.coin()); o.codec.level = 1;
        VmSource* src = NewDirectorySource(root); Manifest m; Str w, err;
        bool ok = BackupVm(*src, vm, *t, o, nullptr, m, w, err);
        if (!ok) { c_.note("len %zu (%zu UTF-8 bytes, store path %zu): %s", lens[li], vm.size(), deep.size(), err.c_str()); Vec<Str> names; t->list(names); for (const Str& n : names) c_.note("  in folder: %s (%zu)", n.c_str(), n.size()); }
        T_CHECK(ok);
        if (ok) { bool vOk = false, rOk = false; Poke(*t, m.archive, o.password, tmp.sub("scratch"), &vOk, &rOk); T_CHECK(vOk && rOk); }
        delete src; delete t;
    }
    return true;
}

// ---- encryption ------------------------------------------------------------------------------------------------------
static bool DecAll(const Bytes& enc, const Str& pw, u32 volume, Bytes& out, bool* final = nullptr, Str* err = nullptr)
{
    MemSource src(enc.data(), enc.size()); DecryptSource ds(&src, pw, volume);
    out.clear(); Bytes buf; buf.resize(70000);
    for (;;) { i64 k = ds.read(buf.data(), buf.size()); if (k < 0) { if (err) *err = ds.err; return false; } if (k == 0) break; size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf.data(), (size_t)k); }
    if (final) *final = ds.finalVolume();
    return true;
}

static Bytes EncVol(const Bytes& d, const Str& pw, const u8* salt, u32 vol, bool last, u32 iter = 2000, size_t chunk = 1 << 18)
{
    MemSink ms; EncryptSink es(&ms, pw, iter, salt, vol); es.setLastVolume(last);
    for (size_t o = 0; o < d.size(); o += chunk) es.write(d.data() + o, d.size() - o < chunk ? d.size() - o : chunk);
    es.finish(); return ms.data;
}

VM2B_TEST("core", encrypt_header_and_volume_games)
{
    Bytes d = Noise(c_, 2500000, 1), out; u8 salt[16]; c_.fill(salt, 16);
    // headers: short, wrong magic, hostile iteration counts, wrong volume index -- fast errors, no hang
    Bytes v0 = EncVol(d, "pw", salt, 0, true);
    T_CHECK(DecAll(v0, "pw", 0, out) && out.size() == d.size());
    for (size_t cut = 0; cut < 40; ++cut) { Bytes x = v0; x.resize(cut); T_CHECK(!DecAll(x, "pw", 0, out)); }
    for (int t = 0; t < 8; ++t) { Bytes x = v0; x[t] ^= 0x55; T_CHECK(!DecAll(x, "pw", 0, out)); }
    DWORD t0 = GetTickCount();
    u32 bad[] = { 0, 1, 999, 100000001, 0xffffffffu, 0x80000000u };
    for (u32 it : bad) { Bytes x = v0; x[24] = (u8)(it >> 24); x[25] = (u8)(it >> 16); x[26] = (u8)(it >> 8); x[27] = (u8)it; Str er; T_CHECK(!DecAll(x, "pw", 0, out, nullptr, &er)); }
    T_CHECK(GetTickCount() - t0 < 5000);                                                                       // refused up front, not derived
    { Bytes x = v0; x[31] ^= 1; T_CHECK(!DecAll(x, "pw", 0, out)); }                                             // wrong volume index in the header
    T_CHECK(!DecAll(v0, "pw", 1, out));                                                                         // a volume read as another one
    // the nonce (volume, block) is never reused: all blocks of all volumes of one backup are different ciphertexts for equal plaintext
    Bytes zeros; zeros.resize(4 << 20);
    Bytes a = EncVol(zeros, "pw", salt, 0, false, 2000, 1 << 20), b = EncVol(zeros, "pw", salt, 1, false, 2000, 1 << 20);
    Vec<Str> seen;
    for (int pass = 0; pass < 2; ++pass) {
        const Bytes& x = pass ? b : a; size_t o = 32;
        while (o + 4 <= x.size()) { u32 len = (u32)x[o] << 24 | (u32)x[o + 1] << 16 | (u32)x[o + 2] << 8 | x[o + 3]; Str blk((const char*)x.data() + o + 4, 32 < len ? 32 : len); for (const Str& s : seen) T_CHECK(s != blk); seen.push_back(blk); o += 4 + len; }
    }
    T_CHECK(seen.size() >= 8);
    // two encryptions of the same data with a fresh salt differ
    Bytes e1 = EncVol(d, "pw", nullptr, 0, true), e2 = EncVol(d, "pw", nullptr, 0, true);
    T_CHECK(e1.size() == e2.size() && memcmp(e1.data() + 8, e2.data() + 8, 16) != 0);
    // multi-volume: every boundary. Volumes 0..2 of one backup; reorder / duplicate / drop / truncate at block boundaries.
    Bytes p0 = Noise(c_, 3 << 20, 1), p1 = Noise(c_, 1500000, 1), p2 = Noise(c_, 100, 1);
    Bytes w0 = EncVol(p0, "pw", salt, 0, false, 2000, 1 << 20), w1 = EncVol(p1, "pw", salt, 1, false, 2000, 1 << 20), w2 = EncVol(p2, "pw", salt, 2, true, 2000, 1 << 20);
    bool fin = false;
    T_CHECK(DecAll(w0, "pw", 0, out, &fin) && !fin); T_CHECK(DecAll(w1, "pw", 1, out, &fin) && !fin); T_CHECK(DecAll(w2, "pw", 2, out, &fin) && fin && out.size() == 100);
    T_CHECK(!DecAll(w1, "pw", 0, out)); T_CHECK(!DecAll(w2, "pw", 1, out)); T_CHECK(!DecAll(w0, "pw", 2, out));       // reordered / swapped
    // blocks moved between volumes (same salt, same key!): block 0 of volume 0 pasted into volume 1 in place of its own
    {
        size_t s0 = 32; u32 l0 = (u32)w0[s0] << 24 | (u32)w0[s0 + 1] << 16 | (u32)w0[s0 + 2] << 8 | w0[s0 + 3];
        size_t s1 = 32; u32 l1 = (u32)w1[s1] << 24 | (u32)w1[s1 + 1] << 16 | (u32)w1[s1 + 2] << 8 | w1[s1 + 3];
        Bytes x = Prefix(w1, 32); for (size_t i = 0; i < 4 + l0; ++i) x.push_back(w0[s0 + i]); for (size_t i = s1 + 4 + l1; i < w1.size(); ++i) x.push_back(w1[i]);
        T_CHECK(!DecAll(x, "pw", 1, out));
    }
    // truncation / duplication / reorder of blocks at every boundary of the 3-block volume
    Vec<size_t> st; { size_t o = 32; while (o < w0.size()) { st.push_back(o); u32 len = (u32)w0[o] << 24 | (u32)w0[o + 1] << 16 | (u32)w0[o + 2] << 8 | w0[o + 3]; o += 4 + len; } st.push_back(w0.size()); }
    T_CHECK(st.size() == 4);                                                                                       // 3 MiB = 3 blocks (+ the end)
    for (size_t cut = 0; cut + 1 < st.size(); ++cut) { Bytes x = Prefix(w0, st[cut]); T_CHECK(!DecAll(x, "pw", 0, out)); }     // cut at every block boundary, even keeping the header only
    for (size_t b2 = 0; b2 + 1 < st.size(); ++b2) {
        Bytes x = Prefix(w0, st[b2 + 1]); for (size_t i = st[b2]; i < st[b2 + 1]; ++i) x.push_back(w0[i]);                  // block b2 doubled
        for (size_t i = st[b2 + 1]; i < w0.size(); ++i) x.push_back(w0[i]);
        T_CHECK(!DecAll(x, "pw", 0, out));
    }
    for (size_t b2 = 0; b2 + 1 < st.size(); ++b2) {                                                                // block b2 removed
        Bytes x = Prefix(w0, st[b2]); for (size_t i = st[b2 + 1]; i < w0.size(); ++i) x.push_back(w0[i]);
        T_CHECK(!DecAll(x, "pw", 0, out));
    }
    // block length field damage: huge, zero, off by one
    u32 lens[] = { 0, 15, 16, 17, 0x7fffffffu, 0xffffffffu, (1u << 20) + 17 };
    for (u32 L : lens) { Bytes x = w0; x[32] = (u8)(L >> 24); x[33] = (u8)(L >> 16); x[34] = (u8)(L >> 8); x[35] = (u8)L; T_CHECK(!DecAll(x, "pw", 0, out)); }
    // an empty password and unicode passwords round trip; a password that differs in one byte fails
    Bytes ep = EncVol(p2, "", salt, 0, true); T_CHECK(DecAll(ep, "", 0, out)); T_CHECK(!DecAll(ep, " ", 0, out));
    Str uni = "\xd0\x9f\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c\xe6\x97\xa5\xe6\x9c\xac\xf0\x9f\x94\x91";
    Bytes up = EncVol(p2, uni, salt, 0, true); T_CHECK(DecAll(up, uni, 0, out)); T_CHECK(!DecAll(up, uni.substr(0, uni.size() - 1), 0, out));
    return true;
}


// ---- tar -----------------------------------------------------------------------------------------------------------
namespace {
// head bytes, then `zeros` zero bytes, then tail bytes: a huge archive without the memory
struct GenSource : Source {
    Bytes head, tail; u64 zeros, pos; u64 total;
    GenSource(const Bytes& h, u64 z, const Bytes& t) : head(h), tail(t), zeros(z), pos(0), total(h.size() + z + t.size()) {}
    i64 read(void* p, size_t n) override {
        if (pos >= total) return 0;
        u8* o = (u8*)p; size_t got = 0;
        while (got < n && pos < total) {
            if (pos < head.size()) { size_t k = head.size() - (size_t)pos; if (k > n - got) k = n - got; memcpy(o + got, head.data() + pos, k); got += k; pos += k; }
            else if (pos < head.size() + zeros) { u64 left = head.size() + zeros - pos; size_t k = left > n - got ? n - got : (size_t)left; memset(o + got, 0, k); got += k; pos += k; }
            else { size_t i = (size_t)(pos - head.size() - zeros); size_t k = tail.size() - i; if (k > n - got) k = n - got; memcpy(o + got, tail.data() + i, k); got += k; pos += k; }
        }
        return (i64)got;
    }
};

// a raw header block with the given fields (octal text), correct checksum
Bytes RawHeader(const Str& name, const Str& sizeField, char type, const Str& mtimeField = "00000000000")
{
    Bytes h; h.resize(512);
    memcpy(h.data(), name.data(), name.size() > 100 ? 100 : name.size());
    memcpy(h.data() + 100, "0000644", 7); memcpy(h.data() + 124, sizeField.data(), sizeField.size() > 12 ? 12 : sizeField.size());
    memcpy(h.data() + 136, mtimeField.data(), mtimeField.size() > 12 ? 12 : mtimeField.size());
    h[156] = (u8)type; memcpy(h.data() + 257, "ustar", 5); memcpy(h.data() + 263, "00", 2);
    memset(h.data() + 148, ' ', 8); u32 sum = 0; for (int i = 0; i < 512; ++i) sum += h[i];
    Str ck = Str::fmt("%06o", sum); memcpy(h.data() + 148, ck.data(), 6); h[154] = 0; h[155] = ' ';
    return h;
}
Bytes PaxBlock(const Str& records)    // header + padded body of an 'x' entry
{
    Bytes b = RawHeader("PaxHeaders/x", Str::fmt("%011o", (unsigned)records.size()), 'x');
    for (size_t i = 0; i < records.size(); ++i) b.push_back((u8)records[i]);
    while (b.size() % 512) b.push_back(0);
    return b;
}
Str PaxRec(const Str& k, const Str& v)
{
    Str body = k + "=" + v + "\n"; size_t base = body.size() + 1, len = base + 1;
    for (;;) { size_t d = Str::fmt("%zu", len).size(); if (base + d == len) break; len = base + d; }
    return Str::fmt("%zu ", len) + body;
}
void Append(Bytes& a, const Bytes& b) { for (size_t i = 0; i < b.size(); ++i) a.push_back(b[i]); }
Bytes Zeros(size_t n) { Bytes z; z.resize(n); return z; }
} // namespace

VM2B_TEST("core", tar_huge_sizes_and_pax_edges)
{
    // 1. sizes around the 8 GiB octal limit are written with a PAX size record and read back (nothing is stored: zeros are generated)
    u64 sizes[] = { 077777777777ull, 077777777777ull + 1, 077777777777ull + 511, (u64)9 << 30 | 12345 };
    for (u64 sz : sizes) {
        MemSink ms; TarWriter w(&ms); T_CHECK(w.begin_file("big/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb.vmdk", sz, 1700000000)); size_t hdr = ms.data.size();
        T_CHECK(hdr == 512 || hdr == 1536);
        u64 pad = (512 - (sz & 511)) & 511;
        Bytes tail; Append(tail, RawHeader("after.txt", "00000000004", '0')); { Bytes d = Zeros(512); memcpy(d.data(), "tail", 4); Append(tail, d); } Append(tail, Zeros(1024));
        GenSource gs(ms.data, sz + pad, tail); TarReader r(&gs); TarEntry e;
        T_CHECK(r.next(e) == 1); T_CHECK(e.size == sz); T_CHECK(e.path == "big/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb.vmdk"); T_CHECK(e.mtime == 1700000000);
        u8 b[1000]; T_CHECK(r.read(b, sizeof(b)) > 0);
        T_CHECK(r.next(e) == 1); T_CHECK(e.path == "after.txt"); T_CHECK(e.size == 4);
        T_CHECK(r.read(b, 100) == 4 && !memcmp(b, "tail", 4)); T_CHECK(r.next(e) == 0);
    }
    // 2. hostile PAX records: none crashes, none produces a negative / absurd entry that is then "restored"
    const char* recs[] = { "size=-5", "size=18446744073709551615", "size=99999999999999999999999", "size=1e3", "size=", "mtime=nan", "mtime=1e999", "mtime=-1e999", "mtime=9223372036854775807", "mtime=-9223372036854775808",
                           "path=", "path=../../x", "path=a\nb", "=x", "novalue", "size=0x10" };
    for (const char* rc : recs) {
        Bytes a; Str full = PaxRec(Str(rc).substr(0, Str(rc).find('=') >= 0 ? (size_t)Str(rc).find('=') : strlen(rc)), Str(rc).find('=') >= 0 ? Str(rc).substr((size_t)Str(rc).find('=') + 1) : Str());
        Append(a, PaxBlock(full)); Append(a, RawHeader("f.bin", "00000000012", '0')); Append(a, Zeros(512)); Append(a, Zeros(1024));
        MemSource src(a.data(), a.size()); TarReader r(&src); TarEntry e; int n = 0, rcn;
        while ((rcn = r.next(e)) == 1 && n < 5) { ++n; u8 b[600]; for (int k = 0; k < 4; ++k) { i64 q = r.read(b, sizeof(b)); if (q <= 0) break; } }
        T_CHECK(rcn == 0 || rcn == -1);
    }
    // a record whose length lies (too short / too long / zero / huge)
    const char* lies[] = { "3 x=y\n", "999 path=a\n", "0 path=a\n", "18446744073709551616 path=a\n", "10 path=a\n10 path=b\n", "12 path=abc", "2 \n" };
    for (const char* l : lies) {
        Bytes a; Append(a, PaxBlock(l)); Append(a, RawHeader("f.bin", "00000000000", '0')); Append(a, Zeros(1024));
        MemSource src(a.data(), a.size()); TarReader r(&src); TarEntry e; int rcn = r.next(e); T_CHECK(rcn == 1 || rcn == -1 || rcn == 0);
        if (rcn == 1) { Str o; SafeArchivePath(e.path, o); }
    }
    // 3. odd headers: unknown / link / device / sparse / GNU long name types, directories without a slash, bad checksum, size field garbage
    {
        Bytes a;
        Append(a, RawHeader("hard", "00000000000", '1')); Append(a, RawHeader("sym", "00000000000", '2')); Append(a, RawHeader("chr", "00000000000", '3'));
        Append(a, RawHeader("fifo", "00000000000", '6'));
        Append(a, RawHeader("sparse", "00000000100", 'S')); Append(a, Zeros(512));
        Append(a, RawHeader("dirnoslash", "00000000000", '5'));
        { Bytes d = Zeros(512); memcpy(d.data(), "gnu-long-name-file-xyz", 22); Append(a, RawHeader("././@LongLink", "00000000027", 'L')); Append(a, d); }
        Append(a, RawHeader("short", "00000000003", '0')); { Bytes d = Zeros(512); memcpy(d.data(), "abc", 3); Append(a, d); }
        Append(a, Zeros(1024));
        MemSource src(a.data(), a.size()); TarReader r(&src); TarEntry e; int n = 0; Str names;
        while (r.next(e) == 1) { ++n; names += e.path + (e.skipped ? "(skip)" : e.isDir ? "(dir)" : "") + ";"; }
        T_CHECK(names.find("dirnoslash(dir)") >= 0); T_CHECK(names.find("gnu-long-name-file-xyz") >= 0); T_CHECK(names.find("sparse(skip)") >= 0 || names.find("sparse") < 0);
        T_CHECK(n >= 3);
        Bytes bad = a; bad[0] ^= 1;                                         // the checksum no longer matches
        MemSource s2(bad.data(), bad.size()); TarReader r2(&s2); T_CHECK(r2.next(e) == -1);
        Bytes junk = RawHeader("junk", "99999999999", '0'); for (int i = 124; i < 136; ++i) junk[(size_t)i] = (u8)(i % 2 ? 0xff : 0x80);
        Bytes ju = junk; memset(ju.data() + 148, ' ', 8); u32 sum = 0; for (int i = 0; i < 512; ++i) sum += ju[(size_t)i]; Str ck = Str::fmt("%06o", sum); memcpy(junk.data() + 148, ck.data(), 6);
        Append(junk, Zeros(2048)); MemSource s3(junk.data(), junk.size()); TarReader r3(&s3); int rr = r3.next(e); T_CHECK(rr == 1 || rr == -1);
    }
    // 4. a state of the reader that was saved and loaded: hostile values are refused or harmless
    {
        Json j = Json::object(); j["left"] = (u64)1 << 62; j["pad"] = 9999; TarReader r(nullptr); T_CHECK(!r.loadState(j));
        Json k = Json::object(); k["left"] = -1; k["pad"] = 5; k["cur"] = Json::array(); TarReader r2(nullptr); r2.loadState(k);
        MemSource none((const u8*)"", 0); TarReader r3(&none); r3.loadState(Json()); TarEntry e; (void)e;
    }
    // 5. SafeArchivePath: the table of what must and must not pass
    struct { const char* p; bool ok; } paths[] = { { "vm/a.vmdk", true }, { "./vm/a", true }, { "vm//a", true }, { "vm/./a", true }, { "a b/c d.txt", true }, { "\xd0\xb4/\xd1\x84", true },
        { "", false }, { "/", false }, { "/abs", false }, { "..", false }, { "a/../b", false }, { "a/..", false }, { "a\\b", false }, { "C:/x", false }, { "a:b", false }, { "a/b:stream", false },
        { "con", false }, { "a/NUL.txt", false }, { "lpt1", false }, { "com0", false }, { "a/aux ", false }, { "trail.", false }, { "trail /x", false }, { "a*b", false }, { "a?b", false }, { "a\"b", false }, { "a<b", false }, { "a|b", false },
        { "\xc0\x80", false }, { "\xed\xa0\x80", false }, { "a\x01b", false }, { "a\x7f" "b", true }, { ".", false }, { "./", false }, { "...", false }, { "a/...", false }, { "conx", true }, { "com10", true }, { "prn.x/y", false } };
    for (const auto& t : paths) { Str out; bool ok = SafeArchivePath(t.p, out); if (ok != t.ok) c_.note("SafeArchivePath(\"%s\") = %d, want %d", Str(t.p).replace_all("\x01", "?").c_str(), (int)ok, (int)t.ok); T_CHECK(ok == t.ok); }
    { Str longp; for (int i = 0; i < 1100; ++i) longp += 'a'; Str out; T_CHECK(!SafeArchivePath(longp, out)); }
    return true;
}


// Big archives through the engine: a sparse 5 GiB disk (tar > 4 GiB, PAX size records, many volumes), memory must stay flat.
// Skipped unless VM2B_BIG is set (group "big"):  VM2B_BIG=1 ... --only big [--seed N]. Needs ~6 GiB free.
static bool BigRun(Ctx& c_, const char* codecName, int level, u64 gib)
{
    wchar_t ev[8]; if (!GetEnvironmentVariableW(L"VM2B_BIG", ev, 8)) { c_.skip("set VM2B_BIG=1 to run (needs several GiB of disk and minutes)"); return true; }
    HTmp tmp(c_, "big");
    Str root = tmp.sub("vms"); Str vmdir = JoinPath(root, "vm"); Str e; MakeDirs(vmdir, e);
    { // a sparse file with a few islands of real data
        HANDLE h = CreateFileW(FilePath(JoinPath(vmdir, "disk.vmdk")), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { c_.note("cannot create the sparse file"); return false; }
        Bytes data = Noise(c_, 3 << 20, 2); DWORD w;
        for (int i = 0; i < 6; ++i) { LARGE_INTEGER p; p.QuadPart = (LONGLONG)((u64)i * (gib << 30) / 6); SetFilePointerEx(h, p, nullptr, FILE_BEGIN); WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr); }
        LARGE_INTEGER end; end.QuadPart = (LONGLONG)(gib << 30); SetFilePointerEx(h, end, nullptr, FILE_BEGIN); SetEndOfFile(h); CloseHandle(h);
    }
    Target* t = LocalAt(tmp.sub("store")); BackupOptions o; o.codec.level = level; o.retry = Fast(); o.segmentBytes = (u64)256 << 20; o.verify = true;
    VmSource* src = NewDirectorySource(root); Manifest m; Str w, err;
    DWORD t0 = GetTickCount(); double p0 = PeakMiB();
    bool ok = BackupVm(*src, "vm", *t, o, nullptr, m, w, err);
    double secs = (GetTickCount() - t0) / 1000.0; double p1 = PeakMiB();
    c_.note("%s %llu GiB: %s in %.0f s, %zu volumes, stored %s, peak memory %.0f MiB (before %.0f)", codecName, (unsigned long long)gib, ok ? "ok" : err.c_str(), secs, m.segments.size(), FormatSize(m.size).c_str(), p1, p0);
    T_CHECK(ok);
    if (p1 > 0) T_CHECK(p1 < 1500);                                           // flat: a few volumes' worth of buffers, not the archive
    if (ok) { T_CHECK(m.rawSize >= (gib << 30)); }
    delete src; delete t;
    return true;
}

VM2B_TEST("big", big_sparse_xz1) { return BigRun(c_, "xz level 1", 1, 5); }

} // namespace vm2b

namespace vm2b {
VM2B_TEST("engine", safe_name_guards_windows_device_names)
{
    const char* bad[] = { "nul.txt", "NUL", "con", "Aux.log", "com1", "LPT9.x", "prn.", "nul..", "nul. " };
    for (const char* n : bad) {
        Str r = SafeName(n);
        size_t dot = 0; while (dot < r.size() && r[dot] != '.') ++dot;
        Str stem = r.substr(0, dot); Str up; for (size_t i = 0; i < stem.size(); ++i) up += (char)((stem[i] >= 'a' && stem[i] <= 'z') ? stem[i] - 32 : stem[i]);
        bool dev = up == "CON" || up == "PRN" || up == "AUX" || up == "NUL" || ((up.size() == 4) && (up.starts_with("COM") || up.starts_with("LPT")) && up[3] >= '0' && up[3] <= '9');
        T_CHECK(!dev); T_CHECK(!r.empty()); T_CHECK(!r.ends_with("."));
    }
    T_CHECK(SafeName("console") == "console");           // a longer word that merely starts with a device name is fine
    T_CHECK(SafeName("com10") == "com10");
    return true;
}
// A manifest naming any codec but xz (a foreign or hostile one, or a backup of an older version) is listed but refused with a clear
// message: never a crash, never "OK".
VM2B_TEST("engine", foreign_codec_manifest_is_refused)
{
    HTmp tmp(c_, "fc");
    Str root = tmp.sub("vms"); MakeSimpleVm(c_, root, "vm", 30000);
    Str store = tmp.sub("s"); Target* t = LocalAt(store);
    BackupOptions o = Opts(c_, c_.coin()); Manifest m; Str err;
    T_CHECK(Backup(*t, root, o, m, err));
    T_CHECK(m.codec == "xz"); T_CHECK(m.archive.ends_with(o.password.empty() ? ".tar.xz" : ".tar.xz.enc"));
    Str mpath = JoinPath(store, m.archive + ".json"); Bytes orig; Str e; T_CHECK(ReadWholeFile(mpath.c_str(), orig, e));
    Json base; T_CHECK(Json::parse((const char*)orig.data(), orig.size(), base, e));
    Json cases = Json::array(); const char* names[] = { "gzip", "none", "zstd", "XZ", "", "xz\n", "\x01\xff\xfe-hostile" };
    for (const char* n : names) cases.push(Str(n));
    cases.push(Json((i64)2)); cases.push(Json()); cases.push(Json::object());
    for (size_t i = 0; i < cases.size(); ++i) {
        Json j = base; j["compression"] = cases.at(i);
        if (c_.coin(20)) j.remove("compression");
        Str js = j.dump(); T_CHECK(PutFile(mpath, js.data(), js.size()));
        Vec<Manifest> ms; Str le; ListBackups(*t, nullptr, ms, le);
        Str dest = tmp.sub("r"); RemoveTree(dest); ReadOptions ro; ro.retry = Fast(2); ro.resume = false; Str re;
        T_CHECK(!RestoreBackup(*t, m.archive, dest, o.password, ro, nullptr, nullptr, re));
        T_CHECK(re.find("only xz") >= 0); T_CHECK(re.size() < 200);
        Str ve; bool v = VerifyBackup(*t, m.archive, o.password, ro, nullptr, nullptr, ve); (void)v;     // any answer, no crash
    }
    T_CHECK(PutFile(mpath, orig.data(), orig.size()));
    bool vOk = false, rOk = false; Poke(*t, m.archive, o.password, tmp.sub("scratch"), &vOk, &rOk); T_CHECK(vOk && rOk);
    // a journal of another codec is not resumable (ignored), it never blocks the next backup
    delete t;
    return true;
}

// A config written by an older version (codec gzip / none / unknown / missing) is migrated on load to xz and written back.
VM2B_TEST("service", old_config_is_migrated_to_xz)
{
    HTmp tmp(c_, "mg");
    Str dir = tmp.sub("data"); Str e; T_CHECK(MakeDirs(dir, e));
    const char* old = R"({"servers":[],"scheduler":false,"settings":{"codec":"gzip","level":7,"segment_mib":64},
        "tasks":[{"id":"t1","name":"Old","elements":[
          {"type":"backup","compress":{"codec":"none","level":0}},
          {"type":"backup","compress":{"codec":"gzip","level":9}},
          {"type":"backup","compress":{"codec":"gzip","level":0}},
          {"type":"backup","compress":{"codec":"gzip","level":42}},
          {"type":"backup","compress":{"codec":"lz4","level":3}},
          {"type":"backup","compress":{"level":4}},
          {"type":"backup","compress":{"codec":"xz","level":2}},
          {"type":"backup","compress":"junk"},
          {"type":"backup"}]}]})";
    T_CHECK(PutFile(JoinPath(dir, "config.json"), old, strlen(old)));
    {
        Config cfg; Str err; T_CHECK(cfg.open(dir, err));
        Json st = cfg.settings(); T_CHECK(!st.has("codec")); T_CHECK(st.get("level").integer() == 7); T_CHECK(st.get("threads").integer() == 0);
        Json t; T_CHECK(cfg.task("t1", true, t));
        const Json& el = t.get("elements");
        int want[] = { 1, 9, 6, 9, 5, 4, 2, 0 };
        T_CHECK(el.size() == 9);
        for (size_t i = 0; i < 8; ++i) { T_CHECK(el.at(i).get("compress").get("codec").str() == "xz"); T_CHECK(el.at(i).get("compress").get("level").integer() == want[i]); }
        T_CHECK(!el.at(8).has("compress"));
    }
    Bytes b; T_CHECK(ReadWholeFile(JoinPath(dir, "config.json").c_str(), b, e));           // written back: no old codec is left
    Str txt((const char*)b.data(), b.size()); T_CHECK(txt.find("gzip") < 0); T_CHECK(txt.find("\"none\"") < 0); T_CHECK(txt.find("lz4") < 0);
    // settings of "none" / unknown / missing codec
    const char* olds[] = { R"({"settings":{"codec":"none","level":0}})", R"({"settings":{"codec":"bogus","level":3}})", R"({"settings":{"level":8}})", R"({"settings":{"codec":"gzip"}})" };
    int wantLv[] = { 1, 5, 8, 6 };
    for (int i = 0; i < 4; ++i) {
        Str d2 = tmp.sub(Str::fmt("d%d", i)); T_CHECK(MakeDirs(d2, e)); T_CHECK(PutFile(JoinPath(d2, "config.json"), olds[i], strlen(olds[i])));
        Config cfg; Str err; T_CHECK(cfg.open(d2, err)); T_CHECK(cfg.settings().get("level").integer() == wantLv[i]); T_CHECK(!cfg.settings().has("codec"));
    }
    return true;
}

} // namespace vm2b
