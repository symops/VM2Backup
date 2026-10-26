// The local target: a folder on a local disk or on a network share (UNC), with long-path support.
#include "target.h"
#include "winutil.h"
#include "smb.h"

namespace vm2b {

bool Target::exists(const Str& name, bool& yes)
{
    Vec<Str> names;
    if (!list(names)) return false;
    yes = false;
    for (const Str& n : names) if (n == name) { yes = true; break; }
    return true;
}

bool Target::put_bytes(const Str& name, const void* p, size_t n)
{
    Sink* w = open_write(name);
    if (!w) return false;
    bool ok = w->write(p, n) && w->finish();
    if (!ok) { err = w->err; transient = w->transient; w->abort(); }
    delete w;
    return ok;
}

bool Target::get_bytes(const Str& name, Bytes& out, size_t maxSize)
{
    Source* r = open_read(name);
    if (!r) return false;
    out.clear();
    u8 buf[65536];
    for (;;) {
        i64 k = r->read(buf, sizeof(buf));
        if (k < 0) { err = r->err; transient = r->transient; r->close(); delete r; return false; }
        if (k == 0) break;
        if (out.size() + (size_t)k > maxSize) { r->close(); delete r; return fail("file too big: " + name); }
        size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf, (size_t)k);
    }
    r->close(); delete r;
    return true;
}

namespace {

// Файл на мгновение занят чужим процессом (антивирус, индексатор, другой агент резервного копирования) - нарушение совместного доступа проходит само:
// ждём до ~10 с (как и при переименовании ниже), а не отказываем сразу. Найдено тестом winreal/destination_files_held_by_a_scanner_are_waited_for.
HANDLE OpenWaiting(const Str& path, DWORD access, DWORD share, DWORD disp, DWORD flags)
{
    HANDLE h = INVALID_HANDLE_VALUE; DWORD le = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        h = CreateFileW(FilePath(path), access, share, nullptr, disp, flags, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        le = GetLastError();
        if (le != ERROR_SHARING_VIOLATION && le != ERROR_LOCK_VIOLATION) break;
        Sleep(200);
    }
    SetLastError(le); return h;
}

class LocalWriter : public Sink {
public:
    LocalWriter(const Str& dir, const Str& name) : m_h(INVALID_HANDLE_VALUE), m_done(false) {
        m_final = JoinPath(dir, name); m_tmp = m_final + ".part";
        m_h = OpenWaiting(m_tmp, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (m_h == INVALID_HANDLE_VALUE) err = LastErrorText("cannot create", m_tmp);
    }
    ~LocalWriter() { if (!m_done) abort(); }
    bool ok() const { return m_h != INVALID_HANDLE_VALUE; }
    bool write(const void* p, size_t n) override {
        if (m_h == INVALID_HANDLE_VALUE) return false;
        const u8* d = (const u8*)p;
        while (n) {
            DWORD k = n > (1u << 24) ? (1u << 24) : (DWORD)n, w = 0;
            if (!WriteFile(m_h, d, k, &w, nullptr)) return fail(LastErrorText("cannot write", m_tmp));
            if (!w) return fail("cannot write " + m_tmp + ": the system accepted no data");      // never loop forever on a zero-byte write
            d += w; n -= w;
        }
        return true;
    }
    bool finish() override {
        if (m_done) return true;
        if (m_h == INVALID_HANDLE_VALUE) return false;
        bool ok = FlushFileBuffers(m_h) != 0;
        CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE;
        if (!ok) { Str e = LastErrorText("cannot flush", m_tmp); DeleteFileW(FilePath(m_tmp)); m_done = true; return fail(e); }
        // A virus scanner / indexer / backup agent often opens a file that was just closed: the rename then fails with a sharing violation for a moment.
        BOOL moved = FALSE; DWORD le = 0;
        for (int attempt = 0; attempt < 25; ++attempt) {
            moved = MoveFileExW(FilePath(m_tmp), FilePath(m_final), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (moved) break;
            le = GetLastError();
            if (le != ERROR_SHARING_VIOLATION && le != ERROR_ACCESS_DENIED && le != ERROR_LOCK_VIOLATION) break;
            Sleep(200);
        }
        if (!moved) {
            SetLastError(le); Str e = LastErrorText("cannot rename to", m_final); DeleteFileW(FilePath(m_tmp)); m_done = true; return fail(e);
        }
        m_done = true;
        return true;
    }
    void abort() override {
        if (m_done) return;
        m_done = true;
        if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; }
        DeleteFileW(FilePath(m_tmp));
    }
private:
    HANDLE m_h; Str m_tmp, m_final; bool m_done;
};

class LocalReader : public Source {
public:
    LocalReader(const Str& path) : m_path(path) {
        m_h = OpenWaiting(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN);
        if (m_h == INVALID_HANDLE_VALUE) err = LastErrorText("cannot open", path);
    }
    ~LocalReader() { close(); }
    bool ok() const { return m_h != INVALID_HANDLE_VALUE; }
    i64 read(void* p, size_t n) override {
        if (m_h == INVALID_HANDLE_VALUE) return fail(err.empty() ? Str("file is closed") : err);
        DWORD got = 0;
        if (!ReadFile(m_h, p, n > (1u << 24) ? (1u << 24) : (DWORD)n, &got, nullptr)) return fail(LastErrorText("cannot read", m_path));
        return (i64)got;
    }
    void close() override { if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; } }
private:
    HANDLE m_h; Str m_path;
};

class LocalTarget : public Target {
public:
    explicit LocalTarget(const Str& dir) : m_dir(dir) {}
    const char* kind() const override { return "local"; }
    bool connect() override { Str e; if (!MakeDirs(m_dir, e)) { Str d = SmbDriveUnavailableMessage(m_dir); return fail(d.empty() ? e : d); } return true; }
    Sink* open_write(const Str& name) override {
        if (!safe(name)) { err = "bad file name: " + name; return nullptr; }
        LocalWriter* w = new LocalWriter(m_dir, name);
        if (!w->ok()) { err = w->err; delete w; return nullptr; }
        return w;
    }
    Source* open_read(const Str& name) override {
        if (!safe(name)) { err = "bad file name: " + name; return nullptr; }
        LocalReader* r = new LocalReader(JoinPath(m_dir, name));
        if (!r->ok()) { err = r->err; delete r; return nullptr; }
        return r;
    }
    bool list(Vec<Str>& names) override {
        names.clear();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(FilePath(JoinPath(m_dir, "*")), &fd);
        if (h == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true; return fail(LastErrorText("cannot list", m_dir)); }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Str n = FromWide(fd.cFileName);
            if (n.ends_with(".part")) continue;
            names.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return true;
    }
    bool remove(const Str& name) override {
        if (!safe(name)) return fail("bad file name: " + name);
        BOOL del = FALSE;
        for (int attempt = 0; attempt < 50; ++attempt) { del = DeleteFileW(FilePath(JoinPath(m_dir, name))); if (del || GetLastError() != ERROR_SHARING_VIOLATION) break; Sleep(200); }
        if (!del) { DWORD e = GetLastError(); if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true; return fail(LastErrorText("cannot delete", name)); }
        return true;
    }
private:
    static bool safe(const Str& n) { return !n.empty() && n.find('/') < 0 && n.find('\\') < 0 && n != "." && n != ".." && n.find(':') < 0; }
    Str m_dir;
};

} // namespace

Target* NewLocalTarget(const Json& cfg, Str& err)
{
    Str path = cfg.get("path").str();
    if (path.empty()) { err = "local target: the folder is not set"; return nullptr; }
    return new LocalTarget(path);
}

Target* MakeTarget(const Json& cfg, Str& err)
{
    Str t = cfg.get("type").str();
    // Пароль остался в виде "dpapi:..." - значит расшифровать его не удалось (конфигурацию скопировали на другой компьютер / переустановили Windows).
    // Без этой проверки такая строка уходила бы серверу как пароль, а пользователь видел бы только "неверный пароль".
    if (cfg.get("password").str().starts_with("dpapi:") || cfg.get("key_passphrase").str().starts_with("dpapi:")) { err = "the saved password cannot be decrypted on this computer (it was saved by another Windows installation): enter it again in the server settings"; return nullptr; }
    if (t == "local") return NewLocalTarget(cfg, err);
    if (t == "smb") return NewSmbTarget(cfg, err);
#if defined(VM2B_HAVE_FTP)
    if (t == "ftp" || t == "ftpes" || t == "ftps") return NewFtpTarget(cfg, err);
#endif
    if (t == "sftp") { err = "SFTP is no longer supported, create an SCP server"; return nullptr; }
#if defined(VM2B_HAVE_SSH)
    if (t == "scp") return NewSshTarget(cfg, err);
#endif
    err = Str::fmt("unknown or unavailable target type: %s", t.c_str());
    return nullptr;
}

} // namespace vm2b
