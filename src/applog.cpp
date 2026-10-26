// Program log: UTF-8 with a BOM and timestamps in %ProgramData%\VM2Backup\logs\<name>.log (over 1 MB it becomes
// <name>.old.log), the same layout as Speak2Mic / Show2Cam. An unhandled exception writes the module and the offset of
// the crashing instruction (the link map kept in maps\<version>\ names the function). Standalone: the installer and the
// program both link it.
#include "version.h"
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

// FOLDERID_ProgramData, declared locally so no extra import library is needed.
static const GUID kFolderProgramData = { 0x62ab5d82, 0xfdc1, 0x4dc3, { 0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97 } };

static CRITICAL_SECTION g_cs;
static bool    g_open;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_path[MAX_PATH];

void AppLog(const wchar_t* fmt, ...);

static void WriteUtf8(const wchar_t* text)
{
    HANDLE f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    DWORD written;
    if (size.QuadPart == 0) {
        static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };      // so Notepad shows Cyrillic correctly
        WriteFile(f, bom, 3, &written, nullptr);
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, n);
        if (buf) {
            WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, n, nullptr, nullptr);
            WriteFile(f, buf, (DWORD)(n - 1), &written, nullptr);
            HeapFree(GetProcessHeap(), 0, buf);
        }
    }
    CloseHandle(f);
}

static bool IsElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION el = {};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size)) elevated = el.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

// Права на папки. Раньше ОБЕ папки (данных и logs) получали "Пользователи: изменение" с наследованием, и config.json (серверы, пароли в DPAPI области
// "компьютер", которую снимает любая учётная запись), web.json (хеши паролей, закрытый ключ сертификата) и очередь задач читал и менял любой локальный
// пользователь (найдено на настоящей Windows: guest/acltest.ps1). Теперь папка данных - только SYSTEM и администраторы (защищённый DACL, наследуется
// файлами), а обычным пользователям открыта лишь logs: программа панели пишет туда журнал от имени пользователя.
static void SetProtectedDacl(const wchar_t* dir, const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return;
    BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
    if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present)
        SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);   // применяется и к уже существующим файлам
    LocalFree(sd);
}

static void SecureDataFolders(const wchar_t* dataDir, const wchar_t* logsDir)
{
    SetProtectedDacl(dataDir, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    SetProtectedDacl(logsDir, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)");
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    ULONG_PTR off = 0;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod) && mod) {
        GetModuleFileNameW(mod, name, MAX_PATH);
        off = (ULONG_PTR)addr - (ULONG_PTR)mod;
    }
    const wchar_t* base = wcsrchr(name, L'\\');
    AppLog(L"CRASH: exception 0x%08lX at %ls+0x%llX (address %p), thread %lu", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0,
           base ? base + 1 : name, (unsigned long long)off, addr, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;                        // Windows Error Reporting as usual
}

const wchar_t* AppLogDir() { return g_dir; }

void AppLogOpen(const wchar_t* name)
{
    InitializeCriticalSection(&g_cs);
    PWSTR programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(kFolderProgramData, 0, nullptr, &programData))) {
        _snwprintf(g_dir, MAX_PATH, L"%ls\\VM2Backup\\logs", programData);
        CoTaskMemFree(programData);
    } else {
        wcscpy(g_dir, L"C:\\ProgramData\\VM2Backup\\logs");
    }
    g_dir[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(nullptr, g_dir, nullptr);
    if (IsElevated()) {
        wchar_t parent[MAX_PATH];
        wcscpy(parent, g_dir);
        wchar_t* slash = wcsrchr(parent, L'\\');
        if (slash) { *slash = 0; SecureDataFolders(parent, g_dir); }
    }
    _snwprintf(g_path, MAX_PATH, L"%ls\\%ls.log", g_dir, name);
    g_path[MAX_PATH - 1] = 0;

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 1024 * 1024) {
        wchar_t old[MAX_PATH];
        _snwprintf(old, MAX_PATH, L"%ls\\%ls.old.log", g_dir, name);
        old[MAX_PATH - 1] = 0;
        MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    g_open = true;
    SetUnhandledExceptionFilter(CrashFilter);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    OSVERSIONINFOEXW os = { sizeof(os) };
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW*);
    RtlGetVersionFn getVersion = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (getVersion) getVersion(&os);
    AppLog(L"==== %ls started (pid %lu), VM2Backup build %ls, Windows %lu.%lu.%lu ====", exe, GetCurrentProcessId(),
           L"" VM2B_VER_STR, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
}

void AppLog(const wchar_t* fmt, ...)
{
    if (!g_open) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2048];
    int n = _snwprintf(line, 2048, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    int m = _vsnwprintf(line + n, 2048 - n - 3, fmt, args);
    va_end(args);
    if (m < 0) m = 2048 - n - 3;
    wcscpy(line + n + m, L"\r\n");
    EnterCriticalSection(&g_cs);
    WriteUtf8(line);
    LeaveCriticalSection(&g_cs);
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[1500];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1490, fmt, args);
    va_end(args);
    buf[1490] = 0;
    OutputDebugStringW(buf);
    AppLog(L"%ls", buf);
}
