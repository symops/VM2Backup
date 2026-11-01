// Автотесты против НАСТОЯЩЕГО ESXi (группа "esxireal"): не эмулятор, а живой хост (docs/ESXI.md, раздел «Проверка на настоящем ESXi 7»).
// Запуск: tools/esxi7/run-real.sh (поднимает вложенный ESXi, считает контрольные суммы дисков на хосте по SSH и пишет servers.json с ключом
// "esxi_real").  Без этого ключа тесты пропускаются, остальные группы они не затрагивают.
//
//   "esxi_real": { "host", "port", "user", "password_file" (путь к файлу с паролем: пароль не попадает ни в командную строку, ни в журнал),
//                  "fingerprint" ("sha256:.." самоподписанного сертификата, пусто = verify выключен),
//                  "vms": [ { "name", "on": true|false, "flat": { "<имя>-flat.vmdk": "<sha256>" } } ],   // эталон до бэкапа (sha256sum на хосте)
//                  "restore_prefix": "имя-префикс для восстановленных ВМ" }
#include "autotest.h"
#include "crypto.h"
#include "engine.h"
#include "esxi.h"
#include "winutil.h"

namespace vm2b {

namespace {

struct RealVm { Str name; bool on, hist; Vec<Str> files, hashes; };
struct Real {
    Str host, user, password, fp, prefix; int port; Vec<RealVm> vms;
    Json cfg(const Str& pw = Str(), bool quiesce = false) const {
        Json c = Json::object();
        c["type"] = "esxi"; c["host"] = host; c["port"] = port; c["user"] = user; c["password"] = pw.empty() ? password : pw;
        if (!fp.empty()) c["fingerprint"] = fp; else c["verify"] = false;
        c["task_timeout"] = 300; c["remove_timeout"] = 600; c["timeout"] = 120;
        Json s = Json::object(); s["quiesce"] = quiesce; s["memory"] = false; c["snapshot"] = s;
        return c;
    }
};

static bool Load(Ctx& c, Real& r)
{
    if (c.servers.is_null() || !c.servers.has("esxi_real")) { c.skip("нет ключа \"esxi_real\" (tools/esxi7/run-real.sh)"); return false; }
    const Json& j = c.servers.get("esxi_real");
    r.host = j.get("host").str(); r.user = j.get("user").str(); r.fp = j.get("fingerprint").str(); r.port = (int)j.get("port").integer();
    r.prefix = j.get("restore_prefix").str("vm2b-rt");
    Bytes b; Str e;
    if (!ReadWholeFile(j.get("password_file").str().c_str(), b, e)) { c.note("не прочитан файл пароля: %s", e.c_str()); return false; }
    size_t n = b.size(); while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) --n;
    r.password = Str((const char*)b.data(), n);
    const Json& v = j.get("vms");
    for (size_t i = 0; i < v.size(); ++i) {
        RealVm m; m.name = v.at(i).get("name").str(); m.on = v.at(i).get("on").boolean(); m.hist = v.at(i).get("hist").boolean();
        const Json& f = v.at(i).get("flat"); for (size_t k = 0; k < f.size(); ++k) { m.files.push_back(f.key_at(k)); m.hashes.push_back(f.value_at(k).str()); }
        r.vms.push_back(m);
    }
    return !r.vms.empty();
}

struct Tmp {
    Str path; bool keep;
    Tmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path); Str e; MakeDirs(path, e);
    }
    ~Tmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

static Str HashFileReal(const Str& path, u64& size)
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

static Target* LocalAtR(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }

struct Pr : Progress {
    u64 bytes; int retries, resumes; bool abortAt, keep; u64 limit;
    Pr() : bytes(0), retries(0), resumes(0), abortAt(false), keep(false), limit(0) {}
    void onBytes(u64 n) override { bytes += n; }
    void onRetry(const Str&, int, int, int, const Str&) override { ++retries; }
    void onResume(int, u64) override { ++resumes; }
    bool aborted() override { return abortAt && bytes >= limit; }
    bool keepOnAbort() override { return keep; }
};

static bool Has(const Str& s, const char* t) { return s.find(t) >= 0; }

// Каждый -flat.vmdk из эталона в восстановленной папке совпадает побитно (sha256) с тем, что был на хосте до бэкапа.
static bool CheckFlats(Ctx& c, const RealVm& vm, const Str& dest)
{
    bool ok = true;
    for (size_t i = 0; i < vm.files.size(); ++i) {
        u64 sz = 0; Str h = HashFileReal(JoinPath(JoinPath(dest, EsxiArchivePrefix(vm.name)), vm.files[i]), sz);
        if (h != vm.hashes[i]) { c.note("%s: sha256 не совпал (%llu байт, %s)", vm.files[i].c_str(), (unsigned long long)sz, h.empty() ? "нет файла" : "другой хэш"); ok = false; }
        else c.note("%s: sha256 совпал (%llu байт)", vm.files[i].c_str(), (unsigned long long)sz);
    }
    return ok;
}

} // namespace

VM2B_TEST("esxireal", esxireal_connect_and_list)
{
    Real r; if (!Load(c_, r)) return true;
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Vec<VmInfo> vms; T_CHECK(src->list(vms));
    c_.note("сервер: %s", static_cast<EsxiSource*>(src)->serverInfo().c_str());
    T_CHECK(Has(static_cast<EsxiSource*>(src)->serverInfo(), "7.0"));
    for (const RealVm& v : r.vms) { bool found = false; for (const VmInfo& i : vms) if (i.name == v.name) { found = true; c_.note("ВМ %s: %s, %llu байт", i.name.c_str(), i.state.c_str(), (unsigned long long)i.size); } T_CHECK(found); }
    delete src;
    return true;
}

VM2B_TEST("esxireal", esxireal_wrong_credentials)
{
    Real r; if (!Load(c_, r)) return true;
    for (int kind = 0; kind < 3; ++kind) {
        Json cfg = r.cfg();
        if (kind == 0) cfg["password"] = "wrong-" + r.password;
        if (kind == 1) cfg["user"] = "nobody";
        if (kind == 2) cfg["fingerprint"] = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr); if (!src) return false;
        Vec<VmInfo> vms; T_CHECK(!src->list(vms)); T_CHECK(!src->transient);
        c_.note("вид %d: %s", kind, src->err.c_str());
        T_CHECK(!Has(src->err, r.password.c_str()));
        if (kind < 2) T_CHECK(Has(src->err, "login failed")); else T_CHECK(Has(src->err, "sha256:"));
        delete src;
    }
    return true;
}

VM2B_TEST("esxireal", esxireal_backup_verify_restore_matrix)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal");
    struct Mode { const char* pw; int level; u64 seg; };
    static const Mode modes[] = { { "", 1, 0 }, { "real-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c", 5, (u64)512 * 200000 }, { "", 9, (u64)512 * 4096 }, { "x", 0, 0 } };
    int idx = 0;
    for (const RealVm& v : r.vms) {
        for (const Mode& m : modes) {
            ++idx;
            Str store = tmp.sub(Str::fmt("store%d", idx).c_str()), dest = tmp.sub(Str::fmt("r%d", idx).c_str());
            BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.password = m.pw; o.codec.level = m.level; if (m.seg) o.segmentBytes = m.seg;
            o.allowLive = v.on;
            Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
            Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr;
            bool ok = BackupVm(*src, v.name, *t, o, &pr, man, warn, err);
            if (!ok) c_.note("%s (pw=%d level=%d): бэкап: %s", v.name.c_str(), m.pw[0] != 0, m.level, err.c_str());
            T_CHECK(ok);
            if (ok) {
                c_.note("%s (pw=%d level=%d seg=%llu): %llu байт в %llu, предупреждение: %s", v.name.c_str(), m.pw[0] != 0, m.level, (unsigned long long)m.seg,
                        (unsigned long long)man.rawSize, (unsigned long long)man.size, warn.c_str());
                Str ve; T_CHECK(VerifyBackup(*t, man.archive, o.password, nullptr, nullptr, ve));
                // неверный пароль архива не должен проходить
                if (!o.password.empty()) { Str e2; T_CHECK(!VerifyBackup(*t, man.archive, "wrong", nullptr, nullptr, e2)); }
                Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
                if (!re.empty()) c_.note("restore: %s", re.c_str());
                T_CHECK(CheckFlats(c_, v, dest));
            }
            delete t; delete src; RemoveTree(store); RemoveTree(dest);
        }
        // после копии (в том числе ВКЛЮЧЁННОЙ ВМ: снимок и клоны дисков) на хосте ничего не осталось: следующий begin() не находит старых снимков
        Str e2; VmSource* s2 = NewEsxiSource(r.cfg(), e2); Str fp, tok, w2; bool b = s2 && s2->begin(v.name, true, Str(), fp, tok, w2);
        T_CHECK(b); T_CHECK(!Has(w2, "removed")); if (s2) { if (b) s2->end(false); delete s2; }
    }
    return true;
}

VM2B_TEST("esxireal", esxireal_resume_after_abort)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal-rs");
    for (const RealVm& v : r.vms) {
        if (v.hist) continue;               // у ВМ со снимками тома малы; включённая ВМ идёт через клон диска, продолжение внутри файла клона работает (Range)
        Str store = tmp.sub("store"), dest = tmp.sub("r");
        BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.segmentBytes = (u64)512 * 8192; o.codec.level = 1; o.allowLive = v.on;
        Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
        Target* t = LocalAtR(store); Manifest man; Str warn;
        Pr p1; p1.abortAt = true; p1.keep = true; p1.limit = (u64)40 << 20;
        bool ok1 = BackupVm(*src, v.name, *t, o, &p1, man, warn, err);
        T_CHECK(!ok1);
        delete src;
        Vec<ResumeInfo> ri; Str le; T_CHECK(ListResumable(*t, &v.name, ri, le)); c_.note("%s: прерываний с продолжением: %d", v.name.c_str(), (int)ri.size()); T_CHECK(ri.size() == 1);
        src = NewEsxiSource(r.cfg(), err);
        Pr p2; Str w2; ok1 = BackupVm(*src, v.name, *t, o, &p2, man, w2, err);
        if (!ok1) c_.note("продолжение: %s", err.c_str());
        T_CHECK(ok1); T_CHECK(p2.resumes >= 1);
        if (ok1) { Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re)); T_CHECK(CheckFlats(c_, v, dest)); }
        delete t; delete src; RemoveTree(store); RemoveTree(dest);
    }
    return true;
}

// Восстановление на сам ESXi: файлы PUT /folder + RegisterVM; потом побитовую сверку делает внешний скрипт (sha256sum на хосте).
VM2B_TEST("esxireal", esxireal_restore_to_host)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal-rh");
    int n = 0;
    for (const RealVm& v : r.vms) {
        ++n;
        Str store = tmp.sub("store");
        BackupOptions o; o.kdfIterations = 3000; o.verify = false; o.allowLive = v.on; o.codec.level = 1;
        Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
        Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, v.name, *t, o, &pr, man, warn, err);
        T_CHECK(ok); delete src; if (!ok) { c_.note("бэкап: %s", err.c_str()); delete t; continue; }
        EsxiRestoreOptions ro; ro.vmName = Str::fmt("%s-%d", r.prefix.c_str(), n);
        Str e1; Json info; T_CHECK(EsxiRestoreTargets(r.cfg(), Str(), info, e1)); c_.note("варианты восстановления: %s", info.dump().c_str());
        ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se);
        EsxiRestoreResult res; Str re; Pr pr2;
        bool rok = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr2, &res, re);
        if (!rok) c_.note("restore на хост: %s", re.c_str());
        T_CHECK(rok);
        if (rok) c_.note("RESTORED %s -> %s (%s)", v.name.c_str(), res.vmName.c_str(), res.vmxPath.c_str());
        // повтор с тем же именем: не перезаписывает, а отказывает
        EsxiRestoreResult res2; Str re2; Pr pr3; bool rok2 = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr3, &res2, re2);
        c_.note("повтор с занятым именем: %s", rok2 ? "ok (другое имя)" : re2.c_str());
        T_CHECK(!rok2);
        // ВМ видна в списке
        VmSource* s2 = NewEsxiSource(r.cfg(), err); Vec<VmInfo> vms; bool found = false;
        if (s2 && s2->list(vms)) for (const VmInfo& i : vms) if (i.name == ro.vmName) found = true;
        T_CHECK(found); delete s2;
        delete t; RemoveTree(store);
    }
    return true;
}


// snapshot.history: ВМ со СВОИМ деревом снимков (s1 -> s2 и s1 -> s3, текущее состояние на s3): копия с историей и без неё
VM2B_TEST("esxireal", esxireal_snapshot_history)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal-hi");
    for (const RealVm& v : r.vms) {
        if (!v.hist) continue;
        int counts[2] = { 0, 0 };
        for (int hist = 1; hist >= 0; --hist) {
            Json cfg = r.cfg(); cfg["snapshot"]["history"] = hist != 0;
            Str store = tmp.sub(hist ? "s1" : "s0"), dest = tmp.sub(hist ? "d1" : "d0");
            BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.codec.level = 1;
            Str err; VmSource* src = NewEsxiSource(cfg, err); T_CHECK(src != nullptr); if (!src) return false;
            Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr;
            bool ok = BackupVm(*src, v.name, *t, o, &pr, man, warn, err); if (!ok) c_.note("history=%d: %s", hist, err.c_str());
            T_CHECK(ok); delete src; if (!ok) { delete t; continue; }
            Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
            Bytes mb; Str me, pe; Json mj; if (ReadWholeFile(JoinPath(dest, "vm2backup-meta.json").c_str(), mb, me)) Json::parse((const char*)mb.data(), mb.size(), mj, pe);
            const Json& fl = mj.get("files"); counts[hist] = (int)fl.size(); int vmsn = 0, deltas = 0;
            for (size_t i = 0; i < fl.size(); ++i) { Str p = fl.at(i).get("path").str(); c_.note("  history=%d: %s (%lld)", hist, p.c_str(), (long long)fl.at(i).get("size").integer()); if (p.ends_with(".vmsn")) ++vmsn; if (p.find("sesparse") >= 0) ++deltas; }
            c_.note("history=%d: %d файлов, %d .vmsn, %d дельт, не скопировано снимков: %d", hist, counts[hist], vmsn, deltas, (int)mj.get("snapshots_not_copied").integer());
            T_CHECK(CheckFlats(c_, v, dest));
            if (!hist) {
                EsxiRestoreOptions ro; ro.vmName = r.prefix + "-nohist"; ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se);
                EsxiRestoreResult res; Str rr; Pr pr2; bool rok = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr2, &res, rr); if (!rok) c_.note("restore: %s", rr.c_str());
                T_CHECK(rok); if (rok) c_.note("RESTORED %s -> %s (%s); предупреждение: %s", v.name.c_str(), res.vmName.c_str(), res.vmxPath.c_str(), res.warning.c_str());
            }
            delete t; RemoveTree(store); RemoveTree(dest);
        }
        T_CHECK(counts[0] > 0 && counts[0] < counts[1]);
    }
    return true;
}


// ВКЛЮЧЁННАЯ ВМ со СВОИМ деревом снимков (перед этим тестом vm2b-hist включают: LIVE_HIST=1 tools/esxi7/run-real.sh --only esxireal_live_hist_running): цепочка свёртывается в один клон
// на диск, чужие снимки не копируются (в предупреждении сказано), восстановленная ВМ регистрируется и включается, sha256 диска равна эталону
// (в дельтах этой ВМ без ОС ничего не записано, поэтому клон верхнего звена побитно равен базе).
VM2B_TEST("esxireal", esxireal_live_hist_running)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal-lh");
    for (const RealVm& v : r.vms) {
        if (!v.hist) continue;
        Str err; VmSource* s0 = NewEsxiSource(r.cfg(), err); T_CHECK(s0 != nullptr); if (!s0) return false;
        Vec<VmInfo> vms; T_CHECK(s0->list(vms)); bool running = false; for (const VmInfo& i : vms) if (i.name == v.name && i.state == "running") running = true; delete s0;
        if (!running) { c_.skip("ВМ со снимками выключена (LIVE_HIST=1 tools/esxi7/run-real.sh включает её)"); continue; }
        Str store = tmp.sub("s"), dest = tmp.sub("d");
        BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.codec.level = 1;
        VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
        Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr;
        bool ok = BackupVm(*src, v.name, *t, o, &pr, man, warn, err); if (!ok) c_.note("%s: %s", v.name.c_str(), err.c_str());
        T_CHECK(ok); delete src; if (!ok) { delete t; continue; }
        c_.note("%s (включена, со своими снимками): %llu байт; предупреждение: %s", v.name.c_str(), (unsigned long long)man.rawSize, warn.c_str());
        T_CHECK(Has(warn, "earlier snapshot"));
        Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
        Bytes mb; Str me, pe; Json mj; if (ReadWholeFile(JoinPath(dest, "vm2backup-meta.json").c_str(), mb, me)) Json::parse((const char*)mb.data(), mb.size(), mj, pe);
        T_CHECK(mj.get("live_clone").boolean());
        const Json& fl = mj.get("files"); int deltas = 0; for (size_t i = 0; i < fl.size(); ++i) { Str p = fl.at(i).get("path").str(); c_.note("  %s (%lld)", p.c_str(), (long long)fl.at(i).get("size").integer()); if (p.find("sesparse") >= 0 || p.find("-delta") >= 0) ++deltas; }
        T_CHECK(deltas == 0);
        T_CHECK(CheckFlats(c_, v, dest));
        EsxiRestoreOptions ro; ro.vmName = Str::fmt("%s-lh%u", r.prefix.c_str(), (unsigned)(GetTickCount() % 100000)); ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se);
        EsxiRestoreResult res; Str rr; Pr pr2; bool rok = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr2, &res, rr); if (!rok) c_.note("restore: %s", rr.c_str());
        T_CHECK(rok); if (rok) c_.note("RESTORED %s -> %s (%s)", v.name.c_str(), res.vmName.c_str(), res.vmxPath.c_str());
        delete t; RemoveTree(store); RemoveTree(dest);
    }
    return true;
}

// ---- сценарий «включённая ВМ: пауза, выключение, включение, продолжение / отмена» (этапы запускает tools/esxi7/live-scenario.sh, между этапами по SSH меняется питание ВМ) ----
static Str LiveStore(Ctx& c)
{
    Str pf = c.servers.get("esxi_real").get("password_file").str(); int cut = pf.rfind('\\'); if (cut < 0) cut = pf.rfind('/');
    return JoinPath(pf.substr(0, (size_t)cut), "live-store");
}
static const RealVm* LiveVm(const Real& r) { for (const RealVm& v : r.vms) if (v.on) return &v; return nullptr; }
static BackupOptions LiveOpts() { BackupOptions o; o.kdfIterations = 3000; o.verify = false; o.allowLive = true; o.codec.level = 1; o.segmentBytes = (u64)512 * 2000; return o; }

VM2B_TEST("esxireal", esxireal_live_stage_pause)
{
    Real r; if (!Load(c_, r)) return true; const RealVm* v = LiveVm(r); if (!v) return true;
    Str store = LiveStore(c_); RemoveTree(store);
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr; pr.abortAt = true; pr.keep = true; pr.limit = (u64)40 << 20;
    BackupOptions o = LiveOpts();
    bool ok = BackupVm(*src, v->name, *t, o, &pr, man, warn, err);
    c_.note("этап «пауза»: backup %s: %s", ok ? "готов" : "прерван", err.c_str());
    T_CHECK(!ok);
    Vec<ResumeInfo> ri; Str le; ListResumable(*t, &v->name, ri, le); c_.note("возобновляемых копий: %d (томов %d)", (int)ri.size(), ri.size() ? ri[0].volumesDone : 0);
    T_CHECK(ri.size() == 1);
    delete t; delete src;
    return true;
}

VM2B_TEST("esxireal", esxireal_live_stage_resume)
{
    Real r; if (!Load(c_, r)) return true; const RealVm* v = LiveVm(r); if (!v) return true;
    Str store = LiveStore(c_);
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr; BackupOptions o = LiveOpts();
    bool ok = BackupVm(*src, v->name, *t, o, &pr, man, warn, err);
    c_.note("этап «продолжение»: %s; возобновлений %d; ошибка: %s; предупреждение: %s", ok ? "копия готова" : "копия не удалась", pr.resumes, err.c_str(), warn.c_str());
    delete t; delete src;
    return true;
}

VM2B_TEST("esxireal", esxireal_live_stage_cancel)
{
    Real r; if (!Load(c_, r)) return true; const RealVm* v = LiveVm(r); if (!v) return true;
    Str store = LiveStore(c_);
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; BackupOptions o = LiveOpts();
    { Pr pr; pr.abortAt = true; pr.keep = true; pr.limit = (u64)40 << 20; bool ok = BackupVm(*src, v->name, *t, o, &pr, man, warn, err); T_CHECK(!ok); }
    delete src; src = NewEsxiSource(r.cfg(), err);
    { Pr pr; pr.abortAt = true; pr.keep = false; pr.limit = 1; bool ok = BackupVm(*src, v->name, *t, o, &pr, man, warn, err); c_.note("этап «отмена»: %s (%s)", ok ? "готов" : "отменён", err.c_str()); T_CHECK(!ok); }
    Vec<ResumeInfo> ri; Str le; ListResumable(*t, &v->name, ri, le); c_.note("после отмены возобновляемых копий: %d", (int)ri.size());
    delete t; delete src; RemoveTree(store);
    return true;
}

} // namespace vm2b
