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
#include "throttle.h"
#include "winutil.h"

namespace vm2b {

namespace {

// Какой сервер проверяют: "esxi_real" (сам хост) или "vcenter_real" (vCenter 7, VCSA под qemu, хост добавлен в него); одни и те же тела тестов идут в обеих группах
static thread_local const char* g_realKey = "esxi_real";
struct RealVm { Str name; bool on, hist; Vec<Str> files, hashes; };
struct Real {
    Str host, user, password, fp, prefix, dc; int port; bool vc = false; Vec<RealVm> vms;
    Json cfg(const Str& pw = Str(), bool quiesce = false) const {
        Json c = Json::object();
        c["type"] = vc ? "vcenter" : "esxi"; c["host"] = host; c["port"] = port; c["user"] = user; c["password"] = pw.empty() ? password : pw;
        if (!fp.empty()) c["fingerprint"] = fp; else c["verify"] = false;
        c["task_timeout"] = 300; c["remove_timeout"] = 600; c["timeout"] = 120;
        Json s = Json::object(); s["quiesce"] = quiesce; s["memory"] = false; c["snapshot"] = s;
        return c;
    }
};

static bool Load(Ctx& c, Real& r)
{
    if (c.servers.is_null() || !c.servers.has(g_realKey)) { c.skip((Str("нет ключа \"") + g_realKey + "\" (tools/esxi7/run-real.sh)").c_str()); return false; }
    const Json& j = c.servers.get(g_realKey); r.vc = Str(g_realKey) == "vcenter_real"; r.dc = j.get("datacenter").str();
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
    return true;
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

static bool Body_esxireal_connect_and_list(Ctx& c_)
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

static bool Body_esxireal_wrong_credentials(Ctx& c_)
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

static bool Body_esxireal_backup_verify_restore_matrix(Ctx& c_)
{
    Real r; if (!Load(c_, r)) return true;
    Tmp tmp(c_, "esxireal");
    struct Mode { const char* pw; int level; u64 seg; };
    static const Mode modes[] = { { "", 1, 0 }, { "real-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c", 5, (u64)512 * 200000 }, { "", 9, (u64)512 * 4096 }, { "x", 0, 0 } };
    int idx = 0;
    for (const RealVm& v : r.vms) {
        for (size_t mi = 0; mi < (r.vc ? 2u : 4u); ++mi) {                      // через vCenter два режима (весь набор уже прошёл на хосте)
            const Mode& m = modes[mi];
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

static bool Body_esxireal_resume_after_abort(Ctx& c_)
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
static bool Body_esxireal_restore_to_host(Ctx& c_)
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
        EsxiRestoreOptions ro; ro.vmName = Str::fmt("%s-%d", r.prefix.c_str(), n); ro.datacenter = r.dc;
        Str e1; Json info; T_CHECK(EsxiRestoreTargets(r.cfg(), r.dc, info, e1)); c_.note("варианты восстановления: %s", info.dump().c_str());
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
static bool Body_esxireal_snapshot_history(Ctx& c_)
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
                EsxiRestoreOptions ro; ro.vmName = r.prefix + "-nohist"; ro.datacenter = r.dc; ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se);
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
static bool Body_esxireal_live_hist_running(Ctx& c_)
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
        EsxiRestoreOptions ro; ro.vmName = Str::fmt("%s-lh%u", r.prefix.c_str(), (unsigned)(GetTickCount() % 100000)); ro.datacenter = r.dc; ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se);
        EsxiRestoreResult res; Str rr; Pr pr2; bool rok = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr2, &res, rr); if (!rok) c_.note("restore: %s", rr.c_str());
        T_CHECK(rok); if (rok) c_.note("RESTORED %s -> %s (%s)", v.name.c_str(), res.vmName.c_str(), res.vmxPath.c_str());
        delete t; RemoveTree(store); RemoveTree(dest);
    }
    return true;
}


// ---- те же тела тестов для самого ESXi (esxireal) и для vCenter (vcreal) ----
VM2B_TEST("esxireal", esxireal_connect_and_list) { g_realKey = "esxi_real"; return Body_esxireal_connect_and_list(c_); }
VM2B_TEST("vcreal", vcreal_connect_and_list) { g_realKey = "vcenter_real"; bool r = Body_esxireal_connect_and_list(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_wrong_credentials) { g_realKey = "esxi_real"; return Body_esxireal_wrong_credentials(c_); }
VM2B_TEST("vcreal", vcreal_wrong_credentials) { g_realKey = "vcenter_real"; bool r = Body_esxireal_wrong_credentials(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_backup_verify_restore_matrix) { g_realKey = "esxi_real"; return Body_esxireal_backup_verify_restore_matrix(c_); }
VM2B_TEST("vcreal", vcreal_backup_verify_restore_matrix) { g_realKey = "vcenter_real"; bool r = Body_esxireal_backup_verify_restore_matrix(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_resume_after_abort) { g_realKey = "esxi_real"; return Body_esxireal_resume_after_abort(c_); }
VM2B_TEST("vcreal", vcreal_resume_after_abort) { g_realKey = "vcenter_real"; bool r = Body_esxireal_resume_after_abort(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_restore_to_host) { g_realKey = "esxi_real"; return Body_esxireal_restore_to_host(c_); }
VM2B_TEST("vcreal", vcreal_restore_to_host) { g_realKey = "vcenter_real"; bool r = Body_esxireal_restore_to_host(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_snapshot_history) { g_realKey = "esxi_real"; return Body_esxireal_snapshot_history(c_); }
VM2B_TEST("vcreal", vcreal_snapshot_history) { g_realKey = "vcenter_real"; bool r = Body_esxireal_snapshot_history(c_); g_realKey = "esxi_real"; return r; }
VM2B_TEST("esxireal", esxireal_live_hist_running) { g_realKey = "esxi_real"; return Body_esxireal_live_hist_running(c_); }
VM2B_TEST("vcreal", vcreal_live_hist_running) { g_realKey = "vcenter_real"; bool r = Body_esxireal_live_hist_running(c_); g_realKey = "esxi_real"; return r; }

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


// ================= Запись гостя ВО ВРЕМЯ копии включённой ВМ (docs/ESXI.md, «Запись гостя во время копии — проверено на настоящем ESXi 7») =================
// ВМ `vm2b-guest`: Alpine Linux (live, из ОЗУ с CD) пишет «сырым» блочным писателем (tools/esxi7/guest/vm2bio.c) на диск данных: блок 4 КиБ = заголовок {seq, блок, CRC, время} +
// данные из splitmix64 от (seed, блок, seq); запись k идёт в блок, который определяется только по (seed, k), одна запись в полёте (O_DIRECT|O_DSYNC).  Поэтому ЛЮБОЙ согласованный по
// времени образ диска есть ровно «состояние после записей 1..M»: проверка ниже восстанавливает эталон для найденного M и сверяет каждый блок.  Окно M относительно момента снимка
// (хронология хоста: tools/esxi7/guest/esxi-watch.py) определяет скрипт tools/esxi7/guest-run.sh; тест отвечает за копию, восстановление и «префикс»-согласованность.
//   "esxi_guest": { "vm": "vm2b-guest", "out": "<папка результатов, путь Windows>", "tag": "<метка этапа>", "mbit": <ограничение копии>, "pause_mb": <для этапа паузы>,
//                   "restore_name": "<имя ВМ при восстановлении на хост>" }
namespace {
struct GScan { bool valid = false; u64 M = 0, blocks = 0, ok = 0, zero = 0, torn = 0, stale = 0, inflight = 0, viol = 0; Str seed; };
static const u64 kG = 0x9E3779B97F4A7C15ULL;
static u64 GFin(u64 z) { z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }
static u64 GMix(u64 x) { return GFin(x + kG); }
static u64 GKey(u64 seed, u32 idx, u64 seq) { return GMix(seed + GMix(GMix(idx) ^ GMix(seq ^ 0x5851F42D4C957F2DULL))); }
static u32 g_crct[256];
static void GCrcInit() { for (u32 i = 0; i < 256; ++i) { u32 c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crct[i] = c; } }
static u32 GCrc(u32 c, const u8* p, size_t n) { for (size_t i = 0; i < n; ++i) c = g_crct[(c ^ p[i]) & 255] ^ (c >> 8); return c; }
static void GBlockSeq(u64 seed, u64 N, u64 k, u32& idx) {
    u64 s = k >> 6;
    if ((GMix(seed ^ s) & 3) == 0) idx = (u32)(1 + (s * 64 + (k & 63)) % (N - 1)); else idx = (u32)(1 + GMix(seed ^ GMix(k)) % (N - 1));
}
// 0 пусто, 1 верный (seq), 2 испорчен
static int GCheck(u64 seed, const u8* b, u32 idx, u64& seq) {
    bool zero = true; for (int i = 0; i < 4096; ++i) if (b[i]) { zero = false; break; }
    if (zero) return 0;
    u64 s; u32 i, cr; memcpy(&s, b + 8, 8); memcpy(&i, b + 16, 4); memcpy(&cr, b + 20, 4);
    if (memcmp(b, "VM2BBLK1", 8) != 0 || i != idx) return 2;
    u32 c = GCrc(0xFFFFFFFFu, b, 20); c = ~GCrc(c, b + 32, 4096 - 32); if (c != cr) return 2;
    u64 key = GKey(seed, idx, s);
    for (int j = 0; j < (4096 - 32) / 8; ++j) { u64 w = GFin(key + (u64)(j + 1) * kG); if (memcmp(b + 32 + j * 8, &w, 8) != 0) return 2; }
    seq = s; return 1;
}
static GScan GuestScan(const Str& path)
{
    GScan r; GCrcInit();
    HANDLE h = CreateFileW(FilePath(path), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); u64 N = (u64)sz.QuadPart / 4096;
    Bytes sup; sup.resize(4096); DWORD got = 0; ReadFile(h, sup.data(), 4096, &got, nullptr);
    u64 seed = 0, n2 = 0; if (got != 4096 || memcmp(sup.data(), "VM2BSUPR", 8) != 0) { CloseHandle(h); return r; }
    memcpy(&seed, sup.data() + 8, 8); memcpy(&n2, sup.data() + 16, 8); if (n2 != N) { CloseHandle(h); return r; }
    Vec<u32> seq; seq.resize((size_t)N); Vec<u8> st; st.resize((size_t)N); Bytes buf; buf.resize(1 << 20);
    for (u64 b = 1; b < N;) {
        u64 cnt = N - b; if (cnt > 256) cnt = 256; got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)(cnt * 4096), &got, nullptr) || got != cnt * 4096) { CloseHandle(h); return r; }
        for (u64 i = 0; i < cnt; ++i) { u64 s = 0; int k = GCheck(seed, buf.data() + i * 4096, (u32)(b + i), s); st[(size_t)(b + i)] = (u8)k; if (k == 1) { seq[(size_t)(b + i)] = (u32)s; if (s > r.M) r.M = s; } }
        b += cnt;
    }
    CloseHandle(h);
    Vec<u32> last; last.resize((size_t)N); for (u64 k = 1; k <= r.M; ++k) { u32 idx; GBlockSeq(seed, N, k, idx); last[idx] = (u32)k; }
    u32 infl; GBlockSeq(seed, N, r.M + 1, infl);
    r.blocks = N - 1;
    for (u64 b = 1; b < N; ++b) {
        bool bad = false;
        if (st[(size_t)b] == 1) { if (seq[(size_t)b] == last[(size_t)b]) ++r.ok; else { ++r.stale; bad = true; } } else if (st[(size_t)b] == 0) { ++r.zero; bad = true; } else { ++r.torn; bad = true; }
        if (bad) { if (b == infl && st[(size_t)b] != 0) ++r.inflight; else ++r.viol; }
    }
    r.seed = Str::fmt("%llx", (unsigned long long)seed); r.valid = true; return r;
}
struct GuestCfg { Str vm, out, tag, restoreName; double mbit = 0; int pauseMB = 100; bool expectResume = true; };
static bool LoadGuest(Ctx& c, GuestCfg& g)
{
    if (c.servers.is_null() || !c.servers.has("esxi_guest")) { c.skip("нет ключа \"esxi_guest\" (tools/esxi7/guest-run.sh)"); return false; }
    const Json& j = c.servers.get("esxi_guest");
    g.vm = j.get("vm").str("vm2b-guest"); g.out = j.get("out").str(); g.tag = j.get("tag").str("t"); g.restoreName = j.get("restore_name").str();
    g.mbit = (double)j.get("mbit").integer(); g.pauseMB = (int)j.get("pause_mb").integer(); if (g.pauseMB <= 0) g.pauseMB = 100;
    g.expectResume = !j.has("expect_resume") || j.get("expect_resume").boolean();
    return !g.out.empty();
}
struct GPr : Pr { double tFirst = 0; void onBytes(u64 n) override { if (tFirst == 0) tFirst = UnixNow(); Pr::onBytes(n); } };
static void GuestReport(Ctx& c_, const char* tag, const GScan& s)
{
    c_.note("GUESTSCAN %s: valid=%d M=%llu blocks=%llu ok=%llu zero=%llu torn=%llu stale=%llu inflight=%llu violations=%llu seed=%s", tag, (int)s.valid, (unsigned long long)s.M,
            (unsigned long long)s.blocks, (unsigned long long)s.ok, (unsigned long long)s.zero, (unsigned long long)s.torn, (unsigned long long)s.stale, (unsigned long long)s.inflight,
            (unsigned long long)s.viol, s.seed.c_str());
}
// нет остатков на хосте: следующий begin() не находит старых снимков VM2Backup
static bool NoLeftovers(Ctx& c_, const Real& r, const Str& vm)
{
    Str e2; VmSource* s2 = NewEsxiSource(r.cfg(), e2); Str fp, tok, w2; bool b = s2 && s2->begin(vm, true, Str(), fp, tok, w2);
    T_CHECK(b); T_CHECK(!Has(w2, "removed")); if (s2) { if (b) s2->end(false); delete s2; }
    return true;
}
} // namespace

// Два режима (xz 1 без пароля; пароль + xz 5 + тома ~100 МБ); копия ограничена по скорости (BackupOptions::limiter), чтобы шла минуты, пока гость пишет.
static bool Body_esxireal_guest_write_copy(Ctx& c_)
{
    Real r; if (!Load(c_, r)) return true; GuestCfg g; if (!LoadGuest(c_, g)) return true;
    struct Mode { const char* name; const char* pw; int level; u64 seg; };
    static const Mode modes[] = { { "xz1", "", 1, 0 }, { "pw-xz5-vol100", "guest-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c", 5, (u64)512 * 200000 } };
    Tmp tmp(c_, "esxireal-gw");
    for (const Mode& m : modes) {
        Str store = tmp.sub(m.name), dest = JoinPath(g.out, m.name); RemoveTree(dest); Str me; MakeDirs(g.out, me);
        BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.password = m.pw; o.codec.level = m.level; if (m.seg) o.segmentBytes = m.seg; o.allowLive = true;
        RateLimiter lim; if (g.mbit > 0) { lim.setMbit(g.mbit); o.limiter = &lim; }
        Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
        Target* t = LocalAtR(store); Manifest man; Str warn; GPr pr;
        double t0 = UnixNow();
        c_.note("GUESTRUN %s: начало копии unix=%.3f", m.name, t0);
        bool ok = BackupVm(*src, g.vm, *t, o, &pr, man, warn, err);
        double t1 = UnixNow();
        c_.note("GUESTRUN %s: копия %s unix=%.3f, первый байт диска unix=%.3f, всего %.1f с (%.1f мин), %llu байт, в архиве %llu, томов %d, предупреждение: %s", m.name, ok ? "готова" : "НЕ УДАЛАСЬ",
                t1, pr.tFirst, t1 - t0, (t1 - t0) / 60.0, (unsigned long long)man.rawSize, (unsigned long long)man.size, (int)man.segments.size(), warn.c_str());
        if (!ok) c_.note("ошибка: %s", err.c_str());
        T_CHECK(ok);
        if (ok) {
            Str ve; T_CHECK(VerifyBackup(*t, man.archive, o.password, nullptr, nullptr, ve));
            if (!o.password.empty()) { Str e2; T_CHECK(!VerifyBackup(*t, man.archive, "wrong", nullptr, nullptr, e2)); }
            Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
            GScan s = GuestScan(JoinPath(JoinPath(dest, EsxiArchivePrefix(g.vm)), g.vm + "-flat.vmdk")); GuestReport(c_, m.name, s);
            T_CHECK(s.valid); T_CHECK(s.viol == 0); T_CHECK(s.M > 0);
        }
        delete t; delete src; RemoveTree(store);
        T_CHECK(NoLeftovers(c_, r, g.vm));
    }
    return true;
}
VM2B_TEST("esxireal", esxireal_guest_write_copy) { g_realKey = "esxi_real"; return Body_esxireal_guest_write_copy(c_); }

// Восстановление на сам ESXi из копии, снятой во время записи гостя (ВМ включает и проверяет скрипт: гость печатает «VERIFY OK seq<=S»); та же копия — и в папку для сверки M.
VM2B_TEST("esxireal", esxireal_guest_restore_to_host)
{
    Real r; if (!Load(c_, r)) return true; GuestCfg g; if (!LoadGuest(c_, g)) return true; if (g.restoreName.empty()) return true;
    Tmp tmp(c_, "esxireal-gh"); Str store = tmp.sub("store"), dest = JoinPath(g.out, "tohost-folder"); RemoveTree(dest); Str me; MakeDirs(g.out, me);
    BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.codec.level = 1; o.allowLive = true;
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; GPr pr; double t0 = UnixNow();
    bool ok = BackupVm(*src, g.vm, *t, o, &pr, man, warn, err); T_CHECK(ok); delete src;
    c_.note("GUESTRUN tohost: копия %s unix=%.3f..%.3f", ok ? "готова" : "НЕ УДАЛАСЬ", t0, UnixNow()); if (!ok) { c_.note("ошибка: %s", err.c_str()); delete t; return false; }
    Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
    GScan s = GuestScan(JoinPath(JoinPath(dest, EsxiArchivePrefix(g.vm)), g.vm + "-flat.vmdk")); GuestReport(c_, "tohost-folder", s); T_CHECK(s.valid); T_CHECK(s.viol == 0);
    EsxiRestoreOptions ro; ro.vmName = g.restoreName;
    ReadOptions rd; rd.stateDir = tmp.sub("state"); Str se; MakeDirs(rd.stateDir, se); EsxiRestoreResult res; Str rr; Pr pr2;
    bool rok = RestoreToEsxi(*t, man.archive, r.cfg(), ro, o.password, rd, &pr2, &res, rr); if (!rok) c_.note("restore на хост: %s", rr.c_str());
    T_CHECK(rok); if (rok) c_.note("RESTORED %s -> %s (%s)", g.vm.c_str(), res.vmName.c_str(), res.vmxPath.c_str());
    delete t; RemoveTree(store);
    T_CHECK(NoLeftovers(c_, r, g.vm));
    return true;
}

// Пауза посреди копии (снимок и клон остаются), затем продолжение (между ними скрипт может выключить и включить ВМ): копия = состояние на момент ПЕРВОГО снимка.
VM2B_TEST("esxireal", esxireal_guest_stage_pause)
{
    Real r; if (!Load(c_, r)) return true; GuestCfg g; if (!LoadGuest(c_, g)) return true;
    Str store = JoinPath(g.out, "store-" + g.tag); RemoveTree(store); Str me; MakeDirs(g.out, me);
    BackupOptions o; o.kdfIterations = 3000; o.verify = false; o.codec.level = 1; o.allowLive = true; o.segmentBytes = (u64)512 * 2000;
    RateLimiter lim; if (g.mbit > 0) { lim.setMbit(g.mbit); o.limiter = &lim; }
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; GPr pr; pr.abortAt = true; pr.keep = true; pr.limit = (u64)g.pauseMB << 20;
    double t0 = UnixNow(); bool ok = BackupVm(*src, g.vm, *t, o, &pr, man, warn, err); T_CHECK(!ok);
    c_.note("GUESTRUN %s: пауза после %d МБ, unix=%.3f..%.3f: %s", g.tag.c_str(), g.pauseMB, t0, UnixNow(), err.c_str());
    Vec<ResumeInfo> ri; Str le; ListResumable(*t, &g.vm, ri, le); c_.note("возобновляемых копий: %d (томов %d)", (int)ri.size(), ri.size() ? ri[0].volumesDone : 0); T_CHECK(ri.size() == 1);
    delete t; delete src; return true;
}
// Копия без паузы под управлением скрипта: пока она идёт, скрипт ВЫКЛЮЧАЕТ ВМ (hard power-off гостя посреди передачи); копия должна дойти до конца и быть состоянием на момент снимка.
VM2B_TEST("esxireal", esxireal_guest_stage_full)
{
    Real r; if (!Load(c_, r)) return true; GuestCfg g; if (!LoadGuest(c_, g)) return true;
    Tmp tmp(c_, "esxireal-gf"); Str store = tmp.sub("store"), dest = JoinPath(g.out, "restored-" + g.tag); RemoveTree(dest); Str me; MakeDirs(g.out, me);
    BackupOptions o; o.kdfIterations = 3000; o.verify = true; o.codec.level = 1; o.allowLive = true; o.segmentBytes = (u64)512 * 2000;
    RateLimiter lim; if (g.mbit > 0) { lim.setMbit(g.mbit); o.limiter = &lim; }
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; GPr pr; double t0 = UnixNow();
    c_.note("GUESTRUN %s: начало копии unix=%.3f", g.tag.c_str(), t0);
    bool ok = BackupVm(*src, g.vm, *t, o, &pr, man, warn, err); T_CHECK(ok);
    c_.note("GUESTRUN %s: копия %s unix=%.3f, первый байт диска unix=%.3f, предупреждение: %s", g.tag.c_str(), ok ? "готова" : "НЕ УДАЛАСЬ", UnixNow(), pr.tFirst, warn.c_str());
    if (!ok) c_.note("ошибка: %s", err.c_str());
    delete src;
    if (ok) {
        Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
        GScan s = GuestScan(JoinPath(JoinPath(dest, EsxiArchivePrefix(g.vm)), g.vm + "-flat.vmdk")); GuestReport(c_, g.tag.c_str(), s); T_CHECK(s.valid); T_CHECK(s.viol == 0);
    }
    delete t; T_CHECK(NoLeftovers(c_, r, g.vm));
    return true;
}
VM2B_TEST("esxireal", esxireal_guest_stage_resume)
{
    Real r; if (!Load(c_, r)) return true; GuestCfg g; if (!LoadGuest(c_, g)) return true;
    Str store = JoinPath(g.out, "store-" + g.tag), dest = JoinPath(g.out, "restored-" + g.tag); RemoveTree(dest);
    BackupOptions o; o.kdfIterations = 3000; o.verify = false; o.codec.level = 1; o.allowLive = true; o.segmentBytes = (u64)512 * 2000;
    RateLimiter lim; if (g.mbit > 0) { lim.setMbit(g.mbit); o.limiter = &lim; }
    Str err; VmSource* src = NewEsxiSource(r.cfg(), err); T_CHECK(src != nullptr); if (!src) return false;
    Target* t = LocalAtR(store); Manifest man; Str warn; Pr pr; double t0 = UnixNow();
    bool ok = BackupVm(*src, g.vm, *t, o, &pr, man, warn, err);
    c_.note("GUESTRUN %s: продолжение %s, возобновлений %d, unix=%.3f..%.3f; ошибка: %s; предупреждение: %s", g.tag.c_str(), ok ? "готово" : "НЕ УДАЛОСЬ", pr.resumes, t0, UnixNow(), err.c_str(), warn.c_str());
    T_CHECK(ok);
    // expect_resume = false: ВМ выключали и включали, пока копия стояла на паузе. ESXi при первом выключении/включении переписывает .vmx (размер меняется) -> отпечаток ВМ другой, и копия честно начинается заново с НОВЫМ снимком; при неизменном .vmx копия продолжается по прежнему снимку.
    // при expect_resume = false любой исход допустим (оба согласованы; результат разбирает analyse.py): продолжение (resumes >= 1) или честный перезапуск с новым снимком (resumes = 0)
    c_.note("возобновлений: %d", pr.resumes); if (g.expectResume) T_CHECK(pr.resumes >= 1); delete src;
    if (ok) {
        Str re; Manifest rm; T_CHECK(RestoreBackup(*t, man.archive, dest, o.password, nullptr, &rm, re));
        GScan s = GuestScan(JoinPath(JoinPath(dest, EsxiArchivePrefix(g.vm)), g.vm + "-flat.vmdk")); GuestReport(c_, g.tag.c_str(), s); T_CHECK(s.valid); T_CHECK(s.viol == 0);
    }
    delete t; RemoveTree(store);
    T_CHECK(NoLeftovers(c_, r, g.vm));
    return true;
}

} // namespace vm2b
