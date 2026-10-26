#include "winutil.h"
#include <time.h>

namespace vm2b {

Wide::Wide(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) { m_v.resize(1); return; }
    m_v.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, m_v.data(), n);
}

Str FromWide(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return Str();
    Str s; s.resize((size_t)n - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.mut(), n, nullptr, nullptr);
    return s;
}

Wide FilePath(const Str& utf8)
{
    Str p = utf8.replace_all("/", "\\");
    bool prefixed = p.starts_with("\\\\?\\");
    bool unc = p.starts_with("\\\\") && !prefixed;
    bool drive = p.size() >= 3 && p[1] == ':' && p[2] == '\\';
    if (!prefixed && !unc && !drive) {                       // relative: make it absolute (needed for the prefix)
        Wide w(p);
        wchar_t full[2048]; DWORD n = GetFullPathNameW(w.c_str(), 2048, full, nullptr);
        if (n && n < 2048) { p = FromWide(full); drive = p.size() >= 3 && p[1] == ':'; unc = p.starts_with("\\\\"); }
    }
    if (!prefixed && p.size() >= 240) {
        if (unc) p = "\\\\?\\UNC\\" + p.substr(2);
        else if (drive) p = "\\\\?\\" + p;
    }
    return Wide(p);
}

Str WinErrorText(unsigned long code)
{
    wchar_t buf[512]; buf[0] = 0;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 512, nullptr);
    while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ' || buf[n - 1] == L'.')) buf[--n] = 0;
    return n ? Str::fmt("%s (%lu)", FromWide(buf).c_str(), code) : Str::fmt("error %lu", code);
}

Str LastErrorText(const char* what, const Str& path) { return Str::fmt("%s %s: %s", what, path.c_str(), WinErrorText(GetLastError()).c_str()); }

bool DirExists(const Str& path) { DWORD a = GetFileAttributesW(FilePath(path)); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
bool FileExists(const Str& path) { DWORD a = GetFileAttributesW(FilePath(path)); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

bool MakeDirs(const Str& path, Str& err)
{
    Str p = path.replace_all("/", "\\");
    while (p.size() > 3 && p.ends_with("\\")) p = p.substr(0, p.size() - 1);
    if (DirExists(p)) return true;
    int cut = p.rfind('\\');
    if (cut > 2 && !(cut == 2 && p[1] == ':')) { if (!MakeDirs(p.substr(0, (size_t)cut), err)) return false; }
    if (p.size() == 2 && p[1] == ':') return DirExists(p);
    if (!CreateDirectoryW(FilePath(p), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) { err = LastErrorText("cannot create folder", p); return false; }
    return true;
}

bool RemoveTree(const Str& path)
{
    Str base = path.replace_all("/", "\\");
    while (base.size() > 3 && base.ends_with("\\")) base = base.substr(0, base.size() - 1);
    // Сама корневая папка - ссылка (junction / symlink): удаляется только ссылка. Раньше обход шёл ВНУТРЬ и стирал содержимое цели (найдено на настоящей Windows:
    // тест со ссылкой на C:\Windows стирал файлы Windows). Дочерние ссылки и так не разворачиваются (ниже).
    { DWORD at = GetFileAttributesW(FilePath(base)); if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_REPARSE_POINT)) return RemoveDirectoryW(FilePath(base)) != 0 || DeleteFileW(FilePath(base)) != 0; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(FilePath(base + "\\*"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            Str child = base + "\\" + FromWide(fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveTree(child);
            else { SetFileAttributesW(FilePath(child), FILE_ATTRIBUTE_NORMAL); if (!DeleteFileW(FilePath(child))) RemoveDirectoryW(FilePath(child)); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(FilePath(base)) != 0;
}

Str JoinPath(const Str& a, const Str& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    bool sep = a.ends_with("\\") || a.ends_with("/");
    return sep ? a + b : a + "\\" + b;
}

Str PathBase(const Str& p)
{
    int i = p.rfind('\\'), j = p.rfind('/'); if (j > i) i = j;
    return i < 0 ? p : p.substr((size_t)i + 1);
}

double UnixNow()
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)(t - 116444736000000000ull) / 1e7;
}

static bool LocalTm(double t, struct tm* out)
{
    __time64_t s = (__time64_t)t;
    return _localtime64_s(out, &s) == 0;
}

Str LocalStamp(double t) { struct tm m; if (!LocalTm(t, &m)) return "00000000-000000"; return Str::fmt("%04d%02d%02d-%02d%02d%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec); }
Str LocalIso(double t) { struct tm m; if (!LocalTm(t, &m)) return ""; return Str::fmt("%04d-%02d-%02dT%02d:%02d:%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min, m.tm_sec); }

// Cuts at most `maxBytes` bytes without splitting a UTF-8 sequence (Linux / FTP servers allow 255 BYTES per name, NTFS 255 UTF-16 units;
// the archive name adds ~35 characters to the VM name).
Str TruncateUtf8(const Str& s, size_t maxBytes)
{
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((u8)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

Str SafeName(const Str& s)
{
    Str r;
    for (size_t i = 0; i < s.size(); ++i) {
        u8 c = (u8)s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-' || c >= 0x80;
        r += ok ? (char)c : '_';
    }
    r = TruncateUtf8(r, kMaxNameBytes);
    while (r.ends_with(".")) r = r.substr(0, r.size() - 1);
    if (r.empty()) return Str("vm");
    size_t dot = 0; while (dot < r.size() && r[dot] != '.') ++dot;                 // CON, NUL, COM1 ... (also "nul.txt") are devices on Windows
    if (dot == 3 || dot == 4) {
        char b[5]; for (size_t i = 0; i < dot; ++i) b[i] = (char)((r[i] >= 'a' && r[i] <= 'z') ? r[i] - 32 : r[i]); b[dot] = 0;
        bool dev = dot == 3 ? (!strcmp(b, "CON") || !strcmp(b, "PRN") || !strcmp(b, "AUX") || !strcmp(b, "NUL")) : ((!strncmp(b, "COM", 3) || !strncmp(b, "LPT", 3)) && b[3] >= '0' && b[3] <= '9');
        if (dev) r = "_" + r;
    }
    return r;
}

} // namespace vm2b
