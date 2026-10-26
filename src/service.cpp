// VM2BackupService.exe — the Windows service of VM2Backup (the counterpart of VM Explorer's VMXWinService): runs the scheduled
// tasks and the jobs the program asks for, also when nobody is logged on. It is a thin shell around ServiceCore (svccore.cpp).
//
//   VM2BackupService.exe                 started by the Service Control Manager; started by hand it runs in a console window
//   VM2BackupService.exe /console        run in this console (Ctrl+C stops it) [/data <folder>] [/pipe <name>] [/tick <ms>]
//   VM2BackupService.exe /install        register the service (automatic start, restarts after a failure)   [/manual]
//   VM2BackupService.exe /uninstall      stop and remove it
//   VM2BackupService.exe /start | /stop
//   VM2BackupService.exe /reset-web      [/data <folder>] [/pipe <name>]  the service must be stopped; administrator rights; web site settings back to the
//                                        defaults (on, 127.0.0.1:8443, no 2FA requirement) and the user admin / admin (the other users stay)
// The exit code is 0 on success.
#include "svccore.h"
#include "webauth.h"
#include "ipc.h"
#include "applog.h"
#include "version.h"
#include "winutil.h"
#include <stdio.h>

using namespace vm2b;

static const wchar_t* kServiceName = L"VM2BackupService";

static SERVICE_STATUS_HANDLE g_statusHandle;
static SERVICE_STATUS g_status;
static HANDLE g_stopEvent;
static ServiceOptions g_opt;
// service.shutdown: the answer to the caller must leave the pipe before the core is torn down (a loaded Windows 11 guest lost it: the call failed), so the stop is a moment later.
static DWORD WINAPI ShutdownLater(LPVOID) { Sleep(150); if (g_stopEvent) SetEvent(g_stopEvent); return 0; }
static void OnShutdown() { HANDLE h = CreateThread(nullptr, 0, ShutdownLater, nullptr, 0, nullptr); if (h) CloseHandle(h); else if (g_stopEvent) SetEvent(g_stopEvent); }

static void SetState(DWORD state, DWORD wait = 0)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    g_status.dwWaitHint = wait;
    static DWORD check = 1;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : check++;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

static DWORD WINAPI Control(DWORD code, DWORD, LPVOID, LPVOID)
{
    switch (code) {
    case SERVICE_CONTROL_STOP: case SERVICE_CONTROL_SHUTDOWN:
        SetState(SERVICE_STOP_PENDING, 30000);
        SetEvent(g_stopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE: return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static volatile LONG g_stopped;
static DWORD WINAPI KeepStopPending(LPVOID)
{
    while (!g_stopped) {
        for (int i = 0; i < 40 && !g_stopped; ++i) Sleep(100);
        if (!g_stopped) SetState(SERVICE_STOP_PENDING, 30000);
    }
    return 0;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, Control, nullptr);
    if (!g_statusHandle) return;
    SetState(SERVICE_START_PENDING, 15000);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ServiceCore core; Str err;
    if (!core.start(g_opt, err)) {
        Log(L"the service cannot start: %s", Wide(err).c_str());
        // SCM показывает только "Incorrect function" (код 1): причину кладём ещё и в журнал событий Windows (Приложение), чтобы администратор её увидел
        { HANDLE es = RegisterEventSourceW(nullptr, L"VM2BackupService"); if (es) { Wide m(Str("VM2Backup service cannot start: ") + err); const wchar_t* strs[1] = { m.c_str() }; ReportEventW(es, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strs, nullptr); DeregisterEventSource(es); } }
        g_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR; g_status.dwServiceSpecificExitCode = 1;
        SetState(SERVICE_STOPPED);
        return;
    }
    SetState(SERVICE_RUNNING);
    WaitForSingleObject(g_stopEvent, INFINITE);
    // Stopping can take a while (running jobs are asked to stop and keep their journals): the SCM is told every few seconds that we are alive, else a stop
    // that exceeds the first wait hint is reported as "did not respond".
    HANDLE keepAlive = CreateThread(nullptr, 0, KeepStopPending, nullptr, 0, nullptr);
    core.stop();                                    // running jobs keep their journals and go on after the next start
    InterlockedExchange(&g_stopped, 1);
    if (keepAlive) { WaitForSingleObject(keepAlive, 10000); CloseHandle(keepAlive); }
    SetState(SERVICE_STOPPED);
}

static BOOL WINAPI CtrlC(DWORD) { if (g_stopEvent) SetEvent(g_stopEvent); return TRUE; }

static int RunConsole()
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlC, TRUE);
    ServiceCore core; Str err;
    if (!core.start(g_opt, err)) { fprintf(stderr, "cannot start: %s\n", err.c_str()); return 1; }
    printf("VM2Backup service %s running; data in %s. Ctrl+C stops it.\n", VM2B_VER_STR, core.config().dataDir().c_str());
    fflush(stdout);
    WaitForSingleObject(g_stopEvent, INFINITE);
    core.stop();
    return 0;
}

// ---- installing ------------------------------------------------------------------------------------------------
static int Install(bool manual)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { fprintf(stderr, "cannot open the service manager (%lu): run as administrator\n", (unsigned long)GetLastError()); return 1; }
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 16]; _snwprintf(cmd, MAX_PATH + 16, L"\"%s\" /service", self);
    SC_HANDLE svc = nullptr;
    for (int tries = 0; tries < 100; ++tries) {                                  // a service that was just removed may still be "marked for deletion": wait for it
        svc = CreateServiceW(scm, kServiceName, L"VM2Backup Service", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             manual ? SERVICE_DEMAND_START : SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (svc || GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) break;
        Sleep(200);
    }
    int rc = 0;
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {                       // an update: the same service, the new file
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc) ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, manual ? SERVICE_DEMAND_START : SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd, nullptr, nullptr, nullptr, nullptr, nullptr, L"VM2Backup Service");
    }
    if (!svc) { fprintf(stderr, "cannot create the service (%lu)\n", (unsigned long)GetLastError()); rc = 1; }
    else {
        SERVICE_DESCRIPTIONW d = { (LPWSTR)L"Runs the scheduled virtual machine backups of VM2Backup (also when no one is logged on)." };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);
        SC_ACTION acts[3] = { { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 300000 } };
        SERVICE_FAILURE_ACTIONSW fa = { 86400, nullptr, nullptr, 3, acts };
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        if (!manual) { SERVICE_DELAYED_AUTO_START_INFO di = { TRUE }; ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &di); }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return rc;
}

static bool WaitState(SC_HANDLE svc, DWORD want, int ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        SERVICE_STATUS st;
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == want) return true;
        if ((int)(GetTickCount() - t0) > ms) return false;
        Sleep(200);
    }
}

static int StartStop(bool start, bool quiet = false)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    int rc = 0;
    if (!svc) rc = quiet ? 0 : 1;                                                 // nothing to stop
    else {
        if (start) { if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) rc = 1; else if (!WaitState(svc, SERVICE_RUNNING, 30000)) rc = 1; }
        else { SERVICE_STATUS st; ControlService(svc, SERVICE_CONTROL_STOP, &st); if (!WaitState(svc, SERVICE_STOPPED, 60000)) rc = quiet ? 0 : 1; }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return rc;
}

static int Uninstall()
{
    StartStop(false, true);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { fprintf(stderr, "cannot open the service manager (%lu): run as administrator\n", (unsigned long)GetLastError()); return 1; }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, DELETE);
    int rc = 0;
    if (svc) { if (!DeleteService(svc) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) rc = 1; CloseServiceHandle(svc); }
    // the service disappears when the last handle to it is closed: wait until it really is gone (a caller may install it again at once)
    for (int i = 0; i < 100; ++i) {
        SC_HANDLE probe = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
        if (!probe) break;
        CloseServiceHandle(probe); Sleep(100);
    }
    CloseServiceHandle(scm);
    return rc;
}

static bool IsAdministrator()
{
    BOOL member = FALSE; PSID sid = nullptr; SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) { if (!CheckTokenMembership(nullptr, sid, &member)) member = FALSE; FreeSid(sid); }
    return member != FALSE;
}

// The recovery path when nobody can sign in to the web site (forgotten password, lost 2FA, a bind that hides the site): resets web.json's
// settings and the user admin; refuses while the service runs (it holds web.json) and without administrator rights.
static int ResetWeb()
{
    if (!IsAdministrator()) { fprintf(stderr, "run this as an administrator (elevated command prompt)\n"); return 1; }
    Str e;
    { IpcClient c; if (c.connect(g_opt.pipeName.empty() ? Str(kDefaultPipeName) : g_opt.pipeName, 300, e)) { fprintf(stderr, "the VM2Backup service is running: stop it first (net stop VM2BackupService), then run /reset-web again and start the service\n"); return 1; } }
    Str dir = g_opt.dataDir.empty() ? ServiceCore::DefaultDataDir() : g_opt.dataDir;
    WebAuth wa; Str err;
    if (!wa.open(dir, err, true)) {
        Str f = JoinPath(dir, "web.json");
        if (FileExists(f) && err.find("damaged") >= 0) {                                   // unreadable: keep it aside, start a new one (the certificate is made again)
            Str bak = f + ".damaged"; MoveFileExW(FilePath(f), FilePath(bak), MOVEFILE_REPLACE_EXISTING); printf("web.json was damaged: kept as %s\n", bak.c_str());
            if (!wa.open(dir, err, true)) { fprintf(stderr, "cannot reset: %s\n", err.c_str()); return 1; }
        } else { fprintf(stderr, "cannot reset: %s\n", err.c_str()); return 1; }
    }
    Str report; if (!wa.resetDefaults(report, err)) { fprintf(stderr, "cannot reset: %s\n", err.c_str()); return 1; }
    printf("%s\nData folder: %s\nStart the service and open https://127.0.0.1:8443/ - sign in as admin with the password admin and change it.\n", report.c_str(), dir.c_str());
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    bool resetWeb = false, console = false, install = false, uninstall = false, start = false, stop = false, manual = false, service = false;
    char buf[1024];
    for (int i = 1; i < argc; ++i) {
        Str a = FromWide(argv[i]);
        auto next = [&]() -> Str { return i + 1 < argc ? FromWide(argv[++i]) : Str(); };
        if (a == "/console" || a == "--console") console = true;
        else if (a == "/service") service = true;
        else if (a == "/install") install = true;
        else if (a == "/uninstall" || a == "/remove") uninstall = true;
        else if (a == "/start") start = true;
        else if (a == "/stop") stop = true;
        else if (a == "/reset-web") resetWeb = true;
        else if (a == "/manual") manual = true;
        else if (a == "/data") g_opt.dataDir = next();
        else if (a == "/pipe") g_opt.pipeName = next();
        else if (a == "/tick") g_opt.tickMs = atoi(next().c_str());
        else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    (void)buf;
    g_opt.onShutdown = OnShutdown;
    AppLogOpen(L"service");
    if (resetWeb) return ResetWeb();
    if (install) return Install(manual);
    if (uninstall) return Uninstall();
    if (start) return StartStop(true);
    if (stop) return StartStop(false);
    if (console) return RunConsole();
    SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)kServiceName, ServiceMain }, { nullptr, nullptr } };
    if (StartServiceCtrlDispatcherW(table)) return 0;
    if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT && !service) return RunConsole();   // started from a command line: not a service start
    return 1;
}
