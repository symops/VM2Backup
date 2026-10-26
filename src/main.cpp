// VM2Backup.exe — the control panel. The work is done in the web interface of the service (https://127.0.0.1:8443/); this small
// window only shows whether the service runs and what the web site does, starts / stops / restarts the service, opens the web
// interface and the log folder and sets the language.
//   VM2Backup.exe [/pipe <name>] [/selftest [ms]] [/guitest <result file>]
#include "panel.h"
#include "panelutil.h"
#include "version.h"
#include "applog.h"
#include "winutil.h"
#include <stdio.h>
#include <objbase.h>
#include <shellapi.h>

using namespace ui;
using namespace vm2b;

PanelHooks g_panelHooks = { nullptr, nullptr };

namespace {

int g_selftestMs = 600;                           // how long a dialog stays open in the self test (/selftest <ms>)
const wchar_t kServiceName[] = L"VM2BackupService";
enum { ID_TIMER = 1 };
enum { SCM_UNKNOWN = -2, SCM_MISSING = -1, SCM_STOPPED = 0, SCM_RUNNING = 1, SCM_PENDING = 2 };

// ---- the service control manager ------------------------------------------------------------------------------------------------
int ScmState()
{
    if (g_panelHooks.service) return SCM_STOPPED;                          // a test: "installed"; whether it runs is told by the pipe
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!scm) return SCM_UNKNOWN;
    int r = SCM_UNKNOWN;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (!svc) { r = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? SCM_MISSING : SCM_UNKNOWN; }
    else {
        SERVICE_STATUS st;
        if (QueryServiceStatus(svc, &st)) r = st.dwCurrentState == SERVICE_RUNNING ? SCM_RUNNING : st.dwCurrentState == SERVICE_STOPPED ? SCM_STOPPED : SCM_PENDING;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm); return r;
}

bool ScmControl(bool start)
{
    if (g_panelHooks.service) return g_panelHooks.service(start ? PANEL_SVC_START : PANEL_SVC_STOP);
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS); bool ok = false;
    if (svc) {
        if (start) ok = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        else {
            SERVICE_STATUS st; ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) != 0 || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
            // a stop is done when the process is gone: wait (a running backup is paused first), a start may follow
            for (int i = 0; ok && i < 150; ++i) { if (!QueryServiceStatus(svc, &st) || st.dwCurrentState == SERVICE_STOPPED) break; Sleep(200); }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm); return ok;
}

struct SvcJob { HWND wnd; int op; };

DWORD WINAPI SvcThread(LPVOID p)
{
    SvcJob* j = (SvcJob*)p; int fail = 0;
    if (j->op == PANEL_SVC_STOP || j->op == PANEL_SVC_RESTART) { if (!ScmControl(false)) fail = 1; }
    if (!fail && (j->op == PANEL_SVC_START || j->op == PANEL_SVC_RESTART)) { if (!ScmControl(true)) fail = 2; }
    PostMessageW(j->wnd, WM_APP_SVCDONE, (WPARAM)fail, 0);
    delete j; return 0;
}

// ---- what the panel shows --------------------------------------------------------------------------------------------------------
struct PanelView {
    bool online = false;                          // the service answers on its pipe
    int scm = SCM_STOPPED;                        // what the service manager says (when it does not answer)
    Str version, dataDir;
    int web = 0;                                  // 0 = off, 1 = listening, 2 = enabled but not listening
    Str host; long long port = 0;                 // the first listening address (IPv6 in brackets) and the port
};

class Panel : public Form {
public:
    void onInit() override { build(); poll(); SetTimer(hwnd(), ID_TIMER, 1000, nullptr); }

    void build() {
        group(12, 8, 536, 156, TR("Service"));
        label(28, 32, 118, 20, TR("Service:")); label(150, 32, 386, 20, L"", PANEL_ID_STATE);
        label(28, 56, 118, 20, TR("Version:")); label(150, 56, 386, 20, L"", PANEL_ID_VER);
        label(28, 80, 118, 20, TR("Data folder:")); label(150, 80, 386, 38, L"", PANEL_ID_DATA);
        label(28, 126, 118, 20, TR("Web site:")); label(150, 126, 386, 20, L"", PANEL_ID_WEB);
        button(PANEL_ID_START, 12, 176, 172, 30, TR("Start service")); button(PANEL_ID_STOP, 194, 176, 172, 30, TR("Stop service")); button(PANEL_ID_RESTART, 376, 176, 172, 30, TR("Restart service"));
        button(PANEL_ID_OPENWEB, 12, 216, 263, 30, TR("Open web interface"), true); button(PANEL_ID_LOGS, 285, 216, 263, 30, TR("Open logs folder"));
        label(12, 266, 112, 20, TR("Language:"), PANEL_ID_LANGLBL); combo(PANEL_ID_LANG, 128, 262, 220); button(IDCANCEL, 448, 260, 100, 30, TR("Exit"));
        comboAdd(PANEL_ID_LANG, U8(TR("Windows language")), 0); for (int i = 0; i < LangCount(); ++i) comboAdd(PANEL_ID_LANG, U8(LangName(i)), i + 1);
        comboSetData(PANEL_ID_LANG, m_lang);
        render(m_view);
    }

    // ---- show a state
    void render(const PanelView& v) {
        if (!ctl(PANEL_ID_STATE)) return;
        setText(PANEL_ID_STATE, v.online ? U8(TR("Running")) : v.scm == SCM_MISSING ? U8(TR("Not installed")) : U8(TR("Stopped")));
        setText(PANEL_ID_VER, v.online && !v.version.empty() ? v.version : Str("-"));
        setText(PANEL_ID_DATA, v.online && !v.dataDir.empty() ? v.dataDir : Str("-"));
        Str web = v.web == 1 ? Str::fmt(U8(TR("Listening on %s:%lld")).c_str(), v.host.c_str(), v.port) : v.web == 2 ? U8(TR("Enabled, but not listening")) : U8(TR("Off"));
        setText(PANEL_ID_WEB, web);
        bool installed = v.online || v.scm != SCM_MISSING;
        enable(PANEL_ID_START, !m_busy && !v.online && installed); enable(PANEL_ID_STOP, !m_busy && v.online); enable(PANEL_ID_RESTART, !m_busy && installed);
    }

    void poll() {
        PanelView v; Json st; Str err;
        v.online = g_client.call("state", Json::object(), st, err);
        if (v.online) {
            v.version = st.get("version").str(); v.dataDir = st.get("data_dir").str();
            if (m_polls > 0 && !m_wasOnline && m_adopt) { int l = (int)st.get("settings").get("language").integer(0); if (l != m_lang) { m_lang = l; adoptLanguage(); } }     // the service came back: its language setting is the truth
            Json w; Str e2;
            if (g_client.call("web.status", Json::object(), w, e2)) { m_web = w; v.web = !w.get("enabled").boolean() ? 0 : w.get("listening").boolean() ? 1 : 2; v.host = PanelUrlHost(w.get("address").str()); v.port = (long long)w.get("port").integer(8443); }
        } else { v.scm = ScmState(); m_web = Json::object(); }
        m_wasOnline = v.online; ++m_polls;
        m_view = v; render(v);
    }

    void adoptLanguage() { LangSelect(m_lang); rebuild(); }

    void rebuild() { destroyControls(); build(); InvalidateRect(hwnd(), nullptr, TRUE); }

    // ---- actions
    void runService(int op) {
        if (m_busy) return;
        m_busy = true; render(m_view);
        SvcJob* j = new SvcJob{ hwnd(), op };
        HANDLE t = CreateThread(nullptr, 0, SvcThread, j, 0, nullptr);
        if (t) CloseHandle(t); else { delete j; m_busy = false; render(m_view); }
    }

    void openThing(const Str& what) {
        Vec<wchar_t> w = W(what);
        if (g_panelHooks.open) { g_panelHooks.open(w.data()); return; }
        INT_PTR r = (INT_PTR)ShellExecuteW(hwnd(), L"open", w.data(), nullptr, nullptr, SW_SHOWNORMAL);
        if (r <= 32) message(Str::fmt(U8(TR("Could not open: %s")).c_str(), what.c_str()), true);
    }

    void openWeb() {
        if (!m_view.online) { message(U8(TR("The VM2Backup service is not running, so there is no web site. Start the service first.")), true); return; }
        Json w; Str err;
        if (!g_client.call("web.status", Json::object(), w, err)) { message(ServiceError(err), true); return; }
        Str url; PanelWebResult r = PanelWebUrl(w, url);
        if (r == PANEL_WEB_OFF) message(U8(TR("The web site is off. Switch it on in the web interface, or reset it with VM2BackupService.exe /reset-web.")), true);
        else if (r == PANEL_WEB_NOT_LISTENING) message(U8(TR("The web site is enabled, but not listening. Check the certificate and the port in the web interface.")), true);
        else openThing(url);
    }

    bool onCommand(int id, int code) override {
        if (code != BN_CLICKED && !(id == PANEL_ID_LANG && code == CBN_SELCHANGE)) return false;
        switch (id) {
        case PANEL_ID_START: runService(PANEL_SVC_START); return true;
        case PANEL_ID_STOP: if (confirm(U8(TR("Stop the service? Running backups are paused and continue after the next start.")))) runService(PANEL_SVC_STOP); return true;
        case PANEL_ID_RESTART: if (confirm(U8(TR("Restart the service? Running backups are paused and continue after the restart.")))) runService(PANEL_SVC_RESTART); return true;
        case PANEL_ID_OPENWEB: openWeb(); return true;
        case PANEL_ID_LOGS: { const wchar_t* d = AppLogDir(); if (d && *d) openThing(U8(d)); return true; }
        case PANEL_ID_LANG: {
            int l = (int)comboData(PANEL_ID_LANG); Json a = Json::object(), r; Str err; a["language"] = (i64)l;
            if (!g_client.call("settings.set", a, r, err) && g_client.online()) { message(ServiceError(err), true); comboSetData(PANEL_ID_LANG, m_lang); return true; }
            m_lang = l; adoptLanguage(); return true; }
        case IDCANCEL: PostMessageW(hwnd(), WM_CLOSE, 0, 0); return true;
        }
        return false;
    }

    LRESULT onMessage(UINT m, WPARAM wp, LPARAM lp, bool& handled) override {
        if (m == WM_APP_SVCDONE) {
            handled = true; m_busy = false;
            if (wp == 1) message(U8(TR("The service could not be stopped (are you an administrator?).")), true);
            else if (wp == 2) message(U8(TR("The service could not be started (is it installed? are you an administrator?).")), true);
            poll(); return 0;
        }
        if (m == WM_APP_REFRESH) { handled = true; poll(); return 0; }
        return 0;
    }

    void onTimer(UINT_PTR id) override {
        if (id != ID_TIMER) return;
        poll();
        if (m_selftest) selftestStep();
    }

    // ---- self test: the panel in every state, then quit
    void startSelftest() {
        m_selftest = true; if (Form::s_layoutLog) checkLayout(*Form::s_layoutLog); Form::s_autoCloseMs = g_selftestMs;
        wchar_t st[16]; if (GetEnvironmentVariableW(L"VM2B_SELFTEST_STAGE", st, 16)) m_stage = _wtoi(st); if (GetEnvironmentVariableW(L"VM2B_SELFTEST_MS", st, 16) && _wtoi(st) > 0) Form::s_autoCloseMs = _wtoi(st);
    }
    void layoutVariants() {
        // the texts of every state, the longest ones included, must fit their controls in every language
        PanelView a; a.online = true; a.version = "1.0.298.325"; a.dataDir = "C:\\Users\\Administrator.CORP\\AppData\\Roaming\\Some Long Folder Name\\VM2Backup\\Data"; a.web = 1; a.host = "[fe80::1234:5678:9abc:def0%25eth0]"; a.port = 65535;
        PanelView b; b.scm = SCM_STOPPED; PanelView c; c.scm = SCM_MISSING; PanelView d = a; d.web = 2; PanelView e = a; e.web = 0;
        const PanelView* all[] = { &a, &b, &c, &d, &e };
        for (const PanelView* v : all) { render(*v); fitAll(); if (Form::s_layoutLog) checkLayout(*Form::s_layoutLog); }
        render(m_view);
    }
    void selftestStep() {
        if (m_stepBusy) return; m_stepBusy = true;
        switch (m_stage++) {
        case 0: layoutVariants(); break;
        default: printf("selftest: panel shown in %d states, no crash\n", 5); fflush(stdout); PostMessageW(hwnd(), WM_CLOSE, 0, 0); m_exitCode = 0; break;
        }
        m_stepBusy = false;
    }
    int exitCode() const { return m_exitCode; }
    void setLang(int l, bool adopt) { m_lang = l; m_adopt = adopt; }

    PanelView m_view; Json m_web; int m_polls = 0; bool m_adopt = true, m_wasOnline = false, m_busy = false, m_selftest = false, m_stepBusy = false; int m_stage = 0, m_lang = 0, m_exitCode = 1;
};

} // namespace

void GuiTestStart(HWND mainWnd, const Str& resultFile);          // guitest.cpp
int GuiTestFailures();
void GuiTestWait();                                              // waits until the script has written its result (Esc ends the panel before it can)

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int show)
{
    AppLogOpen(L"program");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitUi();
    bool selftest = false; Str guitestFile;
    if (cmd) {
        const wchar_t* p = wcsstr(cmd, L"/pipe");
        if (p) { p += 5; while (*p == L' ') ++p; Str name; while (*p && *p != L' ') { name += (char)*p; ++p; } if (!name.empty()) g_pipeName = name; }
        const wchar_t* gt = wcsstr(cmd, L"/guitest");
        if (gt) { gt += 8; while (*gt == L' ') ++gt; Str f; while (*gt && *gt != L' ') { f += (char)*gt; ++gt; } guitestFile = f; }
        const wchar_t* st = wcsstr(cmd, L"/selftest"); selftest = st != nullptr;
        if (st) { st += 9; while (*st == L' ') ++st; int ms = _wtoi(st); if (ms > 0) g_selftestMs = ms; }
    }
    LangSelect(0);
    int lang = 0;
    if (!g_client.connect(selftest ? 8000 : 1500)) { /* the panel opens anyway and says that the service is not running */ }
    { Json st; Str e; if (g_client.call("state", Json::object(), st, e)) { lang = (int)st.get("settings").get("language").integer(0); LangSelect(lang); } }
    { wchar_t lg[16]; if (selftest && GetEnvironmentVariableW(L"VM2B_SELFTEST_LANG", lg, 16)) { lang = _wtoi(lg); LangSelect(lang); } }          // 1 = English, 2 = Russian ... (screenshots)
    static Vec<Str> layoutIssues; { wchar_t lo[4]; if (selftest && GetEnvironmentVariableW(L"VM2B_LAYOUT_OUT", lo, 4)) Form::s_layoutLog = &layoutIssues; }
    if (!guitestFile.empty()) { lang = 1; LangSelect(1); }                // the script finds controls by their English texts
    Panel f; f.setLang(lang, guitestFile.empty() && !selftest); f.setFixedSize(true);
    if (!f.createTop(Str("VM2Backup ") + VM2B_VER_STR, 560, 304, selftest ? SW_SHOWNORMAL : show)) return 1;
    if (selftest) f.startSelftest();
    if (!guitestFile.empty()) GuiTestStart(f.hwnd(), guitestFile);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(f.hwnd(), &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    if (!guitestFile.empty()) GuiTestWait();
    if (selftest && Form::s_layoutLog) {
        wchar_t lo[MAX_PATH];
        if (GetEnvironmentVariableW(L"VM2B_LAYOUT_OUT", lo, MAX_PATH)) {
            Str all = Str::fmt("# dpi %u\r\n", f.dpi()); Vec<Str>& v = *Form::s_layoutLog;
            for (size_t i = 0; i < v.size(); ++i) { bool dup = false; for (size_t k = 0; k < i; ++k) if (v[k] == v[i]) dup = true; if (!dup) all += v[i] + "\r\n"; }
            HANDLE h = CreateFileW(lo, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, all.data(), (DWORD)all.size(), &w, nullptr); CloseHandle(h); }
        }
    }
    if (!guitestFile.empty()) return GuiTestFailures() ? 1 : 0;
    return selftest ? f.exitCode() : 0;
}
