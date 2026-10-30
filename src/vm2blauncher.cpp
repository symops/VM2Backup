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
#include <stdio.h>
#include <wchar.h>

typedef BOOL(WINAPI* IsWow64Process2Fn)(HANDLE, USHORT*, USHORT*);

static const char kMagic[8] = { 'V', 'M', '2', 'B', 'P', 'K', 'G', '1' };

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

// Cabinet extraction: every file to <root>\<its path in the cabinet>.
static UINT CALLBACK CabCallback(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR)
{
    const wchar_t* root = (const wchar_t*)context;
    if (notification == SPFILENOTIFY_FILEINCABINET) {
        FILE_IN_CABINET_INFO_W* info = (FILE_IN_CABINET_INFO_W*)param1;
        _snwprintf(info->FullTargetName, MAX_PATH, L"%ls\\%ls", root, info->NameInCabinet);
        info->FullTargetName[MAX_PATH - 1] = 0;
        for (wchar_t* c = info->FullTargetName; *c; c++) if (*c == L'/') *c = L'\\';
        wchar_t dir[MAX_PATH];
        wcscpy(dir, info->FullTargetName);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) { *slash = 0; SHCreateDirectoryExW(nullptr, dir, nullptr); }
        return FILEOP_DOIT;
    }
    if (notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_FILE_NOT_FOUND;   // one cabinet only
    return NO_ERROR;
}

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
    wchar_t temp[MAX_PATH], cab[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(root, MAX_PATH, L"%lsVM2Backup-Setup-%lu", temp, GetCurrentProcessId());
    root[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(nullptr, root, nullptr);
    _snwprintf(cab, MAX_PATH, L"%ls\\payload.cab", root);
    cab[MAX_PATH - 1] = 0;
    HANDLE out = CreateFileW(cab, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
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
    if (ok) { ok = SetupIterateCabinetW(cab, 0, CabCallback, root) != FALSE; if (!ok) { g_cabFailed = true; g_extractError = GetLastError(); } }
    DeleteFileW(cab);
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
        RemoveTree(root);
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
    if (extracted) RemoveTree(root);
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return (int)code;
}
