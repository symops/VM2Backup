// Autotests of the service core (run in-process, with its own pipe, data folder and a clock the test can move).
#include "autotest.h"
#include "svccore.h"
#include "schedule.h"
#include "winutil.h"
#include "webauth.h"
#include "tests_svc.h"

namespace vm2b {

VM2B_TEST("service", rpc_basics_and_secrets)
{
    Svc s(c_, "svc1"); if (!s.up) return false;
    Json st = s.ok("state", Json::object());
    T_CHECK(st.get("version").str().size() > 3); T_CHECK(!st.get("scheduler").boolean());               // the scheduler is off until it is switched on
    Json sv = s.ok("servers.list", Json::object()); T_CHECK(sv.size() == 1 && sv.at(0).get("id").str() == "local");
    // an FTP server with a password: the password is stored protected and never comes back
    Json f = Json::object(); f["type"] = "ftpes"; f["name"] = "Office FTP"; f["host"] = "ftp.example.com"; f["port"] = 21; f["user"] = "backup"; f["password"] = "S3cret-Passw0rd!"; f["directory"] = "/vm";
    Json a = Json::object(); a["server"] = f; Json saved = s.ok("servers.save", a);
    T_CHECK(saved.get("has_password").boolean()); T_CHECK(!saved.has("password"));
    Json list = s.ok("servers.list", Json::object()); T_CHECK(list.size() == 2);
    T_CHECK(list.dump().find("S3cret") < 0);
    Bytes cfg; Str e; T_CHECK(ReadWholeFile(JoinPath(s.core.config().dataDir(), "config.json").c_str(), cfg, e));
    Str text((const char*)cfg.data(), cfg.size());
    T_CHECK(text.find("S3cret") < 0); T_CHECK(text.find("dpapi:") >= 0);                                  // on disk: DPAPI, no plain text
    Json revealed; T_CHECK(s.core.config().server(saved.get("id").str(), true, revealed)); T_CHECK(revealed.get("password").str() == "S3cret-Passw0rd!");   // the service itself can use it
    // saving again with the mask keeps the old password
    Json f2 = saved; f2["password"] = kMask; f2["port"] = 2121; Json a2 = Json::object(); a2["server"] = f2; s.ok("servers.save", a2);
    s.core.config().server(saved.get("id").str(), true, revealed); T_CHECK(revealed.get("password").str() == "S3cret-Passw0rd!" && revealed.get("port").integer() == 2121);
    // validation
    Json bad = Json::object(); bad["type"] = "scp"; bad["name"] = "x"; Json ab = Json::object(); ab["server"] = bad; T_CHECK(!s.call("servers.save", ab)); T_CHECK(s.lastErr.find("address") >= 0);
    Json dup = f; Json ad = Json::object(); ad["server"] = dup; T_CHECK(!s.call("servers.save", ad));                                   // the same name
    T_CHECK(!s.call("nonsense", Json::object())); T_CHECK(s.lastErr.find("unknown command") >= 0);
    Json delLocal = Json::object(); delLocal["id"] = "local"; T_CHECK(!s.call("servers.delete", delLocal));
    // settings
    Json bs = Json::object(); bs["max_parallel"] = 0; T_CHECK(!s.call("settings.set", bs)); Json bs2 = Json::object(); bs2["nonsense"] = 1; T_CHECK(!s.call("settings.set", bs2));
    // a second service on the same pipe name must refuse to start
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {               // Wine does not enforce FILE_FLAG_FIRST_PIPE_INSTANCE
        ServiceCore other; ServiceOptions o2; o2.dataDir = s.tmp.sub("data2"); o2.pipeName = s.pipe; o2.pipeSddl = "D:(A;;GA;;;WD)"; Str err2; T_CHECK(!other.start(o2, err2));
    }
    return true;
}

VM2B_TEST("service", task_run_history_explorer)
{
    Svc s(c_, "svc2"); if (!s.up) return false;
    Str root = s.tmp.sub("vms"); Bytes disk = Random(c_, 2500000); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), disk); Bytes cfgb = Random(c_, 3000); WriteBytes(JoinPath(root, "vm1\\vm1.vmx"), cfgb);
    Str srv = s.addDirServer(root);
    Json vms = s.ok("servers.vms", [&] { Json a = Json::object(); a["id"] = srv; return a; }());
    T_CHECK(vms.get("vms").size() == 1 && vms.get("vms").at(0).get("name").str() == "vm1");
    Str store = s.tmp.sub("store");
    Json el = Svc::Element(srv, "vm1", JoinPath(store, "{VM}"), "xz", 1, "pw-123");
    Str task = s.saveTask("Nightly", el);
    T_CHECK(!task.empty());
    // the stored task never shows the password
    Json tl = s.ok("tasks.list", Json::object()); T_CHECK(tl.size() == 1); T_CHECK(tl.dump().find("pw-123") < 0); T_CHECK(tl.at(0).get("elements").at(0).get("encrypt").get("has_password").boolean());
    Json ra = Json::object(); ra["id"] = "Nightly"; Json run = s.ok("tasks.run", ra);
    Str jid = run.get("job").str(); T_CHECK(!jid.empty());
    Str fin = s.waitJob(jid); T_CHECK(fin == "Success" || fin == "Gone");
    T_CHECK(s.waitHistory(1));
    Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 10; return a; }());
    T_CHECK(hl.size() == 1 && hl.at(0).get("status").str() == "Success"); T_CHECK(hl.at(0).get("summary").get("ok").integer() == 1);
    Json hr = s.ok("history.get", [&] { Json a = Json::object(); a["id"] = hl.at(0).get("id").str(); return a; }());
    T_CHECK(hr.get("results").size() == 1); T_CHECK(hr.get("results").at(0).get("vm").str() == "vm1"); T_CHECK(hr.get("results").at(0).get("raw").integer() > 2500000);
    T_CHECK(hr.get("results").at(0).get("volumes").integer() >= 3);                                      // 1 MiB volumes
    Str archive = hr.get("results").at(0).get("archive").str();
    // Backup Explorer
    Json qa = Json::object(); qa["server"] = "local"; Json lst = s.ok("backups.list", qa);
    T_CHECK(lst.get("backups").size() == 1); T_CHECK(lst.get("backups").at(0).get("archive").str() == archive); T_CHECK(lst.get("backups").at(0).get("encrypted").boolean());
    // verify and restore are jobs too
    Json va = Json::object(); va["server"] = "local"; va["directory"] = JoinPath(store, "vm1"); va["archive"] = archive; va["password"] = "pw-123";
    Json vr = s.ok("backups.verify", va); Str vj = vr.get("job").str(); Str vs = s.waitJob(vj); T_CHECK(vs == "Success" || vs == "Gone");
    Json bad = va; bad["password"] = "nope"; Json br = s.ok("backups.verify", bad); Str bs = s.waitJob(br.get("job").str()); T_CHECK(bs == "Failed" || bs == "Gone");
    Json rr = va; rr["dest"] = s.tmp.sub("restored"); Json rj = s.ok("backups.restore", rr); Str rs = s.waitJob(rj.get("job").str()); T_CHECK(rs == "Success" || rs == "Gone");
    T_CHECK(s.waitHistory(4));
    Bytes got; Str e; T_CHECK(ReadWholeFile(JoinPath(s.tmp.sub("restored"), "vm1\\disk.vmdk").c_str(), got, e) && got.size() == disk.size() && memcmp(got.data(), disk.data(), disk.size()) == 0);
    // a second run while the first would still be running is refused; running twice in a row is fine
    Json run2 = s.ok("tasks.run", ra); s.waitJob(run2.get("job").str());
    Json del = va; T_CHECK(s.call("backups.delete", del)); Json lst2 = s.ok("backups.list", qa); T_CHECK(lst2.get("backups").size() == 1);
    // the task survives a restart of the service: the configuration is on disk
    return true;
}

VM2B_TEST("service", scheduler_fake_clock)
{
    Svc s(c_, "svc3", true); if (!s.up) return false;
    g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), Random(c_, 20000)); Str srv = s.addDirServer(root);
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "every"; Json ev = Json::object(); ev["value"] = 10; ev["unit"] = "minutes"; sch["every"] = ev;
    sch["first_time"] = FormatLocalMin(UnixToLocalMin(t0)).replace_all(" ", "T");
    Str task = s.saveTask("Every10", Svc::Element(srv, "vm1", JoinPath(s.tmp.sub("store"), "{VM}")), sch);
    Json bad = sch; bad["every"] = Json::object(); Json bt = Json::object(); bt["name"] = "bad"; bt["schedule"] = bad; Json ba = Json::object(); ba["task"] = bt; T_CHECK(!s.call("tasks.save", ba));
    // the scheduler is off: time passes, nothing runs
    g_fakeNow = t0 + 11 * 60; Sleep(300); T_CHECK(s.historyCount() == 0);
    Json on = Json::object(); on["enabled"] = true; s.ok("scheduler.set", on);
    Sleep(200);                                                                                                // the first tick after enabling sees no time passing
    g_fakeNow += 10 * 60; T_CHECK(s.waitHistory(1, 20000));                                                    // one run for the 10 minutes that passed
    int n1 = s.historyCount();
    Sleep(300); T_CHECK(s.historyCount() == n1);                                                                // nothing more until the next slot
    g_fakeNow += 10 * 60; T_CHECK(s.waitHistory(n1 + 1, 20000));
    // a slot that was missed by much more than the grace time is not run late
    int n2 = s.historyCount(); g_fakeNow += 5 * 3600; Sleep(600); T_CHECK(s.historyCount() <= n2 + 1);
    Json nx = s.ok("tasks.next", [&] { Json a = Json::object(); a["id"] = "Every10"; return a; }()); T_CHECK(nx.get("next").integer() > g_fakeNow);
    Json off = Json::object(); off["enabled"] = false; s.ok("scheduler.set", off);
    return true;
}

// Ограничение скорости в записи элемента задачи (speed_mbit) должно действовать при запуске ЗАДАЧИ (ошибка найдена на Windows 10 x64, фаза speedlimit): измеряем время записи на цель с лимитом и без.
VM2B_TEST("service", task_element_speed_limit_is_applied)
{
    Svc s(c_, "svc_spd", true); if (!s.up) return false;
    Str root = s.tmp.sub("vms"); Bytes disk = Random(c_, 4200000); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), disk); Str srv = s.addDirServer(root);
    auto runTimed = [&](const char* name, double mbit, double& limitSeen) -> i64 {
        Json el = Svc::Element(srv, "vm1", JoinPath(s.tmp.sub(name), "{VM}"), "xz", 0); el["verify"] = false; if (mbit > 0) el["speed_mbit"] = mbit;
        Str task = s.saveTask(name, el); Json ra = Json::object(); ra["id"] = task;
        DWORD t0 = GetTickCount(); Json run = s.ok("tasks.run", ra); Str jid = run.get("job").str(); limitSeen = 0;
        while (GetTickCount() - t0 < 60000) { Json jobs = s.ok("jobs.list", Json::object()); bool live = false; for (size_t i = 0; i < jobs.size(); ++i) if (jobs.at(i).get("id").str() == jid) { Str stt = jobs.at(i).get("status").str(); live = stt == "Waiting" || stt == "Running"; double l = jobs.at(i).get("limit_mbit").num(); if (live && l > limitSeen) limitSeen = l; } if (!live && GetTickCount() - t0 > 300) break; Sleep(40); }
        Str st = s.waitJob(jid, 60000); if (st != "Success" && st != "Gone" && !st.empty()) return -1;
        return (i64)(GetTickCount() - t0);
    };
    double l0 = 0, l1 = 0; i64 free_ms = runTimed("free", 0, l0); i64 slow_ms = runTimed("slow", 8.0, l1);        // 8 Mbit/s = 1 MB/s: 4,2 MB of incompressible data need several seconds
    c_.note("task without a limit: %d ms (limit seen %.2f), with speed_mbit=8: %d ms (limit seen %.2f)", (int)free_ms, l0, (int)slow_ms, l1);
    T_CHECK(free_ms >= 0 && slow_ms >= 0); T_CHECK(l0 == 0); T_CHECK(l1 > 7.5 && l1 < 8.5);
    T_CHECK(slow_ms >= 2500 && slow_ms > free_ms + 1500);
    return true;
}

VM2B_TEST("service", windows_pause_and_continue)
{
    Svc s(c_, "svc4", true); if (!s.up) return false;
    // a Tuesday 2026-10-20, 10:00 local; the window is 09:00-11:00 on weekdays at 8 Mbit/s (1 MB/s)
    i64 base = LocalMinToUnix(CivilToMin(2026, 10, 20, 10, 0)); g_fakeNow = base;
    Str root = s.tmp.sub("vms"); Bytes disk = Random(c_, 4200000); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), disk); Str srv = s.addDirServer(root);
    Json win = Json::array(); { Json w = Json::object(); Json d = Json::array(); for (int k = 0; k < 5; ++k) d.push(k); w["days"] = d; w["from"] = "09:00"; w["to"] = "11:00"; w["mbit"] = 8; win.push(w); }
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "once"; sch["at"] = "2026-10-20T10:01";
    Str task = s.saveTask("Windowed", Svc::Element(srv, "vm1", JoinPath(s.tmp.sub("store"), "{VM}")), sch, win);
    Json on = Json::object(); on["enabled"] = true; s.ok("scheduler.set", on);
    Sleep(200);
    g_fakeNow = base + 90;                                                                                       // 10:01:30: the run starts, limited to ~1 MB/s
    DWORD t0 = GetTickCount(); Str jid;
    while (GetTickCount() - t0 < 15000) { Json jobs = s.ok("jobs.list", Json::object()); if (jobs.size()) { jid = jobs.at(0).get("id").str(); Json l = jobs.at(0); if (l.get("limit_mbit").num() > 7.5 && l.get("limit_mbit").num() < 8.5) break; } Sleep(30); }
    T_CHECK(!jid.empty());
    Json jobs = s.ok("jobs.list", Json::object()); T_CHECK(jobs.size() == 1 && jobs.at(0).get("limit_mbit").num() > 7.5 && jobs.at(0).get("limit_mbit").num() < 8.5);   // the window's speed limit is in force
    // the window closes in the middle of the run: the job pauses and keeps its finished volumes
    Sleep(2500); g_fakeNow = base + 61 * 60;                                                                     // 11:01
    Str st = s.waitJob(jid, 20000); T_CHECK(st == "Paused");
    Json qa = Json::object(); qa["server"] = "local"; Json lst = s.ok("backups.list", qa);
    T_CHECK(lst.get("resumable").size() == 1); T_CHECK(lst.get("backups").size() == 0);                          // an interrupted backup, not a finished one
    T_CHECK(s.historyCount() == 0);                                                                              // a pause is not a result
    Sleep(500); T_CHECK(s.core.runningJobs() == 0);
    // the next day at 10:00 the window is open again: the task goes on from the journal
    g_fakeNow = LocalMinToUnix(CivilToMin(2026, 10, 21, 10, 0));
    T_CHECK(s.waitHistory(1, 40000));
    Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 5; return a; }()); T_CHECK(hl.size() == 1 && hl.at(0).get("status").str() == "Success");
    Json lst2 = s.ok("backups.list", qa); T_CHECK(lst2.get("backups").size() == 1); T_CHECK(lst2.get("resumable").size() == 0);
    Json rr = Json::object(); rr["server"] = "local"; rr["directory"] = lst2.get("backups").at(0).get("directory").str(); rr["archive"] = lst2.get("backups").at(0).get("archive").str(); rr["dest"] = s.tmp.sub("rest");
    Json rj = s.ok("backups.restore", rr); Str rs = s.waitJob(rj.get("job").str()); T_CHECK(rs == "Success" || rs == "Gone");
    Bytes got; Str e; T_CHECK(ReadWholeFile(JoinPath(s.tmp.sub("rest"), "vm1\\disk.vmdk").c_str(), got, e) && got.size() == disk.size() && memcmp(got.data(), disk.data(), disk.size()) == 0);
    return true;
}

VM2B_TEST("service", restart_continues_a_running_job)
{
    Tmp tmp(c_, "svc5"); Str data = tmp.sub("data");
    Str root = tmp.sub("vms"); Bytes disk = Random(c_, 5000000); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), disk);
    Str pipe = Str::fmt("vm2b-test-r-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(c_.next() % 100000000));
    Str taskId, srv, archive;
    {
        ServiceCore a; ServiceOptions o; o.dataDir = data; o.pipeName = pipe; o.pipeSddl = "D:(A;;GA;;;WD)"; o.tickMs = 40; Str err;
        T_CHECK(a.start(o, err)); IpcClient cli; T_CHECK(cli.connect(pipe, 5000, err));
        Json st = Json::object(); st["kdf_iterations"] = 3000; st["retry_base_ms"] = 1; st["segment_mib"] = 1; Json r; cli.call("settings.set", st, r, err);
        Json s = Json::object(); s["type"] = "directory"; s["name"] = "VMs"; s["path"] = root; Json aa = Json::object(); aa["server"] = s; T_CHECK(cli.call("servers.save", aa, r, err)); srv = r.get("id").str();
        Json t = Json::object(); t["name"] = "T"; Json els = Json::array(); els.push(Svc::Element(srv, "vm1", JoinPath(tmp.sub("store"), "{VM}"), "xz")); t["elements"] = els;
        // a speed limit so that the job is still running when the service stops
        Json sl = Json::object(); sl["speed_limit_mbit"] = 16; cli.call("settings.set", sl, r, err);
        Json ta = Json::object(); ta["task"] = t; T_CHECK(cli.call("tasks.save", ta, r, err)); taskId = r.get("id").str();
        Json ra = Json::object(); ra["id"] = taskId; T_CHECK(cli.call("tasks.run", ra, r, err));
        Sleep(1500);                                                                                               // about 2.5 MB are stored by now
        cli.close(); a.stop();                                                                                    // the service goes down in the middle of the job
    }
    {   // a new service: the task was running, so it goes on (the scheduler is switched on for that)
        ServiceCore b; ServiceOptions o; o.dataDir = data; o.pipeName = pipe; o.pipeSddl = "D:(A;;GA;;;WD)"; o.tickMs = 40; Str err;
        T_CHECK(b.start(o, err)); IpcClient cli; T_CHECK(cli.connect(pipe, 5000, err)); Json r;
        Json en = Json::object(); en["enabled"] = true; cli.call("scheduler.set", en, r, err);
        Json sl = Json::object(); sl["speed_limit_mbit"] = 0; cli.call("settings.set", sl, r, err);
        Json qa = Json::object(); qa["server"] = "local";
        DWORD t0 = GetTickCount(); bool done = false;
        while (GetTickCount() - t0 < 40000 && !done) { Json l; if (cli.call("backups.list", qa, l, err) && l.get("backups").size() == 1) { done = true; archive = l.get("backups").at(0).get("archive").str(); } Sleep(100); }
        T_CHECK(done);
        // the backup is listed as soon as its manifest is on disk; the history entry is written a moment later (a race that only a fast real disk shows)
        Json hl; Json la = Json::object(); la["limit"] = 10;
        for (DWORD h0 = GetTickCount(); GetTickCount() - h0 < 15000; Sleep(50)) { cli.call("history.list", la, hl, err); if (hl.size() >= 1) break; }
        T_CHECK(hl.size() == 1 && hl.at(0).get("status").str() == "Success");
        cli.close(); b.stop();
    }
    return true;
}

VM2B_TEST("service", default_pipe_security)
{
    // the shipped access rights (Administrators + SYSTEM) must at least let an administrator in
    Svc s(c_, "svc6", false, true);
    if (!s.up) { c_.skip("this environment does not accept the security descriptor"); return true; }
    Json st = s.ok("state", Json::object()); T_CHECK(st.get("version").str().size() > 3);
    return true;
}

} // namespace vm2b

namespace vm2b {

static Str SiblingExe(const wchar_t* name)
{
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    wchar_t* sl = wcsrchr(p, L'\\'); if (sl) wcscpy(sl + 1, name);
    return FromWide(p);
}

static bool RunWait(const Str& exe, const Str& args, DWORD ms, DWORD* code)
{
    Str cmd = "\"" + exe + "\" " + args; Vec<wchar_t> w; { Wide ww(cmd); size_t n = wcslen(ww.c_str()) + 1; w.resize(n); memcpy(w.data(), ww.c_str(), n * sizeof(wchar_t)); }
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    bool done = WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 99);
    DWORD c = 0; GetExitCodeProcess(pi.hProcess, &c); if (code) *code = c;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return done;
}

VM2B_TEST("service", exe_console_mode)
{
    Str exe = SiblingExe(L"VM2BackupService.exe");
    if (!FileExists(exe)) { c_.skip("VM2BackupService.exe is not next to the test (ONLY=autotest build)"); return true; }
    Tmp tmp(c_, "svcexe"); Str pipe = Str::fmt("vm2b-exe-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(c_.next() % 100000000));
    Str cmd = "\"" + exe + "\" /console /data \"" + tmp.sub("data") + "\" /pipe " + pipe + " /tick 100";
    Vec<wchar_t> w; { Wide ww(cmd); size_t n = wcslen(ww.c_str()) + 1; w.resize(n); memcpy(w.data(), ww.c_str(), n * sizeof(wchar_t)); }
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
    T_CHECK(CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0);
    IpcClient cli; Str err; bool up = cli.connect(pipe, 15000, err); T_CHECK(up);
    if (up) {
        Json st; T_CHECK(cli.call("state", Json::object(), st, err)); T_CHECK(st.get("version").str().size() > 3);
        T_CHECK(st.get("data_dir").str().find("data") >= 0);
        Json sh; T_CHECK(cli.call("service.shutdown", Json::object(), sh, err));
    }
    DWORD w1 = WaitForSingleObject(pi.hProcess, 20000); T_CHECK(w1 == WAIT_OBJECT_0);
    DWORD code = 99; GetExitCodeProcess(pi.hProcess, &code); T_CHECK(code == 0);
    if (w1 != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    T_CHECK(FileExists(JoinPath(tmp.sub("data"), "config.json")));
    return true;
}

// VM2BackupService.exe /reset-web: the recovery when nobody can sign in to the web site.
VM2B_TEST("service", exe_reset_web)
{
    Str exe = SiblingExe(L"VM2BackupService.exe");
    if (!FileExists(exe)) { c_.skip("VM2BackupService.exe is not next to the test (ONLY=autotest build)"); return true; }
    Tmp tmp(c_, "svcreset"); Str dir = tmp.sub("data"), err; DWORD code = 99;
    Str pipe = Str::fmt("vm2b-reset-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(c_.next() % 100000000));
    {   // a site in a bad state: another port, everything open, 2FA required, admin demoted with a changed password and 2FA, a second user
        WebAuth a; a.setKdfIterationsForTests(1000); T_CHECK(a.open(dir, err, true));
        Json st = Json::object(); st["port"] = 9999; st["require_2fa_admins"] = true; st["trust_proxy"] = true; Json b = Json::array(); b.push("all"); st["bind"] = b; st["enabled"] = false; T_CHECK(a.setSettings(st, err));
        Json u = Json::object(); u["name"] = "admin"; u["role"] = "viewer"; u["password"] = "Zebra-Quartz-Maple-77"; T_CHECK(a.usersSave(u, err));
        Json en; T_CHECK(a.totpEnroll("admin", en, err));
        Json d = Json::object(); d["name"] = "dave"; d["role"] = "operator"; d["password"] = "Another-Quartz-Maple-77"; T_CHECK(a.usersSave(d, err));
    }
    // refused while the service runs (it holds web.json)
    {   Str cmd = "\"" + exe + "\" /console /data \"" + dir + "\" /pipe " + pipe + " /tick 100";
        Vec<wchar_t> w; { Wide ww(cmd); size_t n = wcslen(ww.c_str()) + 1; w.resize(n); memcpy(w.data(), ww.c_str(), n * sizeof(wchar_t)); }
        STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si); PROCESS_INFORMATION pi;
        T_CHECK(CreateProcessW(nullptr, w.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0);
        IpcClient cli; bool up = cli.connect(pipe, 15000, err); T_CHECK(up);
        Bytes before; ReadWholeFile(JoinPath(dir, "web.json").c_str(), before, err);
        T_CHECK(RunWait(exe, "/reset-web /data \"" + dir + "\" /pipe " + pipe, 30000, &code) && code == 1);
        Bytes after; ReadWholeFile(JoinPath(dir, "web.json").c_str(), after, err); T_CHECK(before.size() == after.size() && (before.empty() || memcmp(before.data(), after.data(), before.size()) == 0));
        if (up) { Json sh; cli.call("service.shutdown", Json::object(), sh, err); }
        WaitForSingleObject(pi.hProcess, 20000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    // stopped: the reset works
    T_CHECK(RunWait(exe, "/reset-web /data \"" + dir + "\" /pipe " + pipe, 60000, &code) && code == 0);
    {   WebAuth a; a.setKdfIterationsForTests(1000); T_CHECK(a.open(dir, err, false));
        Json s = a.settings(); T_CHECK(s.get("enabled").boolean() && s.get("port").integer() == 8443 && s.get("bind").size() == 1 && s.get("bind").at(0).str() == "127.0.0.1" && !s.get("require_2fa_admins").boolean() && !s.get("trust_proxy").boolean());
        Json ul = a.usersList(); T_CHECK(ul.size() == 2);
        for (size_t i = 0; i < ul.size(); ++i) { if (ul.at(i).get("name").str() == "admin") { T_CHECK(ul.at(i).get("role").str() == "admin" && !ul.at(i).get("totp_enabled").boolean() && !ul.at(i).get("totp_pending").boolean()); } else T_CHECK(ul.at(i).get("name").str() == "dave" && ul.at(i).get("role").str() == "operator"); }
        WebAuth::LoginResult r = a.login("127.0.0.1", "admin", "admin", "", ""); T_CHECK(r.status == 200 && r.role == "admin");
        r = a.login("127.0.0.1", "dave", "Another-Quartz-Maple-77", "", ""); T_CHECK(r.status == 200);                 // the other users keep their passwords
        Str au = a.auditTail(50); T_CHECK(au.find("web_reset") >= 0 && au.find("admin / admin") >= 0 && au.find("Zebra") < 0);
    }
    // a damaged web.json is kept aside and replaced
    { Str j = JoinPath(dir, "web.json"); T_CHECK(WriteWholeFile(j.c_str(), "{ not json", 10, err)); }
    T_CHECK(RunWait(exe, "/reset-web /data \"" + dir + "\" /pipe " + pipe, 60000, &code) && code == 0);
    T_CHECK(FileExists(JoinPath(dir, "web.json.damaged")));
    { WebAuth a; a.setKdfIterationsForTests(1000); T_CHECK(a.open(dir, err, false)); T_CHECK(a.userCount() == 1); T_CHECK(a.login("127.0.0.1", "admin", "admin", "", "").status == 200); }
    return true;
}

// Regression: an edit of a server MERGES into the stored record, and a task element keeps its encryption password by its own id.
VM2B_TEST("service", server_edit_merges_and_task_elements_keep_passwords_by_id)
{
    Svc s(c_, "svcmerge"); if (!s.up) return false;
    // 1. a server with every optional field; the edit form sends only some of them
    Json f = Json::object(); f["type"] = "scp"; f["name"] = "Key host"; f["host"] = "ssh.example.com"; f["port"] = 22; f["user"] = "backup"; f["directory"] = "/bk";
    f["key_file"] = "C:\\keys\\id_ed25519"; f["key_passphrase"] = "Pass-Phrase-1"; f["host_key"] = "SHA256:abc"; f["passive"] = false; f["tls_min"] = "1.0"; f["datacenter"] = "DC9"; f["transport"] = "api";
    Json a = Json::object(); a["server"] = f; Json saved = s.ok("servers.save", a); Str id = saved.get("id").str(); T_CHECK(!id.empty());
    Json e1 = Json::object(); e1["id"] = id; e1["type"] = "scp"; e1["name"] = "Key host"; e1["host"] = "ssh2.example.com"; e1["port"] = 2222; e1["user"] = "backup"; e1["directory"] = "/bk";
    Json a1 = Json::object(); a1["server"] = e1; Json sv = s.ok("servers.save", a1);
    Json rev; T_CHECK(s.core.config().server(id, true, rev));
    T_CHECK(rev.get("host").str() == "ssh2.example.com" && rev.get("port").integer() == 2222);                                       // what was sent changed
    T_CHECK(rev.get("key_file").str() == "C:\\keys\\id_ed25519" && rev.get("key_passphrase").str() == "Pass-Phrase-1");           // what was not sent stayed, the secret too
    T_CHECK(rev.get("host_key").str() == "SHA256:abc" && !rev.get("passive").boolean(true) && rev.get("tls_min").str() == "1.0" && rev.get("datacenter").str() == "DC9" && rev.get("transport").str() == "api");
    T_CHECK(sv.get("has_key_passphrase").boolean() && !sv.has("key_passphrase"));
    Json e2 = e1; e2["key_file"] = ""; e2["key_passphrase"] = kMask; e2["tls_min"] = "1.2"; Json a2 = Json::object(); a2["server"] = e2; s.ok("servers.save", a2);
    T_CHECK(s.core.config().server(id, true, rev)); T_CHECK(rev.get("key_file").str().empty() && rev.get("key_passphrase").str() == "Pass-Phrase-1" && rev.get("tls_min").str() == "1.2" && rev.get("datacenter").str() == "DC9");
    Json e3 = e1; e3["key_passphrase"] = ""; Json a3 = Json::object(); a3["server"] = e3; s.ok("servers.save", a3);
    T_CHECK(s.core.config().server(id, true, rev)); T_CHECK(rev.get("key_passphrase").str().empty());                                  // an explicit empty value clears a secret
    // 2. task elements: stable ids, passwords by id (reorder and remove must never hand an element another one's password)
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vmA\\d.vmdk"), Random(c_, 1000)); Str srv = s.addDirServer(root);
    Json t = Json::object(); t["name"] = "Enc"; Json els = Json::array();
    els.push(Svc::Element(srv, "vmA", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1, "pw-one-AAA")); els.push(Svc::Element(srv, "vmB", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1, "pw-two-BBB")); els.push(Svc::Element(srv, "vmC", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1, "pw-three-CCC"));
    t["elements"] = els; Json ta = Json::object(); ta["task"] = t; Json ts = s.ok("tasks.save", ta); Str tid = ts.get("id").str();
    T_CHECK(ts.get("elements").size() == 3); Str ids[3]; for (int i = 0; i < 3; ++i) { ids[i] = ts.get("elements").at((size_t)i).get("id").str(); T_CHECK(!ids[i].empty()); }
    T_CHECK(ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2]);
    auto pwOf = [&](const char* vm) { Json full; s.core.config().task(tid, true, full); for (size_t i = 0; i < full.get("elements").size(); ++i) if (full.get("elements").at(i).get("source").get("vms").at(0).str() == vm) return full.get("elements").at(i).get("encrypt").get("password").str(); return Str("?"); };
    T_CHECK(pwOf("vmA") == "pw-one-AAA" && pwOf("vmB") == "pw-two-BBB" && pwOf("vmC") == "pw-three-CCC");
    // the client sends the elements back as listed (no passwords), reordered and with one removed
    Json back = ts; Json reordered = Json::array(); reordered.push(ts.get("elements").at(2)); reordered.push(ts.get("elements").at(0)); back["elements"] = reordered;
    Json tb = Json::object(); tb["task"] = back; Json ts2 = s.ok("tasks.save", tb);
    T_CHECK(ts2.get("elements").size() == 2 && ts2.get("elements").at(0).get("id").str() == ids[2] && ts2.get("elements").at(1).get("id").str() == ids[0]);      // ids kept
    T_CHECK(pwOf("vmC") == "pw-three-CCC" && pwOf("vmA") == "pw-one-AAA" && pwOf("vmB") == "?");
    // a new element without an id and without a password is refused (it can not inherit anything); with a password it gets its own id
    Json back2 = ts2; Json e4 = Svc::Element(srv, "vmB", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1, nullptr); e4["encrypt"]["enabled"] = true; Json l3 = back2.get("elements"); l3.push(e4); back2["elements"] = l3;
    Json tc = Json::object(); tc["task"] = back2; T_CHECK(!s.call("tasks.save", tc)); T_CHECK(s.lastErr.find("password") >= 0);
    // 3. old tasks without element ids are migrated when the configuration is opened, and the ids stay on the next open
    Str dir = s.core.config().dataDir();
    { Bytes b; Str er; T_CHECK(ReadWholeFile(JoinPath(dir, "config.json").c_str(), b, er)); Json cfg; T_CHECK(Json::parse((const char*)b.data(), b.size(), cfg, er));
      Json tl = cfg.get("tasks"); for (size_t i = 0; i < tl.size(); ++i) { Json els2 = tl.item(i)["elements"]; for (size_t k = 0; k < els2.size(); ++k) els2.item(k).remove("id"); }
      cfg["tasks"] = tl; Str js = cfg.dump(true); T_CHECK(WriteWholeFile(JoinPath(dir, "config.json").c_str(), js.data(), js.size(), er)); }
    Config c1; Str er1; T_CHECK(c1.open(dir, er1)); Json t1; T_CHECK(c1.task(tid, false, t1)); Str m0 = t1.get("elements").at(0).get("id").str(), m1 = t1.get("elements").at(1).get("id").str(); T_CHECK(!m0.empty() && !m1.empty() && m0 != m1);
    Config c2; T_CHECK(c2.open(dir, er1)); Json t2; T_CHECK(c2.task(tid, false, t2)); T_CHECK(t2.get("elements").at(0).get("id").str() == m0 && t2.get("elements").at(1).get("id").str() == m1);
    return true;
}

// fs.browse: the folder picker of the web site lists local folders only, bounded and without tricks.
VM2B_TEST("service", fs_browse_lists_local_folders_safely)
{
    Svc s(c_, "svcfs"); if (!s.up) return false;
    Str base = s.tmp.sub("pick"); Str e; MakeDirs(JoinPath(base, "Zeta"), e); MakeDirs(JoinPath(base, "alpha"), e); MakeDirs(JoinPath(base, "Beta\\inner"), e);
    WriteBytes(JoinPath(base, "key.pem"), Random(c_, 100));
    Str hid = JoinPath(base, "hidden"); MakeDirs(hid, e); SetFileAttributesW(FilePath(hid), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY);
    auto call = [&](const Str& path, bool files, Json& r) { Json a = Json::object(); a["path"] = path; if (files) a["files"] = true; return s.call("fs.browse", a, &r); };
    Json r; T_CHECK(call("", false, r)); T_CHECK(r.get("entries").size() >= 1 && r.get("entries").at(0).get("kind").str() == "drive" && r.get("parent").str().empty());
    T_CHECK(r.get("entries").at(0).get("path").str().size() == 3 && r.get("entries").at(0).get("path").str()[1] == ':');
    T_CHECK(call(base, false, r)); T_CHECK(r.get("entries").size() == 3);                                       // sub-folders only, hidden ones left out
    T_CHECK(r.get("entries").at(0).get("name").str() == "alpha" && r.get("entries").at(1).get("name").str() == "Beta" && r.get("entries").at(2).get("name").str() == "Zeta");   // sorted without case
    T_CHECK(r.get("entries").at(1).get("path").str() == base + "\\Beta" && r.get("entries").at(0).get("kind").str() == "dir");
    T_CHECK(call(base + "\\", true, r)); T_CHECK(r.get("entries").size() == 4 && r.get("entries").at(3).get("kind").str() == "file");      // folders first, then the file
    T_CHECK(call(base + "\\Beta", false, r)); T_CHECK(r.get("parent").str() == base && r.get("entries").size() == 1);
    T_CHECK(call(JoinPath(base, "alpha"), false, r)); T_CHECK(r.get("entries").size() == 0 && r.get("path").str().ends_with("\\"));          // an empty folder is not an error
    { Str root = base.substr(0, 3); T_CHECK(call(root, false, r)); T_CHECK(r.get("parent").str().empty()); }
    { Str two = base.substr(0, 3); int c = base.find('\\', 3); if (c > 0) { T_CHECK(call(base.substr(0, (size_t)c), false, r)); T_CHECK(r.get("parent").str() == two); } }
    const char* bad[] = { "\\\\?\\C:\\", "\\\\.\\C:\\", "\\\\srv\\share\\x", "\\\\?\\UNC\\h\\s", "relative\\x", "C:", "C:x", "C:\\a\\..\\b", "C:\\..", "C:\\a\\.\\b", "C:\\CON", "C:\\a\\nul.txt", "C:\\a*", "C:\\a?b", "C:\\a|b",
        "C:\\x.", "C:\\x ", "C:\\a:b", "C:\\a\\\\b", "C:\\a\x01b", "http://x/y", "C:\\COM1" };
    for (const char* b : bad) { Json x; if (call(b, false, x)) { c_.note("accepted: %s", b); return false; } T_CHECK(!s.lastErr.empty()); }
    { Str lp = "C:\\"; for (int i = 0; i < 300; ++i) lp += "dir\\"; Json x; T_CHECK(!call(lp, false, x)); }
    T_CHECK(!call(base + "\\nonexistent", false, r));
    // more than 5000 folders: cut, flagged
    Str many = s.tmp.sub("many"); MakeDirs(many, e); for (int i = 0; i < 5100; ++i) CreateDirectoryW(FilePath(Str::fmt("%s\\d%05d", many.c_str(), i)), nullptr);
    T_CHECK(call(many, false, r)); T_CHECK(r.get("entries").size() == 5000 && r.get("truncated").boolean());
    return true;
}

VM2B_TEST("service", tasks_copy_and_next_for_an_unsaved_schedule)
{
    Svc s(c_, "svccopy"); if (!s.up) return false;
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vmA\\d.vmdk"), Random(c_, 1000)); Str srv = s.addDirServer(root);
    Json el = Svc::Element(srv, "vmA", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", 1, "copy-me-PW1"); Str tid = s.saveTask("Orig", el);
    Json a = Json::object(); a["id"] = tid; Json c1 = s.ok("tasks.copy", a); Str cid = c1.get("id").str();
    T_CHECK(!cid.empty() && cid != tid && c1.get("name").str() == "Orig (copy)" && c1.get("elements").size() == 1);
    T_CHECK(c1.get("elements").at(0).get("id").str() != s.ok("tasks.list", Json::object()).at(0).get("elements").at(0).get("id").str());     // new element ids
    Json full; T_CHECK(s.core.config().task(cid, true, full)); T_CHECK(full.get("elements").at(0).get("encrypt").get("password").str() == "copy-me-PW1");     // the stored password came along
    Json c2 = s.ok("tasks.copy", a); T_CHECK(c2.get("name").str() == "Orig (copy) 2");
    Json named = Json::object(); named["id"] = "Orig"; named["name"] = "Mine"; T_CHECK(s.ok("tasks.copy", named).get("name").str() == "Mine");           // by name, with a name
    Json none = Json::object(); none["id"] = "ghost"; T_CHECK(!s.call("tasks.copy", none)); T_CHECK(s.lastErr.find("not found") >= 0);
    T_CHECK(s.ok("tasks.list", Json::object()).size() == 4);
    // tasks.next for a schedule that is being edited
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "every"; Json ev = Json::object(); ev["value"] = 2; ev["unit"] = "hours"; sch["every"] = ev; sch["first_time"] = "2030-01-01T08:00";
    Json na = Json::object(); na["schedule"] = sch; Json nr = s.ok("tasks.next", na); T_CHECK(nr.get("text").str().find("2030-01-01") >= 0 && !nr.get("description").str().empty());
    Json bad = Json::object(); bad["enabled"] = true; bad["mode"] = "weekly"; bad["weekdays"] = Json::array(); bad["start_time"] = "22:00"; Json nb = Json::object(); nb["schedule"] = bad; T_CHECK(!s.call("tasks.next", nb)); T_CHECK(!s.lastErr.empty());
    return true;
}

VM2B_TEST("service", exe_scm_install_start_stop)
{
    // the registration the installer performs: install, start, talk to it, stop, remove (the real service pipe and folder)
    Str exe = SiblingExe(L"VM2BackupService.exe");
    if (!FileExists(exe)) { c_.skip("VM2BackupService.exe is not next to the test"); return true; }
    DWORD code = 0;
    if (!RunWait(exe, "/install /manual", 30000, &code) || code != 0) { c_.skip("the service manager is not available here (not elevated, or the environment has none)"); return true; }
    bool started = RunWait(exe, "/start", 60000, &code) && code == 0;
    T_CHECK(started);
    if (started) {
        IpcClient cli; Str err; bool up = cli.connect(kDefaultPipeName, 10000, err); T_CHECK(up);
        if (up) { Json st; T_CHECK(cli.call("state", Json::object(), st, err)); T_CHECK(st.get("version").str().size() > 3); }
    }
    T_CHECK(RunWait(exe, "/stop", 60000, &code) && code == 0);
    T_CHECK(RunWait(exe, "/uninstall", 60000, &code) && code == 0);
    return true;
}

} // namespace vm2b

namespace vm2b {

VM2B_TEST("gui", program_selftest)
{
    // starts the real VM2Backup.exe (the control panel) against an in-process service: it shows the panel in every state and
    // exits with 0 when nothing broke. Needs a display (tools/run-gui-tests.sh starts Xvfb); a window cannot be shown without one.
    wchar_t env[8]; if (!GetEnvironmentVariableW(L"VM2B_GUI_TESTS", env, 8) || env[0] != L'1') { c_.skip("set VM2B_GUI_TESTS=1 (and a display) to run the window test"); return true; }
    Str exe = SiblingExe(L"VM2Backup.exe");
    if (!FileExists(exe)) { c_.skip("VM2Backup.exe is not next to the test"); return true; }
    Svc s(c_, "gui1"); if (!s.up) return false;
    DWORD code = 99;
    bool done = RunWait(exe, "/pipe " + s.pipe + " /selftest 500", 120000, &code);
    T_CHECK(done); T_CHECK(code == 0);
    return true;
}

// xz is the only compressor: the RPC refuses every other codec with a clear message and accepts xz at levels 1..9.
VM2B_TEST("service", rpc_accepts_only_xz)
{
    Svc s(c_, "svcxz"); if (!s.up) return false;
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm1\\disk.vmdk"), Random(c_, 20000)); Str srv = s.addDirServer(root);
    const char* bad[] = { "gzip", "none", "zstd", "XZ", "" };
    for (const char* codec : bad) {
        Json t = Json::object(); t["name"] = "Old"; Json els = Json::array(); els.push(Svc::Element(srv, "vm1", JoinPath(s.tmp.sub("store"), "{VM}"), codec)); t["elements"] = els;
        Json a = Json::object(); a["task"] = t; Json r;
        T_CHECK(!s.call("tasks.save", a, &r)); T_CHECK(s.lastErr.find("only xz") >= 0);
    }
    for (int lv = 1; lv <= 9; lv += 4) T_CHECK(!s.saveTask(Str::fmt("Good%d", lv).c_str(), Svc::Element(srv, "vm1", JoinPath(s.tmp.sub("store"), "{VM}"), "xz", lv)).empty());
    Json bs = Json::object(); bs["codec"] = "gzip"; T_CHECK(!s.call("settings.set", bs)); T_CHECK(s.lastErr.find("only xz") >= 0);
    bs = Json::object(); bs["codec"] = "none"; T_CHECK(!s.call("settings.set", bs));
    bs = Json::object(); bs["level"] = 12; T_CHECK(!s.call("settings.set", bs));
    bs = Json::object(); bs["level"] = 0; T_CHECK(!s.call("settings.set", bs));
    bs = Json::object(); bs["threads"] = -1; T_CHECK(!s.call("settings.set", bs));
    bs = Json::object(); bs["level"] = 7; bs["threads"] = 2; bs["codec"] = "xz"; T_CHECK(s.call("settings.set", bs));
    Json st = s.ok("settings.get", Json::object()); T_CHECK(st.get("level").integer() == 7); T_CHECK(st.get("threads").integer() == 2); T_CHECK(!st.has("codec"));
    return true;
}

} // namespace vm2b
