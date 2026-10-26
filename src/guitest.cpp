// VM2Backup.exe /guitest <result file> — scripted interaction test of the control panel. A script thread plays the user: it finds the
// dialogs and controls by their (English) texts, types into edits, picks combo / list items, presses buttons and keys, answers message
// boxes, and then reads the state back from the service over a second pipe connection. Everything goes through window messages, so
// the same code paths run as for a real user.  Started by the autotest (tests_service.cpp, test "gui_interaction"), which provides
// the service, a folder with a VM (VM2B_GUITEST_DIR\vms\vm1) and checks the result file (lines "PASS ..." / "FAIL ...").
#include "panel.h"
#include "ipc.h"
#include "version.h"
#include "winutil.h"
#include "applog.h"
#include <stdio.h>
#include <wchar.h>

using namespace ui;

namespace {

HWND g_main = nullptr; DWORD g_pid = 0; FILE* g_out = nullptr; int g_fails = 0, g_passes = 0; Str g_dir;
vm2b::IpcClient g_cli; HANDLE g_done = nullptr;

void Log(const char* kind, const Str& what) { if (g_out) { fprintf(g_out, "%s %s\n", kind, what.c_str()); fflush(g_out); } }
void Check(bool ok, const Str& what) { if (ok) { ++g_passes; Log("PASS", what); } else { ++g_fails; Log("FAIL", what); } }

// ---- window finding ------------------------------------------------------------------------------------------------
Str WText(HWND h) { return GetText(h); }
Str WClass(HWND h) { wchar_t c[64]; GetClassNameW(h, c, 64); return U8(c); }

struct Found { HWND h; };
BOOL CALLBACK EnumTop(HWND h, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && IsWindowVisible(h) && h != g_main) ((Vec<HWND>*)lp)->push_back(h);
    return TRUE;
}
Vec<HWND> Tops() { Vec<HWND> v; EnumWindows(EnumTop, (LPARAM)&v); return v; }

template <class F> bool WaitFor(F f, int ms = 8000) { DWORD t0 = GetTickCount(); for (;;) { if (f()) return true; if ((int)(GetTickCount() - t0) > ms) return false; Sleep(25); } }
bool WaitGone(HWND h, int ms = 8000) { return WaitFor([&] { return !IsWindow(h) || !IsWindowVisible(h); }, ms); }

// ---- actions -------------------------------------------------------------------------------------------------------
void Pause(int ms = 120) { Sleep((DWORD)ms); }
bool Click(HWND h) { if (!h) return false; if (!WaitFor([&] { return IsWindowEnabled(h) && IsWindowVisible(h); }, 5000)) return false; PostMessageW(h, BM_CLICK, 0, 0); Pause(); return true; }
void Key(HWND h, WPARAM vk) { PostMessageW(h, WM_KEYDOWN, vk, 0); PostMessageW(h, WM_KEYUP, vk, 0); Pause(200); }

// message boxes: wait for one, return its text, press a button (IDOK / IDYES / IDNO / IDCANCEL)
HWND FindBox() { Vec<HWND> v = Tops(); for (size_t i = 0; i < v.size(); ++i) if (WClass(v[i]) == "#32770") return v[i]; return nullptr; }
Str BoxText(HWND box) { Str t; for (HWND c = GetWindow(box, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) if (WClass(c) == "Static") { Str x = WText(c); if (!x.empty()) t += (t.empty() ? "" : "\n") + x; } return t; }
bool Box(Str& text, int button = IDOK, int ms = 8000)
{
    HWND b = nullptr; if (!WaitFor([&] { b = FindBox(); return b != nullptr; }, ms)) { text = Str(); return false; }
    text = BoxText(b); HWND ok = GetDlgItem(b, button); if (!ok) ok = GetDlgItem(b, IDOK); if (!ok) ok = GetDlgItem(b, IDYES);
    if (ok) PostMessageW(b, WM_COMMAND, MAKEWPARAM(button, BN_CLICKED), (LPARAM)ok); else PostMessageW(b, WM_CLOSE, 0, 0);
    WaitGone(b, 3000); Pause(80); return true;
}

// ---- service access ------------------------------------------------------------------------------------------------
bool Rpc(const char* cmd, const Json& a, Json& r) { Str e; if (!g_cli.connected()) { if (!g_cli.connect(g_pipeName, 5000, e)) return false; } return g_cli.call(cmd, a, r, e, 20000); }
Json SettingsNow() { Json r; Rpc("settings.get", Json::object(), r); return r; }

long long SettingsLangV() { return SettingsNow().get("language").integer(-1); }

// ---- the control panel ---------------------------------------------------------------------------------------------------
bool MainAlive() { DWORD_PTR r; return SendMessageTimeoutW(g_main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 3000, &r) != 0; }
Str Lbl(int id) { return GetText(GetDlgItem(g_main, id)); }
HWND Ctl(int id) { return GetDlgItem(g_main, id); }
bool Enabled(int id) { HWND h = Ctl(id); return h && IsWindowEnabled(h); }
bool WaitLbl(int id, const Str& want, int ms = 8000) { return WaitFor([&] { return Lbl(id) == want; }, ms); }
bool WaitLblHas(int id, const char* part, int ms = 8000) { return WaitFor([&] { return Lbl(id).find(part) >= 0; }, ms); }

// the hooks: what "open" was asked to open, and the stop / start requests to the autotest (it owns the service)
volatile LONG g_openCount = 0; wchar_t g_openLast[1024] = L"";
void OpenHook(const wchar_t* target) { wcsncpy(g_openLast, target, 1023); g_openLast[1023] = 0; InterlockedIncrement(&g_openCount); }
Str LastOpen() { return U8(g_openLast); }
void Touch(const Str& path) { vm2b::Wide w(path); HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
bool Exists(const Str& path) { vm2b::Wide w(path); return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool SvcHook(int op)
{
    static int ns = 0, nt = 0; const char* nm = op == PANEL_SVC_START ? "start" : "stop"; int n = op == PANEL_SVC_START ? ++nt : ++ns;
    Str base = g_dir + "\\ctl\\" + Str::fmt("%s-%d", nm, n);
    Touch(base + ".req");
    return WaitFor([&] { return Exists(base + ".ack"); }, 30000);
}

bool SiteOn(int port, const char* bind)
{
    Json u = Json::object(); u["name"] = "guiweb"; u["role"] = "operator"; u["password"] = "Long-Gui-Passw0rd"; u["twofa"] = "off"; Json r; Rpc("web.users.save", u, r);
    Json a = Json::object(), b = Json::array(); b.push(bind); a["enabled"] = true; a["port"] = port; a["bind"] = b; a["require_2fa_admins"] = false;
    Json st; return Rpc("web.settings.set", a, r);
}
void SiteOff() { Json a = Json::object(), r; a["enabled"] = false; Rpc("web.settings.set", a, r); }

void ScenarioState()
{
    SiteOff();
    Json st; Check(Rpc("state", Json::object(), st), "the test service answers");
    Check(WaitLbl(PANEL_ID_STATE, U8(Tr("Running"))), "the panel shows the service as running: " + Lbl(PANEL_ID_STATE));
    Check(WaitLbl(PANEL_ID_VER, st.get("version").str()), "the version is the service's: " + Lbl(PANEL_ID_VER));
    Check(Lbl(PANEL_ID_VER) == VM2B_VER_STR, "and it is this build's version");
    Check(Lbl(PANEL_ID_DATA) == st.get("data_dir").str(), "the data folder is shown: " + Lbl(PANEL_ID_DATA));
    Check(WaitLbl(PANEL_ID_WEB, U8(Tr("Off"))), "the web site is shown as off: " + Lbl(PANEL_ID_WEB));
    Check(!Enabled(PANEL_ID_START) && Enabled(PANEL_ID_STOP) && Enabled(PANEL_ID_RESTART), "Start is off, Stop and Restart are on while the service runs");
    Check(Enabled(PANEL_ID_OPENWEB) && Enabled(PANEL_ID_LOGS) && Enabled(PANEL_ID_LANG) && Enabled(IDCANCEL), "Open web interface, Open logs folder, language and Exit are available");
    // the labels follow a change made behind the panel's back (once a second)
    Check(SiteOn(24570, "127.0.0.1"), "the site is switched on through the service");
    Check(WaitLblHas(PANEL_ID_WEB, "127.0.0.1:24570"), "the panel shows the listening address within seconds: " + Lbl(PANEL_ID_WEB));
    SiteOff();
    Check(WaitLbl(PANEL_ID_WEB, U8(Tr("Off"))), "and 'Off' again when the site is switched off");
}

void ScenarioOpenWeb()
{
    SiteOff(); WaitLbl(PANEL_ID_WEB, U8(Tr("Off")));
    Str t; LONG n0 = g_openCount;
    Click(Ctl(PANEL_ID_OPENWEB)); Check(Box(t) && t == U8(Tr("The web site is off. Switch it on in the web interface, or reset it with VM2BackupService.exe /reset-web.")), "site off: a clear message: " + t);
    Check(g_openCount == n0, "nothing was opened while the site is off");
    SiteOn(24570, "127.0.0.1"); Check(WaitLblHas(PANEL_ID_WEB, "24570"), "site on");
    Click(Ctl(PANEL_ID_OPENWEB)); Check(WaitFor([&] { return g_openCount > n0; }, 4000), "the button opens something");
    Check(LastOpen() == "https://127.0.0.1:24570/", "the URL is https://127.0.0.1:24570/ : " + LastOpen());
    // IPv6 loopback (the machine may not have it)
    SiteOn(24571, "::1"); Pause(1500);
    Json st; Rpc("web.status", Json::object(), st);
    if (st.get("listening").boolean()) {
        n0 = g_openCount; WaitLblHas(PANEL_ID_WEB, "[::1]:24571"); Check(Lbl(PANEL_ID_WEB).find("[::1]:24571") >= 0, "an IPv6 address is shown in brackets: " + Lbl(PANEL_ID_WEB));
        Click(Ctl(PANEL_ID_OPENWEB)); WaitFor([&] { return g_openCount > n0; }, 4000); Check(LastOpen() == "https://[::1]:24571/", "IPv6 URL: " + LastOpen());
    } else Log("INFO", "no IPv6 loopback listener here: the IPv6 case was skipped");
    SiteOff(); WaitLbl(PANEL_ID_WEB, U8(Tr("Off")));
    // the Enter key presses the default button, which is Open web interface
    Key(g_main, VK_RETURN); Check(Box(t) && t.find("web site is off") >= 0, "Enter = Open web interface (the default button): " + t);
    // the logs folder
    n0 = g_openCount; Click(Ctl(PANEL_ID_LOGS)); Check(WaitFor([&] { return g_openCount > n0; }, 4000), "Open logs folder opens something");
    Str lg = LastOpen(); Check(lg.lower().find("vm2backup") >= 0 && lg.lower().find("logs") >= 0, "it is the log folder: " + lg);
    Check(Str(U8(AppLogDir())) == lg, "the folder of the program log");
}

void ScenarioLanguages()
{
    int n = LangCount(); Json st; Str t;
    for (int i = 1; i <= n; ++i) {
        HWND cb = Ctl(PANEL_ID_LANG); if (!cb) { Check(false, "language combo found"); return; }
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)i, 0); SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(PANEL_ID_LANG, CBN_SELCHANGE), (LPARAM)cb); Pause(250);
        Check(SettingsLangV() == i, Str::fmt("language %d saved in the service", i));
        Check(Lbl(IDCANCEL) == U8(Tr("Exit")) && Lbl(PANEL_ID_START) == U8(Tr("Start service")) && Lbl(PANEL_ID_OPENWEB) == U8(Tr("Open web interface")), Str::fmt("language %d: the buttons are re-translated at once", i));
        Check(WaitLbl(PANEL_ID_STATE, U8(Tr("Running")), 3000), Str::fmt("language %d: the state is shown in it", i));
        Check(SendMessageW(Ctl(PANEL_ID_LANG), CB_GETCURSEL, 0, 0) == i, Str::fmt("language %d stays selected", i));
        Check(MainAlive(), Str::fmt("language %d: the window responds", i));
        if (i == 2) Check(Lbl(IDCANCEL) != "Exit", "Russian differs from English");
    }
    HWND cb = Ctl(PANEL_ID_LANG); SendMessageW(cb, CB_SETCURSEL, 1, 0); SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(PANEL_ID_LANG, CBN_SELCHANGE), (LPARAM)cb); Pause(300);
    Check(Lbl(IDCANCEL) == "Exit", "back to English");
}

void ScenarioWindow()
{
    LONG_PTR st = GetWindowLongPtrW(g_main, GWL_STYLE); RECT r; GetWindowRect(g_main, &r);
    Check(!(st & WS_THICKFRAME) && !(st & WS_MAXIMIZEBOX), "the panel has a fixed size (no sizing border, no maximize)");
    Check(r.right - r.left > 300 && r.right - r.left < 1400 && r.bottom - r.top > 200 && r.bottom - r.top < 900, Str::fmt("a small window (%dx%d px)", (int)(r.right - r.left), (int)(r.bottom - r.top)));
    ShowWindow(g_main, SW_MINIMIZE); Pause(300); ShowWindow(g_main, SW_RESTORE); Pause(300);
    Check(MainAlive(), "alive after minimize / restore");
    Key(g_main, VK_TAB); Key(g_main, VK_TAB); Check(MainAlive(), "Tab moves the focus without trouble");
}

// the autotest owns the service: stop.req / start.req files ask it to stop and start the real service object
void ScenarioServiceControl()
{
    Str t, ctl = g_dir + "\\ctl";
    Check(WaitLbl(PANEL_ID_STATE, U8(Tr("Running"))), "service running at the start");
    Click(Ctl(PANEL_ID_STOP)); Box(t, IDNO); Pause(1500);
    Check(!Exists(ctl + "\\stop-1.req") && Lbl(PANEL_ID_STATE) == U8(Tr("Running")), "Stop answered No: nothing happens");
    Click(Ctl(PANEL_ID_STOP)); Check(Box(t, IDYES) && t == U8(Tr("Stop the service? Running backups are paused and continue after the next start.")), "Stop asks first: " + t);
    Check(WaitFor([&] { return Exists(ctl + "\\stop-1.req"); }, 8000), "the stop request was made");
    Check(WaitFor([&] { return Exists(ctl + "\\stop-1.ack"); }, 30000), "the service was stopped");
    g_cli.close();
    Check(WaitLbl(PANEL_ID_STATE, U8(Tr("Stopped")), 10000), "the panel shows Stopped: " + Lbl(PANEL_ID_STATE));
    Check(Lbl(PANEL_ID_VER) == "-" && Lbl(PANEL_ID_DATA) == "-" && Lbl(PANEL_ID_WEB) == U8(Tr("Off")), "version, data folder and web site are cleared");
    Check(WaitFor([&] { return Enabled(PANEL_ID_START) && !Enabled(PANEL_ID_STOP) && Enabled(PANEL_ID_RESTART); }, 10000), "Start and Restart are on, Stop is off");
    Check(MainAlive(), "the panel responds while the service is down");
    // everything still works without the service
    Click(Ctl(PANEL_ID_OPENWEB)); Check(Box(t) && t == U8(Tr("The VM2Backup service is not running, so there is no web site. Start the service first.")), "Open web interface explains: " + t);
    { HWND cb = Ctl(PANEL_ID_LANG); SendMessageW(cb, CB_SETCURSEL, 2, 0); SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(PANEL_ID_LANG, CBN_SELCHANGE), (LPARAM)cb); Pause(400);
      Check(Lbl(IDCANCEL) == U8(Tr("Exit")) && Lbl(IDCANCEL) != "Exit" && Lbl(PANEL_ID_STATE) == U8(Tr("Stopped")), "the language switches without the service");
      cb = Ctl(PANEL_ID_LANG); SendMessageW(cb, CB_SETCURSEL, 1, 0); SendMessageW(g_main, WM_COMMAND, MAKEWPARAM(PANEL_ID_LANG, CBN_SELCHANGE), (LPARAM)cb); Pause(400); }
    { LONG n0 = g_openCount; Click(Ctl(PANEL_ID_LOGS)); Check(WaitFor([&] { return g_openCount > n0; }, 4000), "the logs folder opens without the service"); }
    Pause(1500); Check(MainAlive(), "alive after that");
    Click(Ctl(PANEL_ID_START));
    Check(WaitFor([&] { return Exists(ctl + "\\start-1.ack"); }, 30000), "the start request was served");
    Check(WaitLbl(PANEL_ID_STATE, U8(Tr("Running")), 15000), "the panel shows Running again: " + Lbl(PANEL_ID_STATE));
    Check(WaitLbl(PANEL_ID_VER, VM2B_VER_STR, 4000), "the version is back");
    Check(WaitFor([&] { return !Enabled(PANEL_ID_START) && Enabled(PANEL_ID_STOP); }, 10000), "buttons follow the state");
    Json st; Check(Rpc("state", Json::object(), st), "the program can talk to the restarted service");
    // restart
    Click(Ctl(PANEL_ID_RESTART)); Check(Box(t, IDYES) && t.find("Restart") == 0, "Restart asks first: " + t);
    Check(WaitFor([&] { return Exists(ctl + "\\stop-2.ack") && Exists(ctl + "\\start-2.ack"); }, 60000), "Restart = a stop, then a start");
    g_cli.close();
    Check(WaitFor([&] { return Lbl(PANEL_ID_STATE) == U8(Tr("Running")) && Enabled(PANEL_ID_STOP); }, 15000), "running after the restart");     // the buttons follow a moment after the label (the panel is busy until its restart thread ends)
    Check(Rpc("state", Json::object(), st), "the service answers after the restart");
    Check(MainAlive(), "the panel is fine after the restart");
}

// Esc presses Exit (IDCANCEL) and closes the panel: the last thing the test does
void ScenarioEscCloses()
{
    Key(g_main, VK_ESCAPE); Check(WaitFor([&] { return !IsWindow(g_main); }, 5000), "Esc closes the panel");
}

DWORD WINAPI ScriptThread(void*)
{
    Sleep(1500);                                               // the window is up, the first poll done
    typedef void (*Fn)();
    struct Sc { const char* name; Fn fn; } list[] = {
        { "state", ScenarioState }, { "open web", ScenarioOpenWeb }, { "languages", ScenarioLanguages }, { "window", ScenarioWindow }, { "service_control", ScenarioServiceControl },
    };
    wchar_t only[256] = L""; GetEnvironmentVariableW(L"VM2B_GUITEST_ONLY", only, 256); Str onlyS = U8(only);
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); ++i) {
        if (onlyS.empty() ? Str(list[i].name) == "service_control" : onlyS.find(list[i].name) < 0) continue;      // service_control needs the autotest to stop / start the service
        Log("SCENARIO", list[i].name);
        list[i].fn();
        Vec<HWND> left = Tops(); for (size_t k = 0; k < left.size(); ++k) { Log("INFO", "left open: " + WText(left[k])); PostMessageW(left[k], WM_CLOSE, 0, 0); } Pause(300);
        Check(left.size() == 0, Str("no dialog left open after: ") + list[i].name);
        Check(MainAlive(), Str("window alive after: ") + list[i].name);
    }
    if (onlyS.empty()) { Log("SCENARIO", "esc"); ScenarioEscCloses(); }
    Log("DONE", Str::fmt("%d passed, %d failed", g_passes, g_fails));
    if (g_out) { fclose(g_out); g_out = nullptr; }
    if (IsWindow(g_main)) PostMessageW(g_main, WM_CLOSE, 0, 0);
    SetEvent(g_done);
    return 0;
}

DWORD WINAPI Watchdog(void*)
{
    Sleep(600000);                                             // 10 minutes
    if (g_out) { fprintf(g_out, "FAIL watchdog: the test did not finish in 10 minutes (GUI hang?)\n"); fclose(g_out); }
    ExitProcess(3);
}

} // namespace

void GuiTestStart(HWND mainWnd, const Str& resultFile)
{
    g_main = mainWnd; g_pid = GetCurrentProcessId(); g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    vm2b::Wide w(resultFile); g_out = _wfopen(w.c_str(), L"wb");
    wchar_t d[MAX_PATH]; if (GetEnvironmentVariableW(L"VM2B_GUITEST_DIR", d, MAX_PATH)) g_dir = U8(d);
    g_panelHooks.service = SvcHook; g_panelHooks.open = OpenHook;
    CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, ScriptThread, nullptr, 0, nullptr);
}
void GuiTestWait() { if (g_done) WaitForSingleObject(g_done, 30000); }
int GuiTestFailures() { return g_fails; }
