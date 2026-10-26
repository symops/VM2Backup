// VM2Backup-Setup.exe — graphical installer.
//
// Installs VM2Backup.exe (and itself, for removal) to %ProgramFiles%\VM2Backup, creates the shortcuts, registers the
// program in "Apps & features" and starts it afterwards. The same window removes it again. Runs elevated (manifest).
// Expects VM2Backup.exe next to it. Command line (for scripts): /install or /remove, with /silent for no window;
// the exit code is 0 on success. The settings and the captures the user made are never touched by the removal.
#include "version.h"
#include "lang.h"
#include "applog.h"
#define VM2B_AUTHOR L"Symo <symops@gmail.com>"
#define VM2B_VERSION_STR L"" VM2B_VER_STR
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <winsvc.h>

namespace {

// ---- strings: the shared 17-language table (lang.cpp)
int g_lang = 0;          // language chosen in the window: index in the table

// ---- state -----------------------------------------------------------------------------------------------------
enum { IDC_TITLE = 100, IDC_SUB, IDC_STATE, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_LOG, IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_CLOSE, IDC_LANGLBL, IDC_LANG, IDC_PATHLBL, IDC_PATH, IDC_BROWSE };
enum { ACT_INSTALL = 1, ACT_REMOVE };
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
#define WM_VM2B_QUIT (WM_APP + 3)

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VM2Backup";

HINSTANCE g_inst;
HWND g_wnd;
HFONT g_font, g_fontTitle;
HICON g_icon;
HANDLE g_worker;
int g_action;
bool g_ok, g_silent, g_selfDelete;
bool g_wantDesktop = true, g_wantStart = true, g_wantAuto = true;      // g_wantAuto: start the service right after the installation
wchar_t g_dir[MAX_PATH];      // the folder to install to (the window's box, /dir, or what is installed already)

HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }
UINT g_dpi = 96;
int Sc(int v) { return MulDiv(v, (int)g_dpi, 96); }

void Say(const wchar_t* fmt, ...)
{
    wchar_t line[600];
    va_list ap; va_start(ap, fmt); _vsnwprintf(line, 590, fmt, ap); va_end(ap);
    line[590] = 0;
    Log(L"%s", line);
    if (!g_silent && g_wnd) PostMessageW(g_wnd, WM_APP_LOG, 0, (LPARAM)_wcsdup(line));
}

bool KnownFolder(const KNOWNFOLDERID& id, wchar_t* out)
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) return false;
    wcsncpy(out, p, MAX_PATH - 1); out[MAX_PATH - 1] = 0;
    CoTaskMemFree(p);
    return true;
}

bool ProgramDir(wchar_t* dir)
{
    if (!KnownFolder(FOLDERID_ProgramFiles, dir)) return false;
    wcsncat(dir, L"\\VM2Backup", MAX_PATH - wcslen(dir) - 1);
    return true;
}

int Dpi96() { return 96; }

bool GetRegStr(HKEY root, const wchar_t* key, const wchar_t* name, wchar_t* out, DWORD cap);

// Where VM2Backup is installed now (the registry), else the default.
bool InstalledDir(wchar_t* dir)
{
    if (GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", dir, MAX_PATH) && dir[0]) return true;
    return ProgramDir(dir);
}

// A usable folder: a full path (drive letter or UNC), no wildcard / reserved characters, room for the file names.
bool ValidDir(wchar_t* dir)
{
    size_t n = wcslen(dir);
    while (n > 3 && (dir[n - 1] == L'\\' || dir[n - 1] == L' ')) dir[--n] = 0;
    if (n < 4 || n > 200) return false;
    bool drive = iswalpha(dir[0]) && dir[1] == L':' && dir[2] == L'\\';
    bool unc = dir[0] == L'\\' && dir[1] == L'\\';
    if (!drive && !unc) return false;
    for (size_t i = drive ? 3 : 2; i < n; ++i)
        if (dir[i] < 32 || wcschr(L"<>:\"|?*", dir[i])) return false;
    if (drive && n == 3) return false;                       // a drive root: its files would mix with everything else
    return true;
}

void SiblingPath(const wchar_t* name, wchar_t* out)
{
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, name);
}

bool GetRegStr(HKEY root, const wchar_t* key, const wchar_t* name, wchar_t* out, DWORD cap)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD n = cap * sizeof(wchar_t), t = 0;
    bool ok = RegQueryValueExW(k, name, nullptr, &t, (BYTE*)out, &n) == ERROR_SUCCESS && t == REG_SZ;
    RegCloseKey(k);
    if (ok) out[cap - 1] = 0;
    return ok;
}

void SetRegStr(HKEY k, const wchar_t* name, const wchar_t* v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

// ---- steps -----------------------------------------------------------------------------------------------------
bool RunProcess(const wchar_t* exe, const wchar_t* args, DWORD ms, DWORD* code)
{
    wchar_t cmd[2 * MAX_PATH + 64]; _snwprintf(cmd, 2 * MAX_PATH + 64, L"\"%s\" %s", exe, args);
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    bool done = WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0;
    DWORD c = 1; GetExitCodeProcess(pi.hProcess, &c); if (code) *code = c;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return done;
}

// Stops the service (a running backup is paused and continues after the next start) and closes the windows of the program.
void StopServiceNow()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"VM2BackupService", SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS st;
        if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            for (int i = 0; i < 300 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(200);
            Say(L"VM2Backup service: %s", st.dwCurrentState == SERVICE_STOPPED ? L"stopped" : L"did not stop in time");
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

void CloseRunningApp()
{
    StopServiceNow();
    for (int tries = 0; tries < 3; ++tries) {
        HWND app = nullptr; wchar_t title[128];
        for (HWND w = FindWindowExW(nullptr, nullptr, L"VM2BackupForm", nullptr); w; w = FindWindowExW(nullptr, w, L"VM2BackupForm", nullptr)) {
            GetWindowTextW(w, title, 128);
            if (wcsncmp(title, L"VM2Backup ", 10) == 0 && GetWindow(w, GW_OWNER) == nullptr) { app = w; break; }     // the main window (not a dialog)
        }
        if (!app) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(app, &pid);
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(app, WM_CLOSE, 0, 0);
        DWORD w = proc ? WaitForSingleObject(proc, 8000) : WAIT_FAILED;
        if (proc) CloseHandle(proc);
        Say(L"VM2Backup (pid %lu) closed: %s", pid, w == WAIT_OBJECT_0 ? L"yes" : L"not in time");
        if (!proc) Sleep(500);
    }
}

bool CopyOne(const wchar_t* from, const wchar_t* to)
{
    for (int tries = 0; tries < 20; ++tries) {
        if (CopyFileW(from, to, FALSE)) return true;
        Sleep(250);
    }
    return false;
}

bool MakeLink(const wchar_t* linkPath, const wchar_t* target, const wchar_t* workDir, const wchar_t* desc)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) return false;
    link->SetPath(target);
    link->SetWorkingDirectory(workDir);
    link->SetDescription(desc);
    link->SetIconLocation(target, 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(linkPath, TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

void LinkPaths(wchar_t* desktop, wchar_t* menu)
{
    desktop[0] = menu[0] = 0;
    wchar_t d[MAX_PATH], m[MAX_PATH];
    if (KnownFolder(FOLDERID_PublicDesktop, d)) _snwprintf(desktop, MAX_PATH, L"%s\\VM2Backup.lnk", d);
    if (KnownFolder(FOLDERID_CommonPrograms, m)) _snwprintf(menu, MAX_PATH, L"%s\\VM2Backup.lnk", m);
}

// The program's files in a folder (an old location after the path was changed, or the removal); the folder goes if it is empty.
void RemoveFilesIn(const wchar_t* dir, const wchar_t* keepSelf)
{
    static const wchar_t* names[] = { L"VM2Backup.exe", L"VM2BackupService.exe", L"uninstall.cmd", L"VM2Backup-Setup.exe" };
    for (const wchar_t* n : names) {
        wchar_t f[MAX_PATH + 32];
        _snwprintf(f, MAX_PATH + 32, L"%s\\%s", dir, n);
        if (keepSelf && _wcsicmp(f, keepSelf) == 0) continue;
        for (int i = 0; i < 20 && !DeleteFileW(f) && GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES; ++i) Sleep(250);
    }
    RemoveDirectoryW(dir);
}

bool DoInstall()
{
    Say(TR("- Installing VM2Backup -"));
    wchar_t dir[MAX_PATH], oldDir[MAX_PATH];
    wcscpy(dir, g_dir);
    if (!dir[0] && !ProgramDir(dir)) { Say(TR("Error: cannot find the Program Files folder.")); return false; }
    if (!ValidDir(dir)) { Say(TR("Error: the installation folder is not valid (a full path such as C:\\Programs\\VM2Backup is needed).")); return false; }
    bool hadOld = GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", oldDir, MAX_PATH) && oldDir[0] && _wcsicmp(oldDir, dir) != 0;
    wchar_t srcExe[MAX_PATH], srcSvc[MAX_PATH], srcSetup[MAX_PATH], srcUn[MAX_PATH];
    SiblingPath(L"VM2Backup.exe", srcExe); SiblingPath(L"VM2BackupService.exe", srcSvc);
    GetModuleFileNameW(nullptr, srcSetup, MAX_PATH);
    SiblingPath(L"uninstall.cmd", srcUn);
    if (GetFileAttributesW(srcExe) == INVALID_FILE_ATTRIBUTES) { Say(TR("Error: VM2Backup.exe was not found next to the installer.")); return false; }
    if (GetFileAttributesW(srcSvc) == INVALID_FILE_ATTRIBUTES) { Say(TR("Error: VM2BackupService.exe was not found next to the installer.")); return false; }
    Say(TR("Closing the running VM2Backup and stopping its service..."));
    CloseRunningApp();
    Say(TR("Copying the files to %s"), dir);
    SHCreateDirectoryExW(nullptr, dir, nullptr);
    wchar_t dstExe[MAX_PATH], dstSvc[MAX_PATH], dstSetup[MAX_PATH], dstUn[MAX_PATH];
    _snwprintf(dstExe, MAX_PATH, L"%s\\VM2Backup.exe", dir);
    _snwprintf(dstSvc, MAX_PATH, L"%s\\VM2BackupService.exe", dir);
    _snwprintf(dstSetup, MAX_PATH, L"%s\\VM2Backup-Setup.exe", dir);
    _snwprintf(dstUn, MAX_PATH, L"%s\\uninstall.cmd", dir);
    if (!CopyOne(srcExe, dstExe)) { Say(TR("Error: cannot copy %s (error %lu)."), L"VM2Backup.exe", GetLastError()); return false; }
    if (!CopyOne(srcSvc, dstSvc)) { Say(TR("Error: cannot copy %s (error %lu)."), L"VM2BackupService.exe", GetLastError()); return false; }
    wchar_t a[MAX_PATH], b[MAX_PATH];
    wcscpy(a, srcSetup); wcscpy(b, dstSetup);
    if (_wcsicmp(a, b) != 0 && !CopyOne(srcSetup, dstSetup)) { Say(TR("Error: cannot copy %s (error %lu)."), L"VM2Backup-Setup.exe", GetLastError()); return false; }
    if (GetFileAttributesW(srcUn) != INVALID_FILE_ATTRIBUTES) CopyOne(srcUn, dstUn);

    Say(TR("Registering the program in Apps & features."));
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        wchar_t un[MAX_PATH + 24], icon[MAX_PATH + 4];
        _snwprintf(un, MAX_PATH + 24, L"\"%s\" /remove", dstSetup);
        _snwprintf(icon, MAX_PATH + 4, L"%s,0", dstExe);
        SetRegStr(k, L"DisplayName", L"VM2Backup");
        SetRegStr(k, L"DisplayVersion", L"" VM2B_VER_STR);
        SetRegStr(k, L"Publisher", L"" VM2B_AUTHOR_STR);
        SetRegStr(k, L"InstallLocation", dir);
        SetRegStr(k, L"DisplayIcon", icon);
        SetRegStr(k, L"UninstallString", un);
        DWORD one = 1;
        RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        WIN32_FILE_ATTRIBUTE_DATA fd;
        if (GetFileAttributesExW(dstExe, GetFileExInfoStandard, &fd)) {
            DWORD kb = (fd.nFileSizeLow + 1023) / 1024;
            RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&kb, sizeof(kb));
        }
        RegCloseKey(k);
    }

    wchar_t desk[MAX_PATH], menu[MAX_PATH];
    LinkPaths(desk, menu);
    Say(TR("Creating the shortcuts."));
    if (g_wantDesktop && desk[0]) MakeLink(desk, dstExe, dir, L"VM2Backup"); else if (desk[0]) DeleteFileW(desk);
    if (g_wantStart && menu[0]) MakeLink(menu, dstExe, dir, L"VM2Backup"); else if (menu[0]) DeleteFileW(menu);

    {   // the service: registered with automatic start (restarts after a failure); started now when the box is ticked
        DWORD code = 1;
        if (!RunProcess(dstSvc, L"/install", 60000, &code) || code != 0) { Say(TR("Error: the service could not be registered (are you an administrator?).")); return false; }
        if (g_wantAuto) {
            if (RunProcess(dstSvc, L"/start", 90000, &code) && code == 0) Say(TR("The backup service is installed and started."));
            else Say(TR("The backup service is installed but did not start; start it with the \"Start service\" button of the program."));
        } else Say(TR("The backup service is installed (it starts with Windows)."));
    }
    if (hadOld) RemoveFilesIn(oldDir, nullptr);            // the path was changed: the old copy goes
    Say(TR("Done."));
    return true;
}

void ScheduleSelfDelete(const wchar_t* dir)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[2 * MAX_PATH + 160];
    _snwprintf(cmd, 2 * MAX_PATH + 160, L"/c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"", self, dir);
    ShellExecuteW(nullptr, L"open", L"cmd.exe", cmd, nullptr, SW_HIDE);
}

bool DoRemove()
{
    Say(TR("- Removing VM2Backup -"));
    CloseRunningApp();
    wchar_t desk[MAX_PATH], menu[MAX_PATH], dir[MAX_PATH], g_regDir[MAX_PATH];
    g_regDir[0] = 0;
    GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", g_regDir, MAX_PATH);   // read before the key goes
    {   // unregister the service (the installed copy of its exe does it)
        wchar_t svcExe[MAX_PATH]; if (g_regDir[0]) _snwprintf(svcExe, MAX_PATH, L"%s\\VM2BackupService.exe", g_regDir); else ProgramDir(svcExe), wcsncat(svcExe, L"\\VM2BackupService.exe", MAX_PATH - wcslen(svcExe) - 1);
        DWORD code; if (GetFileAttributesW(svcExe) != INVALID_FILE_ATTRIBUTES) RunProcess(svcExe, L"/uninstall", 60000, &code);
    }
    LinkPaths(desk, menu);
    Say(TR("Removing the shortcuts."));
    if (desk[0]) DeleteFileW(desk);
    if (menu[0]) DeleteFileW(menu);
    HKEY k;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    if (g_regDir[0] ? (wcscpy(dir, g_regDir), true) : ProgramDir(dir)) {
        wchar_t self[MAX_PATH], want[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        _snwprintf(want, MAX_PATH, L"%s\\VM2Backup-Setup.exe", dir);
        if (_wcsicmp(self, want) == 0) { g_selfDelete = true; RemoveFilesIn(dir, self); ScheduleSelfDelete(dir); }
        else RemoveFilesIn(dir, nullptr);
    }
    Say(TR("Your settings, the history and the backups you made are kept."));
    Say(TR("Done."));
    return true;
}

DWORD WINAPI Worker(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_ok = g_action == ACT_INSTALL ? DoInstall() : DoRemove();
    if (!g_ok) Say(TR("Failed."));
    CoUninitialize();
    if (!g_silent) PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

// ---- window ----------------------------------------------------------------------------------------------------
bool Installed(wchar_t* ver, wchar_t* loc)
{
    ver[0] = loc[0] = 0;
    return GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", ver, 64) &&
           (GetRegStr(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", loc, MAX_PATH) || true);
}

void Refresh()
{
    wchar_t ver[64], loc[MAX_PATH], text[MAX_PATH + 100];
    bool inst = Installed(ver, loc);
    if (inst) _snwprintf(text, MAX_PATH + 100, TR("Installed: version %s, %s"), ver, loc); else wcscpy(text, TR("Not installed"));
    SetWindowTextW(Ctl(IDC_STATE), text);
    SetWindowTextW(Ctl(IDC_INSTALL), !inst ? TR("Install") : wcscmp(ver, L"" VM2B_VER_STR) == 0 ? TR("Reinstall") : TR("Update"));   // same version: Reinstall
    EnableWindow(Ctl(IDC_REMOVE), inst);
    EnableWindow(Ctl(IDC_LAUNCH), inst);
    SetWindowTextW(Ctl(IDC_TITLE), L"VM2Backup");
    wchar_t sub[200];
    _snwprintf(sub, 200, L"%s — %s", L"" VM2B_VER_STR, TR("Backup of virtual machines to a folder, FTP, FTP over TLS and SCP"));
    SetWindowTextW(Ctl(IDC_SUB), sub);
    SetWindowTextW(Ctl(IDC_DESKTOP), TR("Desktop shortcut"));
    SetWindowTextW(Ctl(IDC_START), TR("Start menu shortcut"));
    SetWindowTextW(Ctl(IDC_AUTO), TR("Start the backup service now"));
    SetWindowTextW(Ctl(IDC_REMOVE), TR("Remove"));
    SetWindowTextW(Ctl(IDC_LAUNCH), TR("Launch"));
    SetWindowTextW(Ctl(IDC_CLOSE), TR("Close"));
    SetWindowTextW(Ctl(IDC_LANGLBL), TR("Language:"));
    SetWindowTextW(Ctl(IDC_PATHLBL), TR("Install to:"));
    SetWindowTextW(Ctl(IDC_BROWSE), TR("Browse..."));
}

void Busy(bool busy)
{
    int ids[] = { IDC_INSTALL, IDC_REMOVE, IDC_LAUNCH, IDC_DESKTOP, IDC_START, IDC_AUTO, IDC_LANG, IDC_PATH, IDC_BROWSE };
    for (int id : ids) EnableWindow(Ctl(id), !busy);
    if (!busy) Refresh();
}

void Start(int action)
{
    if (action == ACT_REMOVE && MessageBoxW(g_wnd, TR("Remove VM2Backup from this computer?\n\nYour settings, the history and the backups you made are kept."), L"VM2Backup", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_wantDesktop = IsDlgButtonChecked(g_wnd, IDC_DESKTOP) == BST_CHECKED;
    g_wantStart = IsDlgButtonChecked(g_wnd, IDC_START) == BST_CHECKED;
    g_wantAuto = IsDlgButtonChecked(g_wnd, IDC_AUTO) == BST_CHECKED;
    if (action == ACT_INSTALL) GetWindowTextW(Ctl(IDC_PATH), g_dir, MAX_PATH);
    g_action = action;
    Busy(true);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, Sc(x), Sc(y), Sc(w), Sc(h), g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE);
    return c;
}

int CALLBACK BrowseInit(HWND h, UINT m, LPARAM, LPARAM data)
{
    if (m == BFFM_INITIALIZED && data) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

void Browse()
{
    wchar_t cur[MAX_PATH], pick[MAX_PATH];
    GetWindowTextW(Ctl(IDC_PATH), cur, MAX_PATH);
    BROWSEINFOW bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_wnd; bi.pszDisplayName = pick; bi.lpszTitle = TR("Select the folder to install VM2Backup to");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = BrowseInit; bi.lParam = (LPARAM)cur;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (SHGetPathFromIDListW(pidl, pick)) {
        size_t n = wcslen(pick);
        // a folder that already holds other things gets its own sub-folder (the removal deletes only our files, but still)
        bool isRoot = n <= 3, ours = n >= 12 && _wcsicmp(pick + n - 12, L"VM2Backup") == 0;
        if (!ours && !isRoot) wcsncat(pick, L"\\VM2Backup", MAX_PATH - n - 1);
        else if (isRoot) _snwprintf(pick, MAX_PATH, L"%sVM2Backup", pick);
        SetWindowTextW(Ctl(IDC_PATH), pick);
    }
    CoTaskMemFree(pidl);
}

void Launch()
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    if (!InstalledDir(dir)) return;
    _snwprintf(exe, MAX_PATH, L"%s\\VM2Backup.exe", dir);
    // The installer runs elevated; Explorer starts the program with the user's rights.
    ShellExecuteW(nullptr, L"open", L"explorer.exe", exe, nullptr, SW_SHOWNORMAL);
}

void BuildControls()
{
    HWND hw = g_wnd;
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    NONCLIENTMETRICSW ncm; memset(&ncm, 0, sizeof(ncm)); ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, Dpi96());
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_SEMIBOLD; lf.lfHeight = lf.lfHeight * 2;
    g_fontTitle = CreateFontIndirectW(&lf);
    Add(L"STATIC", L"", SS_LEFT, 20, 14, 400, 34, IDC_TITLE, g_fontTitle);
    Add(L"STATIC", L"", SS_LEFT, 20, 50, 560, 20, IDC_SUB);
    Add(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 20, 78, 560, 20, IDC_STATE);
    Add(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 20, 106, 110, 24, IDC_PATHLBL);
    Add(L"EDIT", g_dir, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 134, 106, 340, 24, IDC_PATH);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 482, 105, 98, 26, IDC_BROWSE);
    SendMessageW(Ctl(IDC_PATH), EM_SETLIMITTEXT, 200, 0);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 142, 330, 22, IDC_DESKTOP);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 168, 330, 22, IDC_START);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, 20, 194, 330, 22, IDC_AUTO);
    CheckDlgButton(hw, IDC_DESKTOP, g_wantDesktop ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_START, g_wantStart ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hw, IDC_AUTO, g_wantAuto ? BST_CHECKED : BST_UNCHECKED);
    Add(L"STATIC", L"", SS_RIGHT, 360, 146, 100, 20, IDC_LANGLBL);
    HWND lg = Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 466, 142, 114, 300, IDC_LANG);
    for (int i = 0; i < LangCount(); ++i) SendMessageW(lg, CB_ADDSTRING, 0, (LPARAM)LangName(i));
    SendMessageW(lg, CB_SETCURSEL, g_lang, 0);
    Add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 20, 228, 560, 128, IDC_LOG);
    // Four buttons in one row with gaps: 3 x 136 + 1 x 100 + 3 x 10 = 538 <= 560 (Close is flush right).
    int bw = 136, gap = 10, y = 368;
    Add(L"BUTTON", L"", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, y, bw, 32, IDC_INSTALL);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20 + bw + gap, y, bw, 32, IDC_REMOVE);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 20 + 2 * (bw + gap), y, bw, 32, IDC_LAUNCH);
    Add(L"BUTTON", L"", WS_TABSTOP | BS_PUSHBUTTON, 580 - 100, y, 100, 32, IDC_CLOSE);
    Refresh();
}

// The window moved to a monitor with another scale: the controls are made again at the new size (the log text,
// the boxes and the language stay).
void Rescale(UINT dpi, const RECT* suggested)
{
    wchar_t* log = nullptr;
    int n = GetWindowTextLengthW(Ctl(IDC_LOG));
    if (n > 0) { log = (wchar_t*)malloc((n + 1) * sizeof(wchar_t)); if (log) GetWindowTextW(Ctl(IDC_LOG), log, n + 1); }
    g_wantDesktop = IsDlgButtonChecked(g_wnd, IDC_DESKTOP) == BST_CHECKED;
    g_wantStart = IsDlgButtonChecked(g_wnd, IDC_START) == BST_CHECKED;
    g_wantAuto = IsDlgButtonChecked(g_wnd, IDC_AUTO) == BST_CHECKED;
    GetWindowTextW(Ctl(IDC_PATH), g_dir, MAX_PATH);
    g_dpi = dpi;
    HWND c;
    while ((c = GetWindow(g_wnd, GW_CHILD)) != nullptr) DestroyWindow(c);
    BuildControls();
    if (log) { SetWindowTextW(Ctl(IDC_LOG), log); free(log); }
    if (suggested)
        SetWindowPos(g_wnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK Proc(HWND hw, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = hw;
        BuildControls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL: Start(ACT_INSTALL); break;
        case IDC_REMOVE: Start(ACT_REMOVE); break;
        case IDC_LAUNCH: Launch(); break;
        case IDC_BROWSE: Browse(); break;
        case IDC_CLOSE: DestroyWindow(hw); break;
        case IDC_LANG:
            if (HIWORD(wp) == CBN_SELCHANGE) { g_lang = (int)SendMessageW(Ctl(IDC_LANG), CB_GETCURSEL, 0, 0); LangSelect(g_lang + 1); Refresh(); }
            break;
        }
        return 0;
    case WM_DPICHANGED: Rescale(HIWORD(wp), (const RECT*)lp); return 0;
    case WM_APP_LOG: {
        wchar_t* line = (wchar_t*)lp;
        HWND e = Ctl(IDC_LOG);
        int len = GetWindowTextLengthW(e);
        SendMessageW(e, EM_SETSEL, len, len);
        if (len) SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)line);
        free(line);
        return 0; }
    case WM_APP_DONE:
        if (g_worker) { WaitForSingleObject(g_worker, 2000); CloseHandle(g_worker); g_worker = nullptr; }
        Busy(false);
        if (g_ok && g_action == ACT_INSTALL) SetFocus(Ctl(IDC_LAUNCH));
        return 0;
    case WM_CLOSE:
        if (g_worker) return 0;                // not in the middle of copying
        DestroyWindow(hw); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show)
{
    g_inst = inst;
    AppLogOpen(L"setup");
    {
        typedef UINT(WINAPI* GetDpiFn)();
        GetDpiFn sys = (GetDpiFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
        if (sys) g_dpi = sys();
        else { HDC dc = GetDC(nullptr); g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    g_lang = LangAutoIndex();
    LangSelect(g_lang + 1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool doInstall = cmd && wcsstr(cmd, L"/install"), doRemove = cmd && wcsstr(cmd, L"/remove");
    g_silent = cmd && wcsstr(cmd, L"/silent");
    if (cmd && wcsstr(cmd, L"/desktop-")) g_wantDesktop = false;
    if (cmd && wcsstr(cmd, L"/startmenu-")) g_wantStart = false;
    if (cmd && wcsstr(cmd, L"/nostart")) g_wantAuto = false;
    InstalledDir(g_dir);                                    // the default: where it is installed now, else Program Files
    if (cmd) {                                               // /dir "C:\\Some\\Folder"  (quotes optional without spaces)
        const wchar_t* d = wcsstr(cmd, L"/dir");
        if (d && (d[4] == L' ' || d[4] == L'=' || d[4] == L':')) {
            d += 5; while (*d == L' ') ++d;
            bool q = *d == L'"'; if (q) ++d;
            size_t n = 0;
            while (d[n] && (q ? d[n] != L'"' : d[n] != L' ') && n < MAX_PATH - 1) { g_dir[n] = d[n]; ++n; }
            g_dir[n] = 0;
        }
    }
    if (g_silent && (doInstall || doRemove)) {
        g_action = doInstall ? ACT_INSTALL : ACT_REMOVE;
        Worker(nullptr);
        return g_ok ? 0 : 1;
    }
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = Proc; wc.hInstance = inst; wc.lpszClassName = L"VM2BackupSetup";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hIcon = g_icon;
    RegisterClassW(&wc);
    RECT r = { 0, 0, Sc(600), Sc(416) };
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND w = CreateWindowExW(0, L"VM2BackupSetup", L"VM2Backup", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!w) return 1;
    ShowWindow(w, show);
    UpdateWindow(w);
    if (doInstall) PostMessageW(w, WM_COMMAND, IDC_INSTALL, 0);
    else if (doRemove) PostMessageW(w, WM_COMMAND, IDC_REMOVE, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return g_ok || (!doInstall && !doRemove) ? 0 : 1;
}
