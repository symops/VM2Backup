// The "folder with VMs" source.
#include "engine.h"
#include "winutil.h"
#include "version.h"
#include "smb.h"

namespace vm2b {

const char* const kAbortedMessage = "aborted";

namespace {

struct Entry { Str rel; bool dir; u64 size; i64 mtime; };

static i64 FtToUnix(const FILETIME& ft) { u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; return t < 116444736000000000ull ? 0 : (i64)((t - 116444736000000000ull) / 10000000ull); }

class DirectorySource : public VmSource {
public:
    explicit DirectorySource(const Str& root, SmbConnection* conn = nullptr) : m_conn(conn), m_root(root) {}
    ~DirectorySource() { delete m_conn; }
    const char* kind() const override { return "directory"; }

    bool list(Vec<VmInfo>& vms) override {
        vms.clear();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(FilePath(JoinPath(m_root, "*")), &fd);
        if (h == INVALID_HANDLE_VALUE) { Str d = SmbDriveUnavailableMessage(m_root); return fail(d.empty() ? LastErrorText("cannot read", m_root) : d); }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            VmInfo v; v.name = FromWide(fd.cFileName);
            Vec<Entry> es; bool locked = false; Str w;
            walk(JoinPath(m_root, v.name), "", es, locked, w);
            for (const Entry& e : es) v.size += e.size;
            v.state = locked ? "running" : "stopped";
            vms.push_back(v);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return true;
    }

    // Everything that goes into the stream is fixed here (the file list, the sizes, the times, the meta file), because the
    // stream must be byte for byte the same every time pack() runs again after a failure.
    bool begin(const Str& vm, bool allowLive, const Str& resumeToken, Str& fingerprint, Str& token, Str& warning) override {
        m_entries.clear(); m_vm = vm; token.clear();
        if (vm.empty() || vm.find('/') >= 0 || vm.find('\\') >= 0 || vm == "." || vm == "..") return fail("bad VM name: " + vm);
        m_dir = JoinPath(m_root, vm);
        if (!DirExists(m_dir)) return fail("VM not found: " + vm);
        // Папка ВМ - это ссылка (junction / symlink): list() такие папки не показывает; здесь отказ нужен для имени, заданного вручную или оставшегося в задаче,
        // иначе подмена папки ссылкой увела бы копию (с правами службы) в чужой каталог, например C:\Windows.
        { DWORD at = GetFileAttributesW(FilePath(m_dir)); if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT)) return fail("the folder of the VM \"" + vm + "\" is a link (junction or symbolic link): links are not followed, give the real folder"); }
        bool locked = false; Str skipped;
        walk(m_dir, "", m_entries, locked, skipped);
        // Имя, которое восстановление отвергнет (CON, NUL, "имя." и т.п., создаются только через \\?\): копия выглядела бы успешной, но не восстанавливалась бы.
        for (const Entry& e : m_entries) { Str norm; if (!SafeArchivePath(m_vm + "/" + e.rel, norm)) return fail("cannot back up \"" + e.rel + "\": a file with this name could not be restored on Windows (reserved device name, trailing dot or space, or a path that is too long); rename it first"); }
        if (locked && !allowLive) return fail(Str::fmt("the VM \"%s\" is running (a lock file was found): stop it, or allow a copy of a running VM", vm.c_str()));
        warning.clear();
        if (locked) warning = "the VM was running: the copy is consistent only at the crash level";
        if (!skipped.empty()) { if (!warning.empty()) warning += "; "; warning += "skipped (links): " + skipped; }
        // The meta file is the first entry of the stream and must be byte for byte the same when the backup continues in a later run: its time is
        // taken from the journal's token (set on the first run), not from the clock.
        double now = UnixNow();
        if (!resumeToken.empty() && resumeToken.starts_with("created=")) { double t = atof(resumeToken.c_str() + 8); if (t > 0 && t < 1e11) now = t; }
        token = Str::fmt("created=%.3f", now); now = atof(token.c_str() + 8);
        Json meta = Json::object();
        meta["tool"] = "VM2Backup " VM2B_VER_STR; meta["source"] = "directory"; meta["vm"] = vm; meta["created"] = now; meta["running"] = locked;
        m_meta = meta.dump(true); m_metaTime = (i64)now;
        Bytes b; for (size_t i = 0; i < vm.size(); ++i) b.push_back((u8)vm[i]); b.push_back(0);
        for (const Entry& e : m_entries) { Str l = Str::fmt("%s|%d|%llu|%lld\n", e.rel.c_str(), e.dir ? 1 : 0, (unsigned long long)e.size, (long long)e.mtime); for (size_t i = 0; i < l.size(); ++i) b.push_back((u8)l[i]); }
        u8 h[32]; Sha256(b.data(), b.size(), h);
        fingerprint = "dir:" + HexEncode(h, 32);
        return true;
    }

    bool pack(TarWriter& tar, Progress* pr, Str& warning) override {
        if (!tar.add_bytes("vm2backup-meta.json", m_meta.data(), m_meta.size(), m_metaTime)) return fail(tar.error(), false);
        Bytes buf; buf.resize(1 << 20);
        for (const Entry& e : m_entries) {
            if (pr && pr->aborted()) return fail(kAbortedMessage);
            Str arc = m_vm + "/" + e.rel;
            if (e.dir) { if (!tar.add_dir(arc, e.mtime)) return fail(tar.error()); continue; }
            if (pr) pr->onFile(arc);
            if (!tar.begin_file(arc, e.size, e.mtime)) return fail(tar.error());
            u64 left = e.size, skip = tar.skippable();
            if (skip) { if (!tar.skip_data(skip)) return fail(tar.error()); left -= skip; }       // already stored: not read again
            if (left) {
                // Файл ненадолго занят (антивирус, другая программа): ждём до ~8 с, потом понятная ошибка с именем файла. Монопольно занятый файл
                // (виртуальная машина работает и держит диск) копировать нельзя: резервная копия не должна молча пропускать файл.
                HANDLE h = INVALID_HANDLE_VALUE; DWORD le = 0;
                for (int attempt = 0; attempt < 16; ++attempt) {
                    h = CreateFileW(FilePath(JoinPath(m_dir, e.rel)), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                    if (h != INVALID_HANDLE_VALUE) break;
                    le = GetLastError();
                    if (le != ERROR_SHARING_VIOLATION && le != ERROR_LOCK_VIOLATION) break;
                    if (pr && pr->aborted()) return fail(kAbortedMessage);
                    Sleep(500);
                }
                if (h == INVALID_HANDLE_VALUE) { SetLastError(le); return fail(LastErrorText("cannot read", e.rel) + ((le == ERROR_SHARING_VIOLATION || le == ERROR_LOCK_VIOLATION) ? " (the file is held by another program; the backup does not skip files)" : "")); }
                if (skip) { LARGE_INTEGER p; p.QuadPart = (LONGLONG)skip; if (!SetFilePointerEx(h, p, nullptr, FILE_BEGIN)) { Str er = LastErrorText("cannot seek in", e.rel); CloseHandle(h); return fail(er); } }
                bool short_ = false;
                while (left) {
                    if (pr && pr->aborted()) { CloseHandle(h); return fail(kAbortedMessage); }
                    DWORD want = left > buf.size() ? (DWORD)buf.size() : (DWORD)left, got = 0;
                    if (!ReadFile(h, buf.data(), want, &got, nullptr)) { Str er = LastErrorText("cannot read", e.rel); CloseHandle(h); return fail(er); }
                    if (got == 0) { memset(buf.data(), 0, want); got = want; short_ = true; }        // the file shrank while it was read: the entry keeps its announced size
                    if (!tar.write(buf.data(), got)) { CloseHandle(h); return fail(tar.error()); }
                    left -= got;
                    if (pr) pr->onBytes(got);
                }
                CloseHandle(h);
                if (short_) { if (!warning.empty()) warning += "; "; warning += "the file changed while it was read: " + e.rel; }
            }
            if (!tar.end_file()) return fail(tar.error());
        }
        return true;
    }

private:
    // Everything below `dir`, sorted (so the archive is reproducible); .lck files / folders mark a running VM and are not copied.
    void walk(const Str& dir, const Str& rel, Vec<Entry>& out, bool& locked, Str& skipped) {
        Vec<Str> names; Vec<WIN32_FIND_DATAW> data;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(FilePath(JoinPath(dir, "*")), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue; names.push_back(FromWide(fd.cFileName)); data.push_back(fd); } while (FindNextFileW(h, &fd));
        FindClose(h);
        for (size_t i = 1; i < names.size(); ++i) { Str n = names[i]; WIN32_FIND_DATAW d = data[i]; size_t j = i; while (j > 0 && n < names[j - 1]) { names[j] = names[j - 1]; data[j] = data[j - 1]; --j; } names[j] = n; data[j] = d; }
        for (size_t i = 0; i < names.size(); ++i) {
            const WIN32_FIND_DATAW& d = data[i];
            Str r = rel.empty() ? names[i] : rel + "/" + names[i];
            if (names[i].ends_with(".lck")) { locked = true; continue; }
            if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!skipped.empty()) skipped += ", "; skipped += r; continue; }
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                Entry e = { r, true, 0, FtToUnix(d.ftLastWriteTime) }; out.push_back(e);
                walk(JoinPath(dir, names[i]), r, out, locked, skipped);
            } else {
                Entry e = { r, false, ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow, FtToUnix(d.ftLastWriteTime) }; out.push_back(e);
            }
        }
    }
    SmbConnection* m_conn; Str m_root, m_vm, m_dir, m_meta; i64 m_metaTime = 0; Vec<Entry> m_entries;
};

} // namespace

VmSource* NewDirectorySource(const Str& root) { return new DirectorySource(root); }

VmSource* MakeSource(const Json& server, Str& err)
{
    Str t = server.get("type").str();
    if (server.get("password").str().starts_with("dpapi:")) { err = "the saved password cannot be decrypted on this computer (it was saved by another Windows installation): enter it again in the server settings"; return nullptr; }
    if (t == "directory") {
        Str path = server.get("path").str();
        if (path.empty()) { err = "the folder with the virtual machines is not set"; return nullptr; }
        if (SmbIsUncPath(path)) {                                                     // a network folder (\\host\share\dir), with its own credentials if given
            Str unc, e;
            if (!SmbNormalizeUnc(path, unc, e)) { err = "folder with virtual machines: " + e; return nullptr; }
            Str user = server.get("user").str().trim();
            if (user.empty()) return new DirectorySource(SmbMapPath(unc));
            Str root, sub, host; SmbSplitUnc(unc, root, sub, host);
            SmbConnection* c = new SmbConnection(); bool tr = false;
            if (!c->open(root, user, server.get("password").str(), false, e, tr)) { delete c; err = e; return nullptr; }
            return new DirectorySource(SmbMapPath(unc), c);
        }
        return new DirectorySource(path);
    }
#if defined(VM2B_HAVE_ESXI)
    if (t == "esxi" || t == "vcenter") return NewEsxiSource(server, err);
#endif
    err = Str::fmt("this kind of server cannot be a source of virtual machines: %s", t.c_str());
    return nullptr;
}

} // namespace vm2b
