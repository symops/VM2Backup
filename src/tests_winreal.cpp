// Regression tests for problems that only a REAL Windows showed (docs/WINDOWS-TESTS.md); they also run under Wine, where they usually pass anyway.
#include "autotest.h"
#include "tests_svc.h"
#include "smb.h"
#include "engine.h"
#include "config.h"
#include "tar.h"
#include <winioctl.h>

namespace vm2b {

// ReadWholeFile / WriteWholeFile (base.cpp) opened paths of 260+ characters without the long-path prefix: real Windows said "path not found" (Wine does not mind).
// Found by engine/restore_destination_path_edge_cases and engine/tar_volume_boundaries_exhaustive on Windows 10 x86.
VM2B_TEST("core", whole_file_helpers_accept_long_paths)
{
    Tmp tmp(c_, "longpath");
    for (int len = 230; len <= 330; len += (len < 262 && len > 236) ? 1 : 20) {
        Str dir = tmp.path; int i = 0;
        while ((int)dir.size() < len - 12) { dir += Str("\\") + Str::fmt("d%d-", i++); for (int q = c_.range(20, 40); q > 0; --q) dir += 'l'; }
        Str file = dir + "\\f.vmdk"; Str e;
        T_CHECK(MakeDirs(dir, e));
        Bytes b = Random(c_, (size_t)c_.range(1, 5000));
        bool w = WriteWholeFile(file.c_str(), b.data(), b.size(), e); if (!w) c_.note("len %d: write: %s", (int)file.size(), e.c_str());
        T_CHECK(w);
        Bytes got; bool r = ReadWholeFile(file.c_str(), got, e); if (!r) c_.note("len %d: read: %s", (int)file.size(), e.c_str());
        T_CHECK(r && got.size() == b.size() && memcmp(got.data(), b.data(), b.size()) == 0);
        Str fwd = file.replace_all("\\", "/");                                   // forward slashes are the same path
        Bytes got2; T_CHECK(ReadWholeFile(fwd.c_str(), got2, e) && got2.size() == b.size());
        T_CHECK(FileExists(file));
    }
    return true;
}


namespace {
// Windows answers 53 for a missing share on a Samba server too (seen on Windows 10 against Samba 4.22); 53 is also "the server does not answer".
unsigned long Fake53Add(const wchar_t*, const wchar_t*, const wchar_t*) { return 53; }
unsigned long Fake67Add(const wchar_t*, const wchar_t*, const wchar_t*) { return 67; }       // Windows 11 25H2 says 67 even for a server that does not answer (after ~43 s)
unsigned long FakeCancelOk(const wchar_t*, int) { return 0; }
bool FakeProbe(const Str& host) { return host == "answers"; }
const SmbApi kFake53 = { Fake53Add, FakeCancelOk, nullptr, FakeProbe };
const SmbApi kFake67 = { Fake67Add, FakeCancelOk, nullptr, FakeProbe };
struct Fake67Scope { Fake67Scope() { SmbSetApi(&kFake67); } ~Fake67Scope() { SmbSetApi(nullptr); } };
struct Fake53Scope { Fake53Scope() { SmbSetApi(&kFake53); } ~Fake53Scope() { SmbSetApi(nullptr); } };
}

// A share that does not exist on a server that answers is a permanent error (67), not "cannot reach the server" that is retried for minutes.
VM2B_TEST("smb", missing_share_on_a_live_server_is_permanent)
{
    Fake53Scope fs;
    Json j = Json::object(); j["type"] = "smb"; j["user"] = "u"; j["password"] = "pw-secret-1";
    j["path"] = "\\\\answers\\typo"; Str e; Target* t = MakeTarget(j, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(SmbErrorCodeOf(t->err) == 67); T_CHECK(t->err.find("typo") >= 0); T_CHECK(t->err.find("pw-secret-1") < 0);
    delete t;
    j["path"] = "\\\\silent\\share"; t = MakeTarget(j, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(!t->connect()); T_CHECK(t->transient); T_CHECK(SmbErrorCodeOf(t->err) == 53);
    delete t; T_CHECK(SmbOpenConnections() == 0);
    return true;
}

// Windows 11 (25H2) answers 67 (not 53) when nothing answers on the server: that must still be a transient "cannot reach the server", not "the share was not found".
VM2B_TEST("smb", windows11_67_for_a_silent_server_is_transient)
{
    Fake67Scope fs;
    Json j = Json::object(); j["type"] = "smb"; j["user"] = "u"; j["password"] = "pw-secret-1";
    j["path"] = "\\\\silent\\share"; Str e; Target* t = MakeTarget(j, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(!t->connect()); T_CHECK(t->transient); T_CHECK(SmbErrorCodeOf(t->err) == 53); T_CHECK(t->err.find("pw-secret-1") < 0);
    delete t;
    j["path"] = "\\\\answers\\typo"; t = MakeTarget(j, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(SmbErrorCodeOf(t->err) == 67);
    delete t; T_CHECK(SmbOpenConnections() == 0);
    return true;
}


// ============================================================================================================================================
// Round 2 на настоящей Windows (docs/WINDOWS-TESTS.md): то, что имеет смысл только на NTFS с реальным менеджером блокировок и ссылками.
// Под Wine эти тесты либо пропускаются (нет mklink / реальных share-режимов), либо проходят как обычные.
// ============================================================================================================================================
namespace {

bool OnWine() { return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr; }

// Запустить cmd /c <строка> и дождаться (для mklink: нужны права администратора; тесты на госте идут от администратора)
bool RunCmd(const Str& line, Str* out = nullptr)
{
    Str full = "cmd.exe /c " + line; Wide w(full);
    Vec<wchar_t> buf; buf.resize(wcslen(w.c_str()) + 2); wcscpy(buf.data(), w.c_str());
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 30000); DWORD rc = 1; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); (void)out; return rc == 0;
}

bool MakeLink(const Str& link, const Str& target, bool junction, bool dir = true)
{
    return RunCmd(Str("mklink ") + (junction ? "/J " : dir ? "/D " : "") + "\"" + link + "\" \"" + target + "\" >nul 2>&1");
}

bool PutF(const Str& path, const void* p, size_t n)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    HANDLE h = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = n == 0 || (WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n); CloseHandle(h); return ok;
}

Target* LocalT(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }

BackupOptions RealOpts(u64 seg = 512 * 200) { BackupOptions o; o.kdfIterations = 3000; o.codec.level = 1; o.segmentBytes = seg; o.verify = true; o.retry.attempts = 3; o.retry.baseDelayMs = 50; o.retry.maxDelayMs = 200; return o; }

struct RunRes { bool ok; Str err, warn; Manifest m; double sec; };
RunRes BackupDirVm(const Str& root, const Str& vm, Target& t, const BackupOptions& o, Progress* pr = nullptr)
{
    RunRes r; VmSource* src = NewDirectorySource(root); DWORD t0 = GetTickCount();
    r.ok = BackupVm(*src, vm, t, o, pr, r.m, r.warn, r.err); r.sec = (GetTickCount() - t0) / 1000.0; delete src; return r;
}

// Все файлы под dir (относительные пути через '\'), для сравнения деревьев
void ListAll(const Str& dir, const Str& rel, Vec<Str>& out)
{
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*")), &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        Str n = FromWide(fd.cFileName), r = rel.empty() ? n : rel + "\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListAll(JoinPath(dir, n), r, out); else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool SameBytes(const Str& path, const Bytes& want) { Bytes got; Str e; return ReadWholeFile(path.c_str(), got, e) && got.size() == want.size() && (got.empty() || !memcmp(got.data(), want.data(), got.size())); }

} // namespace

// Ссылки внутри папки ВМ не разворачиваются: junction на C:\Windows, на папку снаружи, петля на саму папку, симлинки. Проверяется и то, что
// в архиве нет ничего извне, и что список ВМ не показывает ссылки (иначе копия ушла бы за пределы папки с правами службы).
VM2B_TEST("winreal", links_inside_a_vm_folder_are_not_followed)
{
    Tmp tmp(c_, "wrlinks");
    Str root = tmp.sub("vms"), vm = JoinPath(root, "vm1"), outside = tmp.sub("outside");
    Bytes disk = Random(c_, 300000), vmx = Random(c_, 900), secret = Random(c_, 2000);
    PutF(JoinPath(vm, "disk.vmdk"), disk.data(), disk.size()); PutF(JoinPath(vm, "sub\\a.vmx"), vmx.data(), vmx.size()); PutF(JoinPath(outside, "secret-outside.txt"), secret.data(), secret.size());
    if (!MakeLink(JoinPath(vm, "jout"), outside, true)) { c_.skip("mklink /J is not available here (Wine, or no rights)"); return true; }
    Str win; { wchar_t w[MAX_PATH]; GetWindowsDirectoryW(w, MAX_PATH); win = FromWide(w); }
    T_CHECK(MakeLink(JoinPath(vm, "jwin"), win, true));
    T_CHECK(MakeLink(JoinPath(vm, "loop"), vm, true));                                    // петля: ссылка на саму папку ВМ
    bool sym = MakeLink(JoinPath(vm, "slink"), outside, false, true);                     // симлинк на папку (нужна привилегия)
    bool fsym = MakeLink(JoinPath(vm, "fsl.vmdk"), JoinPath(outside, "secret-outside.txt"), false, false);
    c_.note("symlinks: dir %s, file %s", sym ? "created" : "not created", fsym ? "created" : "not created");
    // ссылка-папка ВМ в корне
    T_CHECK(MakeLink(JoinPath(root, "jvm"), outside, true));
    VmSource* ls = NewDirectorySource(root); Vec<VmInfo> vms; T_CHECK(ls->list(vms));
    bool sawLink = false, sawVm = false; u64 vmSize = 0; for (const VmInfo& v : vms) { if (v.name == "jvm") sawLink = true; if (v.name == "vm1") { sawVm = true; vmSize = v.size; } }
    T_CHECK(sawVm); T_CHECK(!sawLink); T_CHECK(vmSize == disk.size() + vmx.size());        // размер без содержимого ссылок
    delete ls;
    Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
    RunRes r = BackupDirVm(root, "vm1", *t, RealOpts());
    if (!r.ok) c_.note("backup: %s", r.err.c_str());
    T_CHECK(r.ok);
    c_.note("warning: %s", r.warn.c_str());
    T_CHECK(r.warn.find("skipped (links)") >= 0); T_CHECK(r.warn.find("jout") >= 0); T_CHECK(r.warn.find("jwin") >= 0); T_CHECK(r.warn.find("loop") >= 0);
    T_CHECK(r.m.rawSize < disk.size() + vmx.size() + 200000);                              // C:\Windows (гигабайты) не попал в архив
    Str dest = tmp.sub("restored"), re; T_CHECK(RestoreBackup(*t, r.m.archive, dest, "", nullptr, nullptr, re));
    Vec<Str> files; ListAll(dest, "", files); int n = 0;
    for (const Str& f : files) { c_.note("restored: %s", f.c_str()); if (f.find("secret-outside") >= 0 || f.find("jout") >= 0 || f.find("jwin") >= 0 || f.find("loop") >= 0 || f.find("slink") >= 0) T_CHECK(false); if (f.find("vm2backup-meta") < 0) ++n; }
    T_CHECK(n == 2);
    T_CHECK(SameBytes(JoinPath(dest, "vm1\\disk.vmdk"), disk)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\sub\\a.vmx"), vmx));
    // сама папка ВМ - ссылка (имя задали вручную / оно осталось в задаче): отказ с понятным текстом, копия ничего не читает
    RunRes r2 = BackupDirVm(root, "jvm", *t, RealOpts());
    c_.note("link as a VM: ok=%d [%s]", r2.ok ? 1 : 0, r2.err.c_str());
    T_CHECK(!r2.ok); T_CHECK(r2.err.find("link") >= 0);
    delete t;
    // ссылки удаляются как ссылки (RemoveTree не заходит внутрь), содержимое снаружи цело
    RemoveTree(JoinPath(vm, "jout")); RemoveTree(JoinPath(vm, "jwin")); RemoveTree(JoinPath(vm, "loop")); RemoveTree(JoinPath(root, "jvm")); RemoveTree(JoinPath(vm, "slink")); DeleteFileW(FilePath(JoinPath(vm, "fsl.vmdk")));
    T_CHECK(SameBytes(JoinPath(outside, "secret-outside.txt"), secret));
    return true;
}

// Имена файлов, которые Windows не восстановит (зарезервированное устройство, точка / пробел в конце, создаются только через \\?\), не должны давать
// "успешную" копию, которую потом нельзя восстановить: отказ при начале копии, с именем файла.
VM2B_TEST("winreal", source_names_windows_cannot_restore_are_refused)
{
    Tmp tmp(c_, "wrnames");
    Str root = tmp.sub("vms"); Str vm = JoinPath(root, "vm1");
    Bytes d = Random(c_, 1000); PutF(JoinPath(vm, "ok.vmdk"), d.data(), d.size());
    const char* bad[] = { "trail.", "trail2 ", "NUL", "con.txt", "COM1" };
    for (const char* b : bad) {
        Str full = "\\\\?\\" + JoinPath(vm, b);
        HANDLE h = CreateFileW(Wide(full).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { c_.note("cannot create %s here (%lu): skipped", b, GetLastError()); continue; }
        CloseHandle(h);
        Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
        RunRes r = BackupDirVm(root, "vm1", *t, RealOpts());
        c_.note("%s: ok=%d [%s]", b, r.ok ? 1 : 0, r.err.c_str());
        T_CHECK(!r.ok); T_CHECK(r.err.find("could not be restored") >= 0); T_CHECK(r.err.find(b) >= 0);
        Vec<Manifest> list; Str le; T_CHECK(ListBackups(*t, nullptr, list, le)); T_CHECK(list.empty());
        delete t;
        DeleteFileW(Wide(full).c_str());
    }
    // и обычная ВМ из той же папки снова копируется
    Target* t = LocalT(tmp.sub("store2")); T_CHECK(t && t->connect()); if (!t) return false;
    RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); T_CHECK(r.ok); delete t;
    return true;
}

namespace {
struct Holder { HANDLE h; DWORD delayMs; HANDLE th; };
DWORD WINAPI ReleaseLater(LPVOID p) { Holder* x = (Holder*)p; Sleep(x->delayMs); CloseHandle(x->h); x->h = INVALID_HANDLE_VALUE; return 0; }
}

// Блокировки файла-источника другой программой: что делает копия - решено и проверяется.
//   * держат на чтение/запись с общим чтением (редактор, ВМ в Workstation) - файл копируется;
//   * держат монопольно, но отпускают за ~2 с (антивирус) - копия ждёт и успевает;
//   * держат монопольно дольше ~8 с - копия НЕ пропускает файл молча: ошибка с именем файла, манифеста нет;
//   * заблокирован диапазон байтов (LockFile) - то же: ошибка с именем файла.
VM2B_TEST("winreal", source_file_locks_have_deliberate_behaviour)
{
    Tmp tmp(c_, "wrlock");
    Str root = tmp.sub("vms"), vm = JoinPath(root, "vm1");
    Bytes a = Random(c_, 200000), b = Random(c_, 5000);
    PutF(JoinPath(vm, "a.vmdk"), a.data(), a.size()); PutF(JoinPath(vm, "b.vmx"), b.data(), b.size());
    Str ap = JoinPath(vm, "a.vmdk");
    Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
    // 1. запись с общим чтением
    { HANDLE h = CreateFileW(FilePath(ap), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE);
      RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); c_.note("share-read holder: ok=%d [%s]", r.ok ? 1 : 0, r.err.c_str()); T_CHECK(r.ok);
      CloseHandle(h);
      if (r.ok) { Str dest = tmp.sub("r1"), re; T_CHECK(RestoreBackup(*t, r.m.archive, dest, "", nullptr, nullptr, re)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\a.vmdk"), a)); } }
    // 2. монопольно и отпускают через 2 с
    { Holder hd; hd.h = CreateFileW(FilePath(ap), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(hd.h != INVALID_HANDLE_VALUE);
      hd.delayMs = 2000; hd.th = CreateThread(nullptr, 0, ReleaseLater, &hd, 0, nullptr);
      RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); c_.note("exclusive 2 s: ok=%d after %.1f s [%s]", r.ok ? 1 : 0, r.sec, r.err.c_str());
      T_CHECK(r.ok); T_CHECK(r.sec >= 1.5); WaitForSingleObject(hd.th, 10000); CloseHandle(hd.th);
      if (r.ok) { Str dest = tmp.sub("r2"), re; T_CHECK(RestoreBackup(*t, r.m.archive, dest, "", nullptr, nullptr, re)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\a.vmdk"), a)); } }
    // 3. монопольно и не отпускают
    { Vec<Manifest> before; Str le; T_CHECK(ListBackups(*t, nullptr, before, le));
      HANDLE h = CreateFileW(FilePath(ap), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE);
      RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); c_.note("exclusive for good: ok=%d after %.1f s [%s]", r.ok ? 1 : 0, r.sec, r.err.c_str());
      CloseHandle(h);
      T_CHECK(!r.ok); T_CHECK(r.err.find("a.vmdk") >= 0); T_CHECK(r.err.find("another program") >= 0); T_CHECK(r.sec < 60);
      Vec<Manifest> after; T_CHECK(ListBackups(*t, nullptr, after, le)); T_CHECK(after.size() == before.size()); }
    // 4. заблокирован диапазон байтов
    { HANDLE h = CreateFileW(FilePath(ap), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE);
      T_CHECK(LockFile(h, 1000, 0, 5000, 0));
      RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); c_.note("byte range locked: ok=%d after %.1f s [%s]", r.ok ? 1 : 0, r.sec, r.err.c_str());
      UnlockFile(h, 1000, 0, 5000, 0); CloseHandle(h);
      if (!OnWine()) { T_CHECK(!r.ok); T_CHECK(r.err.find("a.vmdk") >= 0); } }                  // Wine не знает блокировок байтов
    // после всего копия снова проходит
    { RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); T_CHECK(r.ok); }
    delete t;
    return true;
}

namespace {
// "Антивирус": ходит по папке назначения и берёт каждый файл МОНОПОЛЬНО на ~150 мс (то, что делает сканер на запись / переименование)
struct Scanner {
    Str dir; volatile LONG stop; HANDLE th; volatile LONG hits;
    static DWORD WINAPI Run(LPVOID p) {
        Scanner* s = (Scanner*)p;
        while (!s->stop) {
            WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW(FilePath(JoinPath(s->dir, "*")), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    HANDLE h = CreateFileW(FilePath(JoinPath(s->dir, FromWide(fd.cFileName))), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) { InterlockedIncrement(&s->hits); Sleep(150); CloseHandle(h); }
                } while (!s->stop && FindNextFileW(f, &fd));
                FindClose(f);
            }
            Sleep(5);
        }
        return 0;
    }
    Scanner(const Str& d) : dir(d), stop(0), th(nullptr), hits(0) { th = CreateThread(nullptr, 0, Run, this, 0, nullptr); }
    ~Scanner() { InterlockedExchange(&stop, 1); WaitForSingleObject(th, 10000); CloseHandle(th); }
};
}

// Антивирус-подобный процесс держит файлы в папке назначения монопольно (в том числе новые тома и манифест): запись, переименование .part, проверка и
// восстановление должны переждать нарушения совместного доступа на настоящем NTFS.
VM2B_TEST("winreal", destination_files_held_by_a_scanner_are_waited_for)
{
    Tmp tmp(c_, "wrscan");
    Str root = tmp.sub("vms"), vm = JoinPath(root, "vm1");
    Bytes d = Random(c_, 1500000), e2 = Random(c_, 7000); PutF(JoinPath(vm, "d.vmdk"), d.data(), d.size()); PutF(JoinPath(vm, "e.vmx"), e2.data(), e2.size());
    Str store = tmp.sub("store");
    Target* t = LocalT(store); T_CHECK(t && t->connect()); if (!t) return false;
    BackupOptions o = RealOpts(512 * 600); o.retry.attempts = 40; o.retry.baseDelayMs = 100; o.retry.maxDelayMs = 400;
    Scanner sc(store);
    RunRes r = BackupDirVm(root, "vm1", *t, o);
    c_.note("backup with a scanner: ok=%d after %.1f s, %ld exclusive grabs [%s]", r.ok ? 1 : 0, r.sec, (long)sc.hits, r.err.c_str());
    T_CHECK(r.ok);
    if (r.ok) {
        ReadOptions ro; ro.retry = o.retry; Str ve; bool v = VerifyBackup(*t, r.m.archive, "", ro, nullptr, nullptr, ve); if (!v) c_.note("verify: %s", ve.c_str()); T_CHECK(v);
        Str dest = tmp.sub("restored"), re; bool rs = RestoreBackup(*t, r.m.archive, dest, "", ro, nullptr, nullptr, re); if (!rs) c_.note("restore: %s", re.c_str()); T_CHECK(rs);
        if (rs) { T_CHECK(SameBytes(JoinPath(dest, "vm1\\d.vmdk"), d)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\e.vmx"), e2)); }
    }
    T_CHECK(sc.hits > 0);                                                                    // сканер действительно успевал брать файлы
    delete t;
    return true;
}

// Юникод, пробелы, эмодзи, очень длинные пути и короткие имена 8.3 на настоящем NTFS; альтернативные потоки данных (file:stream) в архив не попадают.
VM2B_TEST("winreal", unicode_long_names_short_names_and_streams_on_ntfs)
{
    Tmp tmp(c_, "wrunicode");
    Str root = tmp.sub("vms");
    Str vmName = "\xd0\x92\xd0\x9c \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x92\xbe  two  spaces";    // "ВМ привет 日本語 💾  two  spaces"
    Str vm = JoinPath(root, vmName);
    Vec<Str> rels; Vec<Bytes> data;
    auto add = [&](const Str& rel, size_t n) { Bytes b = Random(c_, n); T_CHECK(PutF(JoinPath(vm, rel), b.data(), b.size())); rels.push_back(rel); data.push_back(b); };
    add("\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba 1.vmdk", 120000);                                    // "диск 1.vmdk"
    add("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\\\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab.vmx", 3000);
    add("emoji-\xf0\x9f\x98\x80\xf0\x9f\x92\xbe.log", 2500);
    add("a b   c  .txt", 100); add(".hidden-name", 10); add("UPPER.VMDK", 400);
    add("LongFileNameForShortNameTest.vmdk", 30000);                                           // получает имя 8.3 (LONGFI~1.VMD), если оно включено на томе
    Str deep; for (int i = 0; i < 5; ++i) { deep += Str::fmt("level%d-", i); for (int q = 0; q < 45; ++q) deep += (char)('a' + q % 26); deep += "\\"; }
    add(deep + "\xd0\xb3\xd0\xbb\xd1\x83\xd0\xb1\xd0\xbe\xd0\xba\xd0\xbe.vmdk", 5000);       // путь заметно длиннее 260 знаков
    c_.note("deepest path: %d chars", (int)JoinPath(vm, rels.back()).size());
    // альтернативный поток у настоящего файла
    if (!OnWine()) { Str ads = JoinPath(vm, "emoji-\xf0\x9f\x98\x80\xf0\x9f\x92\xbe.log") + ":secret-stream"; HANDLE h = CreateFileW(FilePath(ads), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, "STREAM-DATA-MARKER", 18, &w, nullptr); CloseHandle(h); } else c_.note("no alternate streams on this volume"); }
    wchar_t sh[MAX_PATH]; DWORD sn = GetShortPathNameW(FilePath(JoinPath(vm, "LongFileNameForShortNameTest.vmdk")), sh, MAX_PATH);
    c_.note("8.3 name: %s", sn ? FromWide(sh).c_str() : "(none: the volume has 8.3 names off)");
    Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
    RunRes r = BackupDirVm(root, vmName, *t, RealOpts(512 * 100)); if (!r.ok) c_.note("backup: %s", r.err.c_str()); T_CHECK(r.ok); if (!r.ok) { delete t; return false; }
    Str dest = tmp.sub("restored"), re; bool rs = RestoreBackup(*t, r.m.archive, dest, "", nullptr, nullptr, re); if (!rs) c_.note("restore: %s", re.c_str()); T_CHECK(rs);
    for (size_t i = 0; i < rels.size(); ++i) { bool same = SameBytes(JoinPath(JoinPath(dest, vmName), rels[i]), data[i]); if (!same) c_.note("differs: %s", rels[i].c_str()); T_CHECK(same); }
    Vec<Str> files; ListAll(dest, "", files); int n = 0;
    for (const Str& f : files) { if (f.find('~') >= 0) { c_.note("short name leaked: %s", f.c_str()); T_CHECK(false); } if (f.find("vm2backup-meta") < 0) ++n; }
    T_CHECK(n == (int)rels.size());
    // потока в восстановленной копии нет
    { Str p = JoinPath(JoinPath(dest, vmName), "emoji-\xf0\x9f\x98\x80\xf0\x9f\x92\xbe.log"); WIN32_FIND_STREAM_DATA sd; HANDLE fs = FindFirstStreamW(FilePath(p), FindStreamInfoStandard, &sd, 0); int streams = 0;
      if (fs != INVALID_HANDLE_VALUE) { do ++streams; while (FindNextStreamW(fs, &sd)); FindClose(fs); }
      c_.note("streams in the restored file: %d", streams); T_CHECK(streams <= 1); }
    delete t;
    return true;
}

// Атрибуты и время: что возвращается при восстановлении (решено: время записи сохраняется с точностью до секунды; атрибуты только чтения / скрытый / системный
// НЕ восстанавливаются - файл ВМ с такими атрибутами был бы нечитаем для программ ВМ; а уже существующий файл только для чтения в папке назначения
// не перезаписывается молча: понятная ошибка).
VM2B_TEST("winreal", attributes_and_times_after_restore)
{
    Tmp tmp(c_, "wrattr");
    Str root = tmp.sub("vms"), vm = JoinPath(root, "vm1");
    Bytes a = Random(c_, 5000), b = Random(c_, 6000), c2 = Random(c_, 7000);
    PutF(JoinPath(vm, "ro.vmx"), a.data(), a.size()); PutF(JoinPath(vm, "hid.vmx"), b.data(), b.size()); PutF(JoinPath(vm, "sys.vmx"), c2.data(), c2.size());
    SetFileAttributesW(FilePath(JoinPath(vm, "ro.vmx")), FILE_ATTRIBUTE_READONLY); SetFileAttributesW(FilePath(JoinPath(vm, "hid.vmx")), FILE_ATTRIBUTE_HIDDEN); SetFileAttributesW(FilePath(JoinPath(vm, "sys.vmx")), FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN);
    SYSTEMTIME st; memset(&st, 0, sizeof(st)); st.wYear = 2023; st.wMonth = 3; st.wDay = 14; st.wHour = 9; st.wMinute = 26; st.wSecond = 53; FILETIME ft; SystemTimeToFileTime(&st, &ft);
    { HANDLE h = CreateFileW(FilePath(JoinPath(vm, "hid.vmx")), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE); SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h); }
    Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
    RunRes r = BackupDirVm(root, "vm1", *t, RealOpts()); if (!r.ok) c_.note("backup: %s", r.err.c_str()); T_CHECK(r.ok); if (!r.ok) { delete t; return false; }
    Str dest = tmp.sub("restored"), re; T_CHECK(RestoreBackup(*t, r.m.archive, dest, "", nullptr, nullptr, re));
    T_CHECK(SameBytes(JoinPath(dest, "vm1\\ro.vmx"), a)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\hid.vmx"), b)); T_CHECK(SameBytes(JoinPath(dest, "vm1\\sys.vmx"), c2));
    DWORD ra = GetFileAttributesW(FilePath(JoinPath(dest, "vm1\\ro.vmx"))), ha = GetFileAttributesW(FilePath(JoinPath(dest, "vm1\\hid.vmx"))), sa = GetFileAttributesW(FilePath(JoinPath(dest, "vm1\\sys.vmx")));
    c_.note("restored attributes: ro=0x%lx hidden=0x%lx system+hidden=0x%lx (not restored by design)", (unsigned long)ra, (unsigned long)ha, (unsigned long)sa);
    T_CHECK(!(ra & FILE_ATTRIBUTE_READONLY)); T_CHECK(!(ha & FILE_ATTRIBUTE_HIDDEN)); T_CHECK(!(sa & FILE_ATTRIBUTE_SYSTEM));
    // время записи: 2023-03-14 09:26:53 UTC, допуск 2 с (NTFS 100 нс, tar - секунды)
    { HANDLE h = CreateFileW(FilePath(JoinPath(dest, "vm1\\hid.vmx")), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); FILETIME w; T_CHECK(h != INVALID_HANDLE_VALUE && GetFileTime(h, nullptr, nullptr, &w)); CloseHandle(h);
      long long dt = (long long)(((unsigned long long)w.dwHighDateTime << 32 | w.dwLowDateTime) / 10000000ull) - (long long)(((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull);
      c_.note("mtime difference: %lld s", dt); T_CHECK(dt >= -2 && dt <= 2); }
    // существующий файл только для чтения в папке назначения: восстановление либо заменяет его, либо останавливается с ПОНЯТНОЙ ошибкой (молча пропускать нельзя)
    { Str d2 = tmp.sub("restored2"); PutF(JoinPath(d2, "vm1\\ro.vmx"), "old", 3); SetFileAttributesW(FilePath(JoinPath(d2, "vm1\\ro.vmx")), FILE_ATTRIBUTE_READONLY);
      Str e2; bool ok = RestoreBackup(*t, r.m.archive, d2, "", nullptr, nullptr, e2); c_.note("restore over a read-only file: ok=%d [%s]", ok ? 1 : 0, e2.c_str());
      if (ok) T_CHECK(SameBytes(JoinPath(d2, "vm1\\ro.vmx"), a)); else { T_CHECK(e2.find("ro.vmx") >= 0); T_CHECK(e2.find("denied") >= 0 || e2.find("Access") >= 0); }
      SetFileAttributesW(FilePath(JoinPath(d2, "vm1\\ro.vmx")), FILE_ATTRIBUTE_NORMAL); }
    delete t;
    return true;
}

// Конфигурация, которую служба не может прочитать (чужая учётная запись, файл занят), НЕ перезаписывается пустой: запуск службы отказывает с понятным текстом.
VM2B_TEST("winreal", unreadable_config_is_never_replaced_by_an_empty_one)
{
    Tmp tmp(c_, "wrcfg");
    Str dir = tmp.sub("data");
    { Config cfg; Str e; T_CHECK(cfg.open(dir, e)); }
    Str cp = JoinPath(dir, "config.json");
    Bytes before; Str e0; T_CHECK(ReadWholeFile(cp.c_str(), before, e0));
    // файл занят монопольно (на Windows читатель с FILE_SHARE_READ тоже не пройдёт, пока писатель держит файл без общего доступа)
    HANDLE h = CreateFileW(FilePath(cp), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { c_.skip("cannot hold config.json exclusively here"); return true; }
    { Config cfg; Str e; bool ok = cfg.open(dir, e); c_.note("open with an unreadable config.json: ok=%d [%s]", ok ? 1 : 0, e.c_str()); T_CHECK(!ok); T_CHECK(e.find("config.json") >= 0); T_CHECK(e.find("untouched") >= 0); }
    CloseHandle(h);
    Bytes after; T_CHECK(ReadWholeFile(cp.c_str(), after, e0)); T_CHECK(after.size() == before.size() && !memcmp(after.data(), before.data(), before.size()));
    { Config cfg; Str e; T_CHECK(cfg.open(dir, e)); }                                         // отпустили - открывается как раньше
    return true;
}

// Пароль, который не удалось расшифровать (конфигурацию перенесли на другой компьютер), даёт понятное сообщение, а не "неверный пароль" у сервера.
VM2B_TEST("winreal", undecryptable_saved_password_gives_a_clear_message)
{
    Json j = Json::object(); j["type"] = "smb"; j["path"] = "\\\\host\\share"; j["user"] = "u"; j["password"] = "dpapi:AAAA-not-a-blob";
    Str e; Target* t = MakeTarget(j, e); T_CHECK(t == nullptr); T_CHECK(e.find("cannot be decrypted") >= 0); T_CHECK(e.find("AAAA") < 0);
    delete t;
    Json s = Json::object(); s["type"] = "directory"; s["path"] = "\\\\host\\share"; s["user"] = "u"; s["password"] = "dpapi:AAAA-not-a-blob";
    Str e2; VmSource* src = MakeSource(s, e2); T_CHECK(src == nullptr); T_CHECK(e2.find("cannot be decrypted") >= 0);
    delete src;
    // правильный пароль (в открытом виде) по-прежнему принимается
    j["password"] = "plain-password-1"; Str e3; Target* t2 = MakeTarget(j, e3); T_CHECK(t2 != nullptr); delete t2;
    return true;
}

// Область DPAPI: секреты защищены с областью "компьютер" (CRYPTPROTECT_LOCAL_MACHINE): их читает любая учётная запись этого компьютера. Поэтому смена
// учётной записи службы секреты НЕ теряет (на госте проверяется скриптом svcacct.ps1), а закрыть их от других пользователей должны права на папку данных.
// Здесь: защита и снятие защиты в одном процессе работают, чужой / повреждённый блок отклоняется без падения.
VM2B_TEST("winreal", dpapi_machine_scope_round_trip_and_damaged_blobs)
{
    Str p = Config::Protect("S3cr3t-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c"); T_CHECK(p.starts_with("dpapi:")); T_CHECK(p.find("S3cr3t") < 0);
    Str plain; T_CHECK(Config::Unprotect(p, plain)); T_CHECK(plain == "S3cr3t-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c");
    Str dmg = p; dmg[dmg.size() / 2] = dmg[dmg.size() / 2] == 'A' ? 'B' : 'A'; Str out; if (!OnWine()) T_CHECK(!Config::Unprotect(dmg, out) || out != plain);   // Wine не проверяет целостность блока
    T_CHECK(!Config::Unprotect("dpapi:", out) || out.empty()); T_CHECK(!Config::Unprotect("dpapi:!!!!", out));
    return true;
}

// Очень большой разреженный файл (6 ГиБ) через весь конвейер: xz уровня 1, проверка, восстановление; пик памяти не должен расти с размером файла.
// Группа winbig: тяжёлая, запускается отдельно (нужно ~8 ГиБ свободного места и 5-10 минут).
VM2B_TEST("winbig", sparse_6gib_vmdk_whole_pipeline)
{
    if (OnWine()) { c_.skip("needs NTFS sparse files"); return true; }
    Tmp tmp(c_, "wrsparse");
    const u64 kSize = (u64)6 << 30;
    ULARGE_INTEGER freeB; if (!GetDiskFreeSpaceExW(Wide(tmp.path).c_str(), &freeB, nullptr, nullptr) || freeB.QuadPart < (u64)9 << 30) { c_.skip("less than 9 GiB free"); return true; }
    Str root = tmp.sub("vms"), vm = JoinPath(root, "bigvm"), f = JoinPath(vm, "disk-flat.vmdk");
    Str e; MakeDirs(vm, e);
    HANDLE h = CreateFileW(FilePath(f), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(h != INVALID_HANDLE_VALUE); if (h == INVALID_HANDLE_VALUE) return false;
    DWORD br; T_CHECK(DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr));
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)kSize; T_CHECK(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)); T_CHECK(SetEndOfFile(h));
    Bytes head = Random(c_, 1 << 20), mid = Random(c_, 1 << 20), tail = Random(c_, 1 << 20);
    LONGLONG offs[3] = { 0, (LONGLONG)(kSize / 2 + 12345), (LONGLONG)kSize - (1 << 20) }; const Bytes* blk[3] = { &head, &mid, &tail };
    for (int i = 0; i < 3; ++i) { pos.QuadPart = offs[i]; SetFilePointerEx(h, pos, nullptr, FILE_BEGIN); DWORD w; T_CHECK(WriteFile(h, blk[i]->data(), (DWORD)blk[i]->size(), &w, nullptr) && w == blk[i]->size()); }
    CloseHandle(h);
    Bytes small = Random(c_, 4000); PutF(JoinPath(vm, "disk.vmdk"), small.data(), small.size());
    Target* t = LocalT(tmp.sub("store")); T_CHECK(t && t->connect()); if (!t) return false;
    BackupOptions o = RealOpts((u64)256 << 20); o.kdfIterations = 3000; o.codec.threads = 0;
    RunRes r = BackupDirVm(root, "bigvm", *t, o); if (!r.ok) c_.note("backup: %s", r.err.c_str());
    c_.note("backup of 6 GiB sparse: ok=%d %.0f s, stored %llu bytes in %d volumes", r.ok ? 1 : 0, r.sec, (unsigned long long)r.m.size, (int)r.m.segments.size());
    T_CHECK(r.ok); if (!r.ok) { delete t; return false; }
    T_CHECK(r.m.rawSize >= kSize);
    // пик памяти процесса (рабочий набор): на госте с 2-4 ГБ ОЗУ копия 6 ГиБ не должна приближаться к размеру файла
    struct PMC { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, a, b, cc, d, e, PagefileUsage, PeakPagefileUsage; } pmc = {}; pmc.cb = sizeof(pmc);
    typedef BOOL (WINAPI *Fn)(HANDLE, PMC*, DWORD); Fn gp = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo");
    double peak = (gp && gp(GetCurrentProcess(), &pmc, sizeof(pmc))) ? (double)pmc.PeakWorkingSetSize / (1 << 20) : 0;
    c_.note("peak working set after the backup: %.0f MiB", peak); T_CHECK(peak == 0 || peak < 1200);
    ReadOptions ro; Str ve; T_CHECK(VerifyBackup(*t, r.m.archive, "", ro, nullptr, nullptr, ve)); if (!ve.empty()) c_.note("verify: %s", ve.c_str());
    Str dest = tmp.sub("restored"), re; bool rs = RestoreBackup(*t, r.m.archive, dest, "", ro, nullptr, nullptr, re); if (!rs) c_.note("restore: %s", re.c_str()); T_CHECK(rs);
    if (rs) {
        Str rf = JoinPath(dest, "bigvm\\disk-flat.vmdk");
        HANDLE g = CreateFileW(FilePath(rf), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); T_CHECK(g != INVALID_HANDLE_VALUE);
        if (g != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; GetFileSizeEx(g, &sz); T_CHECK((u64)sz.QuadPart == kSize);
            for (int i = 0; i < 3; ++i) { pos.QuadPart = offs[i]; SetFilePointerEx(g, pos, nullptr, FILE_BEGIN); Bytes got; got.resize(blk[i]->size()); DWORD rd; T_CHECK(ReadFile(g, got.data(), (DWORD)got.size(), &rd, nullptr) && rd == got.size() && !memcmp(got.data(), blk[i]->data(), got.size())); }
            CloseHandle(g);
        }
        T_CHECK(SameBytes(JoinPath(dest, "bigvm\\disk.vmdk"), small));
    }
    delete t;
    RemoveTree(tmp.path);
    return true;
}

} // namespace vm2b
