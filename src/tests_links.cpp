// Ревью безопасности F6 и F10: ссылки (символические ссылки, junction, жёсткие ссылки) в папке назначения восстановления и в папке виртуальной машины.
// Под Wine символическая ссылка - настоящая ссылка Linux: проверяется, что код отвергает ссылку на всех путях.  Настоящие junction и права NTFS
// проверяет только гостевая фаза на Windows (.winvm/guest/links.ps1, см. docs/WINDOWS-TESTS.md): под Wine они не воспроизводятся.
#include "autotest.h"
#include "engine.h"
#include "winutil.h"

namespace vm2b {

namespace {

struct LTmp {
    Str path; bool keep;
    LTmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000), (unsigned long long)GetTickCount64()));      // (never the same name twice: Wine cannot always remove a link to a folder)
        RemoveTree(path); Str e; MakeDirs(path, e);
    }
    ~LTmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { Str p = JoinPath(path, n); Str e; MakeDirs(p, e); return p; }
};

Bytes Data(Ctx& c, size_t n) { Bytes b; b.resize(n); c.fill(b.data(), n); return b; }
void Put(const Str& path, const Bytes& b) { int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e); WriteWholeFile(path.c_str(), b.data(), b.size(), e); }
bool Get(const Str& path, Bytes& b) { Str e; return ReadWholeFile(path.c_str(), b, e, 1 << 28); }
bool Same(const Str& path, const Bytes& want) { Bytes b; return Get(path, b) && b.size() == want.size() && (want.empty() || memcmp(b.data(), want.data(), want.size()) == 0); }

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
// A symbolic link.  Wine's own CreateSymbolicLinkW makes a link that Wine itself cannot see, so under Wine a real Linux link is made with `ln -s` (start /unix) -
// then Wine reports it the way Windows reports a link (a folder link has the reparse attribute, the final path of a file link is its target's).  On Windows: CreateSymbolicLinkW.
bool MakeLink(const Str& link, const Str& target, bool dir, bool junction = false)
{
    typedef char* (*UnixName)(const wchar_t*);
    HMODULE k = GetModuleHandleW(L"kernel32.dll"); UnixName fn = k ? (UnixName)(void*)GetProcAddress(k, "wine_get_unix_file_name") : nullptr;
    if (!fn && dir && junction) {                                                      // a real junction (Windows): mklink /J needs no privilege
        Str cmd = "cmd.exe /c mklink /J \"" + link + "\" \"" + target + "\" > NUL";
        STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi; Wide wc(cmd); Vec<wchar_t> buf; for (const wchar_t* q = wc.c_str(); *q; ++q) buf.push_back(*q); buf.push_back(0);
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, 20000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        return GetFileAttributesW(FilePath(link)) != INVALID_FILE_ATTRIBUTES;
    }
    if (!fn) return CreateSymbolicLinkW(FilePath(link), FilePath(target), (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    int cut = link.rfind('\\'); if (cut < 0) return false;
    char* up = fn(Wide(link.substr(0, (size_t)cut))); char* ut = fn(Wide(target)); if (!up || !ut) return false;
    Str cmd = Str("C:\\windows\\system32\\start.exe /wait /unix /bin/ln -sfn ") + ut + " " + up + "/" + link.substr((size_t)cut + 1);
    HeapFree(GetProcessHeap(), 0, up); HeapFree(GetProcessHeap(), 0, ut);
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi; Wide wc(cmd); Vec<wchar_t> buf; for (const wchar_t* q = wc.c_str(); *q; ++q) buf.push_back(*q); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 20000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    for (int i = 0; i < 60; ++i) { if (GetFileAttributesW(FilePath(link)) != INVALID_FILE_ATTRIBUTES) return true; Sleep(100); }      // start /unix returns before ln has finished
    return false;
}
bool SymDir(const Str& link, const Str& target) { return MakeLink(link, target, true) || MakeLink(link, target, true, true); }      // (no privilege for symbolic links: a junction does as a folder link)
bool Junction(const Str& link, const Str& target) { return MakeLink(link, target, true, true); }
bool OnWine() { HMODULE n = GetModuleHandleW(L"ntdll.dll"); return n && GetProcAddress(n, "wine_get_version") != nullptr; }
bool SymFile(const Str& link, const Str& target) { return MakeLink(link, target, false); }
bool HardLink(const Str& link, const Str& target) { return CreateHardLinkW(FilePath(link), FilePath(target), nullptr) != 0; }

Target* LLocal(const Str& path) { Json tc = Json::object(); tc["type"] = "local"; tc["path"] = path; Str e; return MakeTarget(tc, e); }
RetryPolicy LFast() { RetryPolicy r; r.attempts = 1; r.baseDelayMs = 1; r.maxDelayMs = 2; return r; }

struct Fx {                    // a VM with three files, backed up unencrypted into <root>\store
    Str root, store, archive; Bytes disk, vmx, sub;
    bool make(Ctx& c, LTmp& t) {
        root = t.sub("vms"); store = t.sub("store");
        disk = Data(c, 300000); vmx = Data(c, 3000); sub = Data(c, 70000);
        Put(JoinPath(root, "vm\\disk.vmdk"), disk); Put(JoinPath(root, "vm\\a.vmx"), vmx); Put(JoinPath(root, "vm\\sub\\b.vmdk"), sub);
        Target* tg = LLocal(store); VmSource* src = NewDirectorySource(root);
        BackupOptions o; o.codec.level = 1; o.verify = false; o.retry = LFast(); o.segmentBytes = 512 * 100;      // 51 KiB volumes: the restore saves its state at the volume borders
        Manifest m; Str w, err; bool ok = BackupVm(*src, "vm", *tg, o, nullptr, m, w, err);
        delete src; delete tg; archive = m.archive; return ok;
    }
    bool restore(const Str& dest, Str& err, const Str& stateDir = Str(), Progress* pr = nullptr) {
        Target* tg = LLocal(store); ReadOptions ro; ro.retry = LFast(); ro.stateDir = stateDir;
        bool ok = RestoreBackup(*tg, archive, dest, "", ro, pr, nullptr, err); delete tg; return ok;
    }
};

} // namespace

// F6: a link planted inside the destination is never followed (a restore runs as SYSTEM): refused with a clear message, the target of the link stays untouched.
VM2B_TEST("restore", restore_never_follows_links_in_the_destination)
{
    LTmp t(c_, "lnk"); Fx fx; T_CHECK(fx.make(c_, t)); if (fx.archive.empty()) return false;
    Str outside = t.sub("outside"); Bytes victim = Data(c_, 5000); Put(JoinPath(outside, "victim.bin"), victim);
    { Str probe = t.sub("probe"); if (!SymDir(JoinPath(probe, "l"), outside)) { c_.skip("this system cannot create symbolic links"); return true; } }
    struct Case { const char* name; int kind; } cases[] = { { "the VM folder is a link to another folder", 0 }, { "a sub folder is a link", 1 }, { "a file is a symbolic link to a file elsewhere", 2 },
                                                            { "a file is a hard link to a file elsewhere", 3 }, { "a link to a folder where a FILE is to be written", 4 },
                                                            { "the VM folder is a junction (real Windows only)", 5 }, { "a sub folder is a junction (real Windows only)", 6 } };
    for (const Case& cs : cases) {
        if (cs.kind >= 5 && OnWine()) continue;
        Str dest = t.sub(Str::fmt("dest%d", cs.kind).c_str()); Str e;
        switch (cs.kind) {
        case 0: T_CHECK(SymDir(JoinPath(dest, "vm"), outside)); break;
        case 1: MakeDirs(JoinPath(dest, "vm"), e); T_CHECK(SymDir(JoinPath(dest, "vm\\sub"), outside)); break;
        case 2: MakeDirs(JoinPath(dest, "vm"), e); if (!SymFile(JoinPath(dest, "vm\\disk.vmdk"), JoinPath(outside, "victim.bin"))) { c_.note("(%s: file symbolic links are not available here)", cs.name); continue; } break;
        case 3: MakeDirs(JoinPath(dest, "vm"), e); if (!HardLink(JoinPath(dest, "vm\\disk.vmdk"), JoinPath(outside, "victim.bin"))) { c_.note("(%s: hard links are not available here)", cs.name); continue; } break;
        case 4: MakeDirs(JoinPath(dest, "vm"), e); T_CHECK(SymDir(JoinPath(dest, "vm\\a.vmx"), outside)); break;
        case 5: MakeDirs(dest, e); T_CHECK(Junction(JoinPath(dest, "vm"), outside)); break;
        case 6: MakeDirs(JoinPath(dest, "vm"), e); T_CHECK(Junction(JoinPath(dest, "vm\\sub"), outside)); break;
        }
        Str err; bool ok = fx.restore(dest, err);
        if (ok) c_.note("%s: the restore went through", cs.name);
        T_CHECK(!ok);
        T_CHECK(err.find("link") >= 0 || err.find("hard") >= 0 || err.find("folder of that name") >= 0 || err.find("outside") >= 0);
        c_.note("%s: %s", cs.name, err.c_str());
        T_CHECK(Same(JoinPath(outside, "victim.bin"), victim));               // untouched
        Vec<Str> names; { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(outside, "*")), &fd); if (h != INVALID_HANDLE_VALUE) { do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) names.push_back(FromWide(fd.cFileName)); } while (FindNextFileW(h, &fd)); FindClose(h); } }
        T_CHECK(names.size() == 1);                                           // nothing was created beside it
    }
    // an ordinary destination, an existing one and an overwritten one: works as before
    Str good = t.sub("good"); Str err; T_CHECK(fx.restore(good, err)); T_CHECK(Same(JoinPath(good, "vm\\disk.vmdk"), fx.disk)); T_CHECK(Same(JoinPath(good, "vm\\sub\\b.vmdk"), fx.sub));
    Put(JoinPath(good, "vm\\a.vmx"), Data(c_, 10)); T_CHECK(fx.restore(good, err)); T_CHECK(Same(JoinPath(good, "vm\\a.vmx"), fx.vmx));
    return true;
}

// F6: the state of a restore is not kept in the destination; a state file or ".tmp" planted there is not read and not written through.
VM2B_TEST("restore", restore_state_is_kept_outside_the_destination)
{
    LTmp t(c_, "rst"); Fx fx; T_CHECK(fx.make(c_, t)); if (fx.archive.empty()) return false;
    Str dest = t.sub("dest"), state = t.sub("state"), outside = t.sub("outside"); Bytes victim = Data(c_, 2000); Put(JoinPath(outside, "victim.bin"), victim);
    // planted by somebody: the old state file name with nonsense, a "restore-state.json" and temporary files (also as links to a victim, when the system can)
    Bytes junk; { Str j = "{\"archive\":\"x\",\"root\":\"y\",\"segment\":99,\"check\":\"z\"}"; junk.resize(j.size()); memcpy(junk.data(), j.data(), j.size()); }
    Put(JoinPath(dest, ".vm2b-restore.json"), junk); Put(JoinPath(dest, "restore-state.json"), junk);
    bool linksOk = SymFile(JoinPath(dest, "restore-state.json.tmp"), JoinPath(outside, "victim.bin")); (void)linksOk;
    struct StopAfter : Progress { int files; int n; StopAfter() : files(0), n(0) {} void onFile(const Str&) override { ++files; } bool aborted() override { return files >= 3; } bool keepOnAbort() override { return true; } } stop;
    Str err; bool ok = fx.restore(dest, err, state, &stop);
    c_.note("interrupted restore: %s", ok ? "finished?!" : err.c_str());
    // the state of the interrupted restore is in the state folder, none in the destination
    bool inState = false; { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(state, "*.json")), &fd); if (h != INVALID_HANDLE_VALUE) { inState = true; FindClose(h); } }
    T_CHECK(inState || ok);
    Bytes b; T_CHECK(Get(JoinPath(dest, ".vm2b-restore.json"), b)); T_CHECK(b.size() == junk.size());            // the planted file was not touched
    T_CHECK(Same(JoinPath(outside, "victim.bin"), victim));
    // the run that continues (state kept outside) and finishes: the destination holds the files, the state is gone
    Str e2; T_CHECK(fx.restore(dest, e2, state)); T_CHECK(Same(JoinPath(dest, "vm\\disk.vmdk"), fx.disk)); T_CHECK(Same(JoinPath(dest, "vm\\a.vmx"), fx.vmx)); T_CHECK(Same(JoinPath(dest, "vm\\sub\\b.vmdk"), fx.sub));
    { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(state, "*.json")), &fd); T_CHECK(h == INVALID_HANDLE_VALUE); if (h != INVALID_HANDLE_VALUE) FindClose(h); }
    T_CHECK(Same(JoinPath(outside, "victim.bin"), victim));
    // a state of a folder that was deleted and made again is not trusted (its files are gone): the restore starts over and the files are all there
    Str d2 = t.sub("dest2"), st2 = t.sub("state2");
    struct Stop1 : Progress { int files; Stop1() : files(0) {} void onFile(const Str&) override { ++files; } bool aborted() override { return files >= 1; } bool keepOnAbort() override { return true; } } s1;
    Str e3; fx.restore(d2, e3, st2, &s1);
    RemoveTree(d2); Str me; MakeDirs(d2, me);
    Str e4; T_CHECK(fx.restore(d2, e4, st2)); T_CHECK(Same(JoinPath(d2, "vm\\disk.vmdk"), fx.disk)); T_CHECK(Same(JoinPath(d2, "vm\\a.vmx"), fx.vmx)); T_CHECK(Same(JoinPath(d2, "vm\\sub\\b.vmdk"), fx.sub));
    return true;
}

// F6: WriteWholeFile (config, state, journals) writes through a temporary file that is created exclusively: a planted "<name>.tmp" (or a link of that name) is not used.
VM2B_TEST("restore", temporary_files_are_created_exclusively)
{
    LTmp t(c_, "tmpx"); Str dir = t.sub("d"), outside = t.sub("outside"); Bytes victim = Data(c_, 1000); Put(JoinPath(outside, "victim.bin"), victim);
    Str target = JoinPath(dir, "config.json"); Str e;
    Bytes old = Data(c_, 20); Put(JoinPath(dir, "config.json.tmp"), old);                       // planted temporary file (the old fixed name)
    bool lnk = SymFile(JoinPath(dir, "other.json.tmp"), JoinPath(outside, "victim.bin"));
    Bytes data = Data(c_, 5000);
    T_CHECK(WriteWholeFile(target.c_str(), data.data(), data.size(), e)); T_CHECK(Same(target, data));
    T_CHECK(Same(JoinPath(dir, "config.json.tmp"), old));                                        // not used, not removed
    if (lnk) { Str t2 = JoinPath(dir, "other.json"); T_CHECK(WriteWholeFile(t2.c_str(), data.data(), data.size(), e)); T_CHECK(Same(JoinPath(outside, "victim.bin"), victim)); T_CHECK(Same(t2, data)); }
    // many saves in a row leave no temporary file behind
    for (int i = 0; i < 20; ++i) { Bytes d = Data(c_, (size_t)c_.range(0, 3000)); T_CHECK(WriteWholeFile(target.c_str(), d.data(), d.size(), e)); T_CHECK(Same(target, d)); }
    int tmps = 0; { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*.tmp")), &fd); if (h != INVALID_HANDLE_VALUE) { do { ++tmps; } while (FindNextFileW(h, &fd)); FindClose(h); } }
    T_CHECK(tmps == (lnk ? 2 : 1));                                                              // only the two planted ones
    return true;
}

// F10: a file of the VM replaced by a link after the folder was walked is not read.
VM2B_TEST("engine", directory_source_does_not_read_files_replaced_by_links)
{
    LTmp t(c_, "srclnk"); Str root = t.sub("vms"), outside = t.sub("outside");
    Bytes secret = Data(c_, 40000);
    const char* mark = "TOP-SECRET-MARKER-0123456789"; for (size_t i = 0; i < strlen(mark); ++i) secret[100 + i] = (u8)mark[i];
    Put(JoinPath(outside, "secret.bin"), secret); Put(JoinPath(outside, "b.vmdk"), secret); Put(JoinPath(outside, "a.vmdk"), secret);
    { Str probe = t.sub("probe"); if (!SymDir(JoinPath(probe, "l"), outside)) { c_.skip("this system cannot create symbolic links"); return true; } }
    for (int variant = 0; variant < 4; ++variant) {            // 0: a file becomes a link   1: a sub folder becomes a link   2: nothing changes (control)   3: a sub folder becomes a junction (real Windows)
        if (variant == 3 && OnWine()) continue;
        Str vmdir = JoinPath(root, Str::fmt("vm%d", variant).c_str());
        Put(JoinPath(vmdir, "a.vmdk"), Data(c_, 30000)); Put(JoinPath(vmdir, "sub\\b.vmdk"), Data(c_, 50000)); Put(JoinPath(vmdir, "sub\\secret.bin"), Data(c_, 40000));
        VmSource* src = NewDirectorySource(root);
        Str fp, token, warn; T_CHECK(src->begin(Str::fmt("vm%d", variant), false, "", fp, token, warn));
        if (variant == 0) { DeleteFileW(FilePath(JoinPath(vmdir, "a.vmdk"))); T_CHECK(SymFile(JoinPath(vmdir, "a.vmdk"), JoinPath(outside, "secret.bin"))); }
        if (variant == 1) { RemoveTree(JoinPath(vmdir, "sub")); T_CHECK(SymDir(JoinPath(vmdir, "sub"), outside)); }
        if (variant == 3) { RemoveTree(JoinPath(vmdir, "sub")); T_CHECK(Junction(JoinPath(vmdir, "sub"), outside)); }
        MemSink ms; TarWriter tw(&ms); Str pw;
        bool ok = src->pack(tw, nullptr, pw) && tw.finish();
        if (variant == 2) T_CHECK(ok);
        else {
            if (ok) c_.note("variant %d: the pack went through", variant);
            T_CHECK(!ok); c_.note("variant %d: %s", variant, src->err.c_str());
            T_CHECK(src->err.find("link") >= 0);
        }
        bool leaked = false; for (size_t i = 0; i + strlen(mark) <= ms.data.size(); ++i) if (memcmp(ms.data.data() + i, mark, strlen(mark)) == 0) { leaked = true; break; }
        T_CHECK(!leaked);                                                                         // the secret of the other folder is not in the stream
        delete src;
    }
    // a link as the folder of the VM: refused at the start (existing check)
    { Str probe2 = JoinPath(root, "vmlink"); T_CHECK(SymDir(probe2, outside)); VmSource* src = NewDirectorySource(root); Str fp, token, warn; T_CHECK(!src->begin("vmlink", false, "", fp, token, warn)); delete src; }
    return true;
}

} // namespace vm2b
