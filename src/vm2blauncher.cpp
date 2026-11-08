// VM2Backup-Setup.exe at the root of the package (built for x86, runs on every Windows): starts the installer of the
// Windows it runs on - x64\VM2Backup-Setup.exe on 64-bit Windows, x86\... on 32-bit Windows - with the same command
// line. The released file carries the whole package (a cabinet, MSZIP) appended to the exe, followed by a 16-byte
// trailer "VM2BPKG1" + its size (package.sh): it is extracted to a temporary folder first and removed after the
// installer closes. Without a payload (a development folder) the x64\ / x86\ folders next to this file are used.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef BOOL(WINAPI* IsWow64Process2Fn)(HANDLE, USHORT*, USHORT*);

static const char kMagic[8] = { 'V', 'M', '2', 'B', 'P', 'K', 'G', '1' };
static const char kManMagic[8] = { 'V', 'M', '2', 'B', 'H', 'S', 'H', '1' };   // the list of file hashes in front of the cabinet

static bool Russian() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN; }
static bool g_quiet = false;     // /silent: an unattended run must end with an exit code, not wait forever for somebody to press OK
static bool g_cabFailed = false;  // the cabinet itself could not be read (SetupIterateCabinet failed)
static DWORD g_extractError = 0; // its Windows error (Wine leaves 0)
// A silent run shows no window, so the reason is written to %TEMP%\VM2Backup-Setup.log (English, one line per error: the exit code alone says nothing to the person who deploys the package).
static void LogSilentError(const wchar_t* text)
{
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(path, MAX_PATH, L"%lsVM2Backup-Setup.log", temp);
    path[MAX_PATH - 1] = 0;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t line[MAX_PATH + 400];
    _snwprintf(line, MAX_PATH + 400, L"%04d-%02d-%02d %02d:%02d:%02d error: %ls\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
    line[MAX_PATH + 399] = 0;
    char utf8[(MAX_PATH + 400) * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD wrote = 0;
    if (n > 1) WriteFile(h, utf8, (DWORD)n - 1, &wrote, nullptr);
    CloseHandle(h);
    OutputDebugStringW(line);
}
static void Error(const wchar_t* en, const wchar_t* ru, const wchar_t* extra = L"")
{
    wchar_t text[MAX_PATH + 300];
    if (g_quiet) { _snwprintf(text, MAX_PATH + 300, L"%ls%ls", en, extra); text[MAX_PATH + 299] = 0; LogSilentError(text); return; }
    _snwprintf(text, MAX_PATH + 300, L"%s%s", Russian() ? ru : en, extra);
    text[MAX_PATH + 299] = 0;
    MessageBoxW(nullptr, text, L"VM2Backup", MB_OK | MB_ICONERROR);
}

// ---- extraction into a private folder (security review F7) -------------------------------------------------------------------------
// * the folder has an unpredictable name and is made with CreateDirectory (fails when it exists), its DACL lets in only the current user, SYSTEM and
//   Administrators (the elevated installer may run as another administrator account), nobody inherits anything;
// * the names from the cabinet are checked (no "..", no absolute path / drive letter / ":" stream, no device names, no trailing dot or blank);
// * the files are hashed against the list the package carries (made by package.sh, inside the signed file), and are held open with no write / delete
//   sharing while the installer runs, so nothing can be swapped after the check (the elevated process starts from these very files).
struct Extract {
    wchar_t root[MAX_PATH];
    wchar_t names[64][MAX_PATH];      // the cabinet names of the extracted files
    int count;
    bool badName;
};
static HANDLE g_held[64];
static int g_heldCount = 0;

static bool NameComponentOk(const wchar_t* c, size_t n)
{
    if (n == 0 || n > 100) return false;
    if (c[n - 1] == L'.' || c[n - 1] == L' ') return false;                              // "x." and "x " are another file on Windows
    for (size_t i = 0; i < n; i++) if (c[i] < 32 || wcschr(L"<>:\"|?*", c[i])) return false;   // ':' = an alternate data stream, drive letter
    size_t stem = 0; while (stem < n && c[stem] != L'.') stem++;                         // device names, also with an extension (CON.txt)
    while (stem > 0 && c[stem - 1] == L' ') stem--;
    static const wchar_t* dev[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
    for (const wchar_t* d : dev) if (_wcsnicmp(c, d, stem) == 0 && wcslen(d) == stem) return false;
    if (stem == 4 && (_wcsnicmp(c, L"COM", 3) == 0 || _wcsnicmp(c, L"LPT", 3) == 0) && c[3] >= L'0' && c[3] <= L'9') return false;
    return true;
}

// A name inside the cabinet: a relative path of plain components. `out` gets it with backslashes.
static bool SafeCabName(const wchar_t* in, wchar_t* out, size_t cap)
{
    size_t n = wcslen(in);
    if (n == 0 || n >= cap || n > 200) return false;
    for (size_t i = 0; i < n; i++) out[i] = in[i] == L'/' ? L'\\' : in[i];
    out[n] = 0;
    if (out[0] == L'\\') return false;                                                  // absolute or UNC path
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || out[i] == L'\\') {
            size_t len = i - start;
            if (len == 0 || !NameComponentOk(out + start, len)) return false;           // empty = "\\" twice; this also refuses "." and ".." (a trailing dot)
            start = i + 1;
        }
    }
    return true;
}

static UINT CALLBACK CabCallback(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR)
{
    Extract* ex = (Extract*)context;
    if (notification == SPFILENOTIFY_FILEINCABINET) {
        FILE_IN_CABINET_INFO_W* info = (FILE_IN_CABINET_INFO_W*)param1;
        wchar_t clean[MAX_PATH];
        if (ex->count >= 64 || !SafeCabName(info->NameInCabinet, clean, MAX_PATH)) { ex->badName = true; return FILEOP_ABORT; }
        _snwprintf(info->FullTargetName, MAX_PATH, L"%ls\\%ls", ex->root, clean);
        info->FullTargetName[MAX_PATH - 1] = 0;
        wcscpy(ex->names[ex->count++], clean);
        wchar_t dir[MAX_PATH];
        wcscpy(dir, info->FullTargetName);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) { *slash = 0; SHCreateDirectoryExW(nullptr, dir, nullptr); }        // sub-folders inherit the private DACL of the root
        return FILEOP_DOIT;
    }
    if (notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_FILE_NOT_FOUND;   // one cabinet only
    return NO_ERROR;
}

static bool RandomHex32(wchar_t* out)    // 128 random bits as 32 hex digits
{
    BYTE b[16] = {};
    HCRYPTPROV prov = 0;
    bool ok = CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT) && CryptGenRandom(prov, 16, b);
    if (prov) CryptReleaseContext(prov, 0);
    if (!ok) {   // never reached on a working system; still unpredictable enough to not collide with another run
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        ULONGLONG a = GetTickCount64() * 2654435761ull ^ (ULONGLONG)q.QuadPart, c = (ULONGLONG)GetCurrentProcessId() * 11400714819323198485ull ^ (ULONGLONG)GetCurrentThreadId();
        memcpy(b, &a, 8); memcpy(b + 8, &c, 8);
    }
    for (int i = 0; i < 16; i++) _snwprintf(out + 2 * i, 3, L"%02x", b[i]);
    return true;
}

// A new folder %TEMP%\VM2Backup-Setup-<random> that only the current user, SYSTEM and Administrators can enter.
static bool MakePrivateRoot(wchar_t* root)
{
    wchar_t temp[MAX_PATH], rnd[40];
    GetTempPathW(MAX_PATH, temp);
    RandomHex32(rnd);
    _snwprintf(root, MAX_PATH, L"%lsVM2Backup-Setup-%ls", temp, rnd);
    root[MAX_PATH - 1] = 0;
    wchar_t* userSid = nullptr;
    HANDLE tok = nullptr; BYTE buf[256]; DWORD need = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &need)) ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &userSid);
        CloseHandle(tok);
    }
    wchar_t sddl[256];
    if (userSid) _snwprintf(sddl, 256, L"D:P(A;OICI;FA;;;%ls)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", userSid);
    else wcscpy(sddl, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)");           // owner rights: the creator
    if (userSid) LocalFree(userSid);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) return false;
    bool ok = CreateDirectoryW(root, &sa) != FALSE;                                      // exclusive: an existing folder (planted by somebody) is an error
    LocalFree(sa.lpSecurityDescriptor);
    return ok;
}

// ---- SHA-256 (CryptoAPI) and the file list of the package ------------------------------------------------------------------------
static bool Sha256Handle(HANDLE f, BYTE out[32], unsigned long long* size)
{
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0; bool ok = false;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) return false;
    if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &h)) {
        LARGE_INTEGER z = {}; static BYTE buf[1 << 16]; DWORD got = 0; unsigned long long total = 0; ok = SetFilePointerEx(f, z, nullptr, FILE_BEGIN) != FALSE;
        while (ok && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) { ok = CryptHashData(h, buf, got, 0) != FALSE; total += got; }
        DWORD len = 32;
        ok = ok && CryptGetHashParam(h, HP_HASHVAL, out, &len, 0) && len == 32;
        *size = total;
        CryptDestroyHash(h);
    }
    CryptReleaseContext(prov, 0);
    return ok;
}

static int HexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// manifest lines: "<64 hex digits> <size> <path>\n".  The file `name` must be listed with this hash and size.
static bool ManifestHas(const char* man, size_t manLen, const wchar_t* name, const BYTE hash[32], unsigned long long size)
{
    char want[300]; int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, want, sizeof(want), nullptr, nullptr);
    if (n <= 1) return false;
    size_t wl = (size_t)n - 1;
    for (size_t pos = 0; pos < manLen;) {
        size_t e = pos; while (e < manLen && man[e] != '\n') e++;
        size_t len = e - pos; const char* l = man + pos; pos = e + 1;
        if (len && l[len - 1] == '\r') len--;
        if (len < 64 + 1 + 1 + 1 + 1) continue;
        BYTE h[32]; bool hex = true;
        for (int i = 0; i < 32 && hex; i++) { int a = HexVal(l[2 * i]), b = HexVal(l[2 * i + 1]); if (a < 0 || b < 0) hex = false; else h[i] = (BYTE)(a * 16 + b); }
        if (!hex || l[64] != ' ') continue;
        const char* p = l + 65; size_t left = len - 65;
        unsigned long long sz = 0; size_t k = 0; while (k < left && p[k] >= '0' && p[k] <= '9') sz = sz * 10 + (unsigned)(p[k++] - '0');
        if (k == 0 || k >= left || p[k] != ' ') continue;
        const char* path = p + k + 1; size_t pl = left - k - 1;
        if (pl == wl && memcmp(path, want, wl) == 0) return sz == size && memcmp(h, hash, 32) == 0;
    }
    return false;
}

static int ManifestCount(const char* man, size_t manLen)
{
    int n = 0; for (size_t i = 0; i < manLen; i++) if (man[i] == '\n') n++;
    return n;
}

// After the extraction: every extracted file is opened WITHOUT write / delete sharing (and stays open until the installer is done), then compared with the
// package's list when it has one.  false: a file is missing, changed or not in the list.
static bool HoldAndVerify(const Extract& ex, const char* man, size_t manLen)
{
    if (man && ManifestCount(man, manLen) != ex.count) return false;
    for (int i = 0; i < ex.count; i++) {
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", ex.root, ex.names[i]);
        path[MAX_PATH - 1] = 0;
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        g_held[g_heldCount++] = h;
        BY_HANDLE_FILE_INFORMATION bi;
        if (!GetFileInformationByHandle(h, &bi) || (bi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;   // a link instead of a file
        if (man) {
            BYTE hash[32]; unsigned long long size = 0;
            if (!Sha256Handle(h, hash, &size) || !ManifestHas(man, manLen, ex.names[i], hash, size)) return false;
        }
    }
    return true;
}

static void ReleaseHeld() { for (int i = 0; i < g_heldCount; i++) CloseHandle(g_held[i]); g_heldCount = 0; }

// The payload appended to this exe, extracted to a new temporary folder (root). false: there is none / it failed.
static bool ExtractPayload(wchar_t* root, bool* failed)
{
    *failed = false;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {}, pos = {};
    GetFileSizeEx(f, &size);
    // A signed file (Authenticode) carries its signature AFTER the payload and the trailer: the certificate table is the last thing in the file.
    // The payload ends where the table starts (tools/sign.sh signs the finished single file; the signature covers the whole payload).
    {
        IMAGE_DOS_HEADER dos = {};
        DWORD rd = 0;
        LARGE_INTEGER z = {};
        if (SetFilePointerEx(f, z, nullptr, FILE_BEGIN) && ReadFile(f, &dos, sizeof(dos), &rd, nullptr) && rd == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
            IMAGE_NT_HEADERS32 nt = {};
            z.QuadPart = dos.e_lfanew;
            if (SetFilePointerEx(f, z, nullptr, FILE_BEGIN) && ReadFile(f, &nt, sizeof(nt), &rd, nullptr) && rd == sizeof(nt) &&
                nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
                const IMAGE_DATA_DIRECTORY& sec = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];   // VirtualAddress = a FILE offset here
                if (sec.VirtualAddress > 0 && sec.Size > 0 && (LONGLONG)sec.VirtualAddress + sec.Size <= size.QuadPart) {
                    size.QuadPart = sec.VirtualAddress;
                    // the signing tool pads the file to a multiple of 8 before the table: skip up to 7 zero bytes behind the trailer
                    for (int pad = 0; pad < 8 && size.QuadPart > 16; pad++) {
                        char t[16] = {};
                        LARGE_INTEGER at;
                        at.QuadPart = size.QuadPart - 16;
                        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, t, 16, &rd, nullptr) && rd == 16 && memcmp(t, kMagic, 8) == 0) break;
                        size.QuadPart--;
                    }
                }
            }
        }
    }
    char trailer[16] = {};
    DWORD got = 0;
    pos.QuadPart = size.QuadPart - 16;
    unsigned long long cabSize = 0;
    bool ok = size.QuadPart > 16 && SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, trailer, 16, &got, nullptr) &&
              got == 16 && memcmp(trailer, kMagic, 8) == 0;
    if (ok) {
        memcpy(&cabSize, trailer + 8, 8);
        ok = cabSize > 0 && cabSize < (unsigned long long)size.QuadPart;
    }
    if (!ok) { CloseHandle(f); return false; }          // a development folder: no payload
    *failed = true;                                      // from here on, a payload that cannot be extracted is an error
    wchar_t cab[MAX_PATH];
    if (!MakePrivateRoot(root)) { CloseHandle(f); root[0] = 0; return false; }              // *failed stays true: "could not be unpacked"
    // the list of files and hashes sits right before the cabinet: [list]["VM2BHSH1" + its size (8 bytes)][cabinet]["VM2BPKG1" + cabinet size]; packages without it are accepted
    static char man[8192]; size_t manLen = 0; bool haveMan = false;
    {
        LARGE_INTEGER at; char ft[16] = {}; DWORD rd = 0;
        at.QuadPart = size.QuadPart - 16 - (LONGLONG)cabSize - 16;
        if (at.QuadPart > 0 && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, ft, 16, &rd, nullptr) && rd == 16 && memcmp(ft, kManMagic, 8) == 0) {
            unsigned long long ml = 0; memcpy(&ml, ft + 8, 8);
            at.QuadPart -= (LONGLONG)ml;
            if (ml > 0 && ml <= sizeof(man) && at.QuadPart > 0 && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, man, (DWORD)ml, &rd, nullptr) && rd == ml) { manLen = (size_t)ml; haveMan = true; }
            else { CloseHandle(f); return false; }                                         // a list that cannot be read: the package is damaged
        }
    }
    _snwprintf(cab, MAX_PATH, L"%ls\\payload.cab", root);
    cab[MAX_PATH - 1] = 0;
    HANDLE out = CreateFileW(cab, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    pos.QuadPart = size.QuadPart - 16 - (LONGLONG)cabSize;
    ok = out != INVALID_HANDLE_VALUE && SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    static char buf[1 << 20];
    for (unsigned long long left = cabSize; ok && left > 0;) {
        DWORD chunk = left > sizeof(buf) ? (DWORD)sizeof(buf) : (DWORD)left, wrote = 0;
        ok = ReadFile(f, buf, chunk, &got, nullptr) && got == chunk && WriteFile(out, buf, chunk, &wrote, nullptr) && wrote == chunk;
        left -= chunk;
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(f);
    static Extract ex;
    memset(&ex, 0, sizeof(ex)); wcscpy(ex.root, root);
    if (ok) { ok = SetupIterateCabinetW(cab, 0, CabCallback, &ex) != FALSE; if (!ok) { g_cabFailed = true; g_extractError = ex.badName ? ERROR_INVALID_DATA : GetLastError(); } }
    DeleteFileW(cab);
    if (ok && !HoldAndVerify(ex, haveMan ? man : nullptr, manLen)) { ok = false; g_cabFailed = true; g_extractError = ERROR_INVALID_DATA; ReleaseHeld(); }
    if (ok) *failed = false;
    return ok;
}

static void RemoveTree(const wchar_t* root)
{
    wchar_t from[MAX_PATH + 2] = {};
    wcsncpy(from, root, MAX_PATH);                       // double-0 terminated
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int show)
{
    g_quiet = cmdLine && wcsstr(cmdLine, L"/silent") != nullptr;
    // The machine Windows runs on: x64 / ARM64 (this x86 program runs under WOW64 there) or x86.
    USHORT process = 0, native = IMAGE_FILE_MACHINE_I386;
    IsWow64Process2Fn isWow64Process2 = (IsWow64Process2Fn)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (isWow64Process2) isWow64Process2(GetCurrentProcess(), &process, &native);
    else {
        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);
        native = wow64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    }
    // ARM64 Windows runs the x64 build under emulation.
    const wchar_t* arch = (native == IMAGE_FILE_MACHINE_AMD64 || native == IMAGE_FILE_MACHINE_ARM64) ? L"x64" : L"x86";

    wchar_t root[MAX_PATH], setup[MAX_PATH];
    bool failed = false, extracted = ExtractPayload(root, &failed);
    if (failed) {
        wchar_t code[80] = L"";
        if (g_extractError) _snwprintf(code, 80, L" (Windows error %lu)", (unsigned long)g_extractError);
        // the cabinet is unreadable: wrong data (13), CRC (23), corrupt file (1392), checksum (323) or no error code at all (Wine) = damaged; a full disk or denied access is something else
        bool damaged = g_cabFailed && (g_extractError == 0 || g_extractError == ERROR_INVALID_DATA || g_extractError == ERROR_CRC || g_extractError == ERROR_FILE_CORRUPT || g_extractError == ERROR_DATA_CHECKSUM_ERROR);
        if (damaged) Error(L"The installation package is damaged or incomplete (it was changed after it was built, or the download was cut short). Download Setup again", L"Пакет установки повреждён или неполон (файл изменён после сборки или скачан не до конца). Скачайте установщик заново", code);
        else Error(L"The installer could not be unpacked to a temporary folder", L"Не удалось распаковать установщик во временную папку", code);
        ReleaseHeld();
        if (root[0]) RemoveTree(root);
        return 1;
    }
    if (!extracted) {
        GetModuleFileNameW(nullptr, root, MAX_PATH);
        wchar_t* slash = wcsrchr(root, L'\\');
        if (slash) *slash = 0;
    }
    _snwprintf(setup, MAX_PATH, L"%ls\\%ls\\VM2Backup-Setup.exe", root, arch);
    setup[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(setup) == INVALID_FILE_ATTRIBUTES) {
        Error(L"The installer for this system was not found: ", L"Не найден установщик для этой системы: ", setup);
        ReleaseHeld();
        if (extracted) RemoveTree(root);
        return 1;
    }
    // "open" honours the installer's manifest (administrator rights: the UAC prompt comes from there).
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = setup;
    sei.lpParameters = cmdLine && *cmdLine ? cmdLine : nullptr;
    sei.nShow = show;
    bool started = ShellExecuteExW(&sei) != FALSE;
    DWORD code = started ? 0 : 1;
    if (started && sei.hProcess) {
        // The installer copies what it needs to Program Files; the temporary folder goes when it closes.
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &code);
    }
    ReleaseHeld();                                       // the files may go now
    if (extracted) RemoveTree(root);
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return (int)code;
}
