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
static void Error(const wchar_t* en, const wchar_t* ru, const wchar_t* extra = L"")
{
    if (g_quiet) return;
    wchar_t text[MAX_PATH + 300];
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
    ok = ok && SetupIterateCabinetW(cab, 0, CabCallback, root);
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
        Error(L"The installer could not be unpacked to a temporary folder.", L"Не удалось распаковать установщик во временную папку.");
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
