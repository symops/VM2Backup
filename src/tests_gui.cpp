// The scripted interaction test of the control panel (src/guitest.cpp): an in-process service, then VM2Backup.exe /guitest plays the user
// (state display, open web interface, open logs folder, all 17 languages, Esc / Enter, fixed size) and writes PASS / FAIL lines that this
// test checks; a second test stops, starts and restarts the service through the panel's buttons.  Needs a display and VM2B_GUI_TESTS=1 (tools/run-gui-tests.sh).
// The pure part (the address that "Open web interface" opens) is the unit test panel_web_url, which needs no display.
#include "autotest.h"
#include "svccore.h"
#include "winutil.h"
#include "tests_svc.h"
#include "panelutil.h"

namespace vm2b {

namespace {
Str SiblingExePath(const wchar_t* name)
{
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); wchar_t* s = wcsrchr(p, L'\\'); if (s) *(s + 1) = 0; wcscat(p, name); return FromWide(p);
}
}

VM2B_TEST("gui", gui_interaction)
{
    wchar_t env[8]; if (!GetEnvironmentVariableW(L"VM2B_GUI_TESTS", env, 8) || env[0] != L'1') { c_.skip("set VM2B_GUI_TESTS=1 (and a display) to run the window test"); return true; }
    Str exe = SiblingExePath(L"VM2Backup.exe");
    if (!FileExists(exe)) { c_.skip("VM2Backup.exe is not next to the test"); return true; }
    Svc s(c_, "guii"); if (!s.up) return false;
    Str e; MakeDirs(s.tmp.sub("ctl"), e);
    Str result = s.tmp.sub("guitest-result.txt"); DeleteFileW(Wide(result).c_str());
    SetEnvironmentVariableW(L"VM2B_GUITEST_DIR", Wide(s.tmp.path).c_str());
    Str cmd = "\"" + exe + "\" /pipe " + s.pipe + " /guitest " + result;
    Wide wc(cmd); size_t n = wcslen(wc.c_str()) + 1; Vec<wchar_t> w; w.resize(n); memcpy(w.data(), wc.c_str(), n * sizeof(wchar_t));
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    T_CHECK(CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    bool done = WaitForSingleObject(pi.hProcess, 1000 * 1000) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 99);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    T_CHECK(done);
    Bytes b; Str err; T_CHECK(ReadWholeFile(result.c_str(), b, err)); if (b.empty()) return false;
    Str all((const char*)b.data(), b.size()); int pass = 0, fail = 0;
    for (size_t pos = 0; pos < all.size();) {
        int nl = all.find('\n', pos); size_t end = nl < 0 ? all.size() : (size_t)nl; Str line = all.substr(pos, end - pos); pos = end + 1;
        if (line.starts_with("PASS ")) ++pass;
        else if (line.starts_with("FAIL ")) { ++fail; c_.note("%s", line.c_str()); }
        else if (line.starts_with("INFO ") || line.starts_with("SCENARIO ")) c_.note("%s", line.c_str());
    }
    c_.note("window test: %d checks passed, %d failed", pass, fail);
    T_CHECK(all.find("DONE ") >= 0); T_CHECK(fail == 0); T_CHECK(pass > 90); T_CHECK(code == 0);
    return true;
}

VM2B_TEST("gui", gui_service_stops_and_restarts)
{
    // the Stop / Start / Restart buttons of the panel against the test service: the panel follows the state, no hang, no crash
    wchar_t env[8]; if (!GetEnvironmentVariableW(L"VM2B_GUI_TESTS", env, 8) || env[0] != L'1') { c_.skip("set VM2B_GUI_TESTS=1 (and a display) to run the window test"); return true; }
    Str exe = SiblingExePath(L"VM2Backup.exe");
    if (!FileExists(exe)) { c_.skip("VM2Backup.exe is not next to the test"); return true; }
    Svc s(c_, "guisd"); if (!s.up) return false;
    Str ctl = s.tmp.sub("ctl"); Str e; MakeDirs(ctl, e);
    Str result = s.tmp.sub("guitest-result.txt");
    SetEnvironmentVariableW(L"VM2B_GUITEST_DIR", Wide(s.tmp.path).c_str()); SetEnvironmentVariableW(L"VM2B_GUITEST_ONLY", L"service_control");
    Str cmd = "\"" + exe + "\" /pipe " + s.pipe + " /guitest " + result;
    Wide wc(cmd); size_t n = wcslen(wc.c_str()) + 1; Vec<wchar_t> w; w.resize(n); memcpy(w.data(), wc.c_str(), n * sizeof(wchar_t));
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    bool started = CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != 0;
    SetEnvironmentVariableW(L"VM2B_GUITEST_ONLY", nullptr);
    T_CHECK(started); if (!started) return false;
    DWORD t0 = GetTickCount(); bool done = false; int ns = 1, nt = 1;
    while (GetTickCount() - t0 < 300 * 1000) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) { done = true; break; }
        Str sr = Str::fmt("stop-%d", ns), tr = Str::fmt("start-%d", nt);
        if (FileExists(JoinPath(ctl, sr + ".req")) && !FileExists(JoinPath(ctl, sr + ".ack"))) { s.cli.close(); s.core.stop(); Bytes z; WriteBytes(JoinPath(ctl, sr + ".ack"), z); ++ns; }
        if (FileExists(JoinPath(ctl, tr + ".req")) && !FileExists(JoinPath(ctl, tr + ".ack"))) { bool ok = s.restart(); T_CHECK(ok); Bytes z; WriteBytes(JoinPath(ctl, tr + ".ack"), z); ++nt; }
    }
    if (!done) TerminateProcess(pi.hProcess, 99);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    T_CHECK(done);
    Bytes b; Str err; T_CHECK(ReadWholeFile(result.c_str(), b, err)); if (b.empty()) return false;
    Str all((const char*)b.data(), b.size()); int pass = 0, fail = 0;
    for (size_t pos = 0; pos < all.size();) {
        int nl = all.find('\n', pos); size_t end = nl < 0 ? all.size() : (size_t)nl; Str line = all.substr(pos, end - pos); pos = end + 1;
        if (line.starts_with("PASS ")) ++pass; else if (line.starts_with("FAIL ")) { ++fail; c_.note("%s", line.c_str()); } else if (line.starts_with("INFO ")) c_.note("%s", line.c_str());
    }
    c_.note("panel service control test: %d checks passed, %d failed", pass, fail);
    T_CHECK(all.find("DONE ") >= 0); T_CHECK(fail == 0); T_CHECK(pass >= 25); T_CHECK(code == 0);
    return true;
}

VM2B_TEST("gui", panel_without_service)
{
    // no service at all (not running / not installed): the panel opens, shows it, goes through its self test and exits with 0
    wchar_t env[8]; if (!GetEnvironmentVariableW(L"VM2B_GUI_TESTS", env, 8) || env[0] != L'1') { c_.skip("set VM2B_GUI_TESTS=1 (and a display) to run the window test"); return true; }
    Str exe = SiblingExePath(L"VM2Backup.exe");
    if (!FileExists(exe)) { c_.skip("VM2Backup.exe is not next to the test"); return true; }
    Str cmd = "\"" + exe + "\" /pipe vm2b-no-such-pipe-" + Str::fmt("%lu", (unsigned long)GetCurrentProcessId()) + " /selftest 300";
    Wide wc(cmd); size_t n = wcslen(wc.c_str()) + 1; Vec<wchar_t> w; w.resize(n); memcpy(w.data(), wc.c_str(), n * sizeof(wchar_t));
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    T_CHECK(CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    bool done = WaitForSingleObject(pi.hProcess, 120 * 1000) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 99);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    T_CHECK(done); T_CHECK(code == 0);
    return true;
}


// "Open web interface": which URL is built from the answer of web.status
namespace {
Json WebAddr(const char* a, bool l) { Json o = Json::object(); o["address"] = a; o["listening"] = l; return o; }
Json WebEntry(const char* e, const Json& addrs) { Json o = Json::object(), arr = Json::array(); o["entry"] = e; for (size_t i = 0; i < addrs.size(); ++i) arr.push(addrs.at(i)); o["addresses"] = arr; return o; }
Json WebSt(bool enabled, bool listening, int port, const char* first, const Json& entries) { Json o = Json::object(); o["enabled"] = enabled; o["listening"] = listening; o["port"] = port; o["address"] = first; o["entries"] = entries; return o; }
}

VM2B_TEST("panel", panel_web_url)
{
    Str url; Json none = Json::array();
    T_CHECK(PanelWebUrl(WebSt(false, false, 8443, "", none), url) == PANEL_WEB_OFF && url.empty());                                // the site is off
    T_CHECK(PanelWebUrl(WebSt(true, false, 8443, "", none), url) == PANEL_WEB_NOT_LISTENING && url.empty());                       // enabled, no listener
    T_CHECK(PanelWebUrl(Json::object(), url) == PANEL_WEB_OFF);                                                                    // an empty answer
    Json a = Json::array(); { Json x = Json::array(); x.push(WebAddr("127.0.0.1", true)); a.push(WebEntry("127.0.0.1", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "127.0.0.1", a), url) == PANEL_WEB_OK && url == "https://127.0.0.1:8443/");        // the default
    Json b = Json::array(); { Json x = Json::array(); x.push(WebAddr("::1", true)); b.push(WebEntry("::1", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 24570, "::1", b), url) == PANEL_WEB_OK && url == "https://[::1]:24570/");                // IPv6 in brackets
    Json c = Json::array(); { Json x = Json::array(); x.push(WebAddr("192.168.1.5", true)); x.push(WebAddr("::1", true)); x.push(WebAddr("127.0.0.1", true)); c.push(WebEntry("all", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 9000, "192.168.1.5", c), url) == PANEL_WEB_OK && url == "https://127.0.0.1:9000/");     // loopback preferred over the first address
    Json d = Json::array(); { Json x = Json::array(); x.push(WebAddr("fe80::1%12", true)); x.push(WebAddr("2001:db8::7", true)); d.push(WebEntry("adapter:eth0", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "fe80::1%12", d), url) == PANEL_WEB_OK && url == "https://[2001:db8::7]:8443/");   // a global IPv6 address before a link-local one
    Json e = Json::array(); { Json x = Json::array(); x.push(WebAddr("fe80::1%12", true)); e.push(WebEntry("adapter:eth0", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "fe80::1%12", e), url) == PANEL_WEB_OK && url == "https://[fe80::1%2512]:8443/");  // the zone id is escaped
    Json f = Json::array(); { Json x = Json::array(); x.push(WebAddr("0.0.0.0", true)); x.push(WebAddr("::", true)); f.push(WebEntry("all", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "0.0.0.0", f), url) == PANEL_WEB_OK && url == "https://127.0.0.1:8443/");           // "all interfaces" is reached through loopback
    Json g = Json::array(); { Json x = Json::array(); x.push(WebAddr("10.0.0.2", false)); x.push(WebAddr("10.0.0.3", true)); g.push(WebEntry("adapter:x", x)); }
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "10.0.0.3", g), url) == PANEL_WEB_OK && url == "https://10.0.0.3:8443/");          // only listening addresses count
    T_CHECK(PanelWebUrl(WebSt(true, true, 8443, "10.0.0.9", none), url) == PANEL_WEB_OK && url == "https://10.0.0.9:8443/");       // no entries: the 'address' field
    T_CHECK(PanelWebUrl(WebSt(true, true, 0, "127.0.0.1", none), url) == PANEL_WEB_OK && url == "https://127.0.0.1:8443/");        // a nonsense port falls back to 8443
    T_CHECK(PanelUrlHost("example.com") == "example.com" && PanelUrlHost("::1") == "[::1]" && PanelUrlHost("") == "");
    return true;
}

} // namespace vm2b
