// Shared helpers of the in-process service tests (tests_service.cpp, tests_svc_esxi.cpp): scratch folder, random bytes, and `Svc` = a ServiceCore
// with its own pipe, data folder and (optionally) a clock the test can move, plus an IpcClient connected to it.
#pragma once
#include "autotest.h"
#include "svccore.h"
#include "winutil.h"

namespace vm2b {

namespace {

[[maybe_unused]] i64 g_fakeNow = 0;
inline i64 FakeClock() { return g_fakeNow; }

struct Tmp {
    Str path; bool keep;
    Tmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path);                                          // a fresh folder: leftovers of an earlier (crashed) run must not leak in
        Str e; MakeDirs(path, e);
    }
    ~Tmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

inline bool WriteBytes(const Str& path, const Bytes& b)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    return WriteWholeFile(path.c_str(), b.data(), b.size(), e);
}

inline Bytes Random(Ctx& c, size_t n) { Bytes b; b.resize(n); c.fill(b.data(), n); return b; }

// A service with everything of its own.
struct Svc {
    Ctx& c; Tmp tmp; ServiceCore core; IpcClient cli; bool up; Str pipe; Str lastErr;
    ServiceOptions opt; bool record; Str transcript;     // record: every answer and every error text is appended to `transcript` (tests that look for leaked secrets)
    Svc(Ctx& ctx, const char* tag, bool fakeClock = false, bool defaultSddl = false, void (*onShutdown)() = nullptr) : c(ctx), tmp(ctx, tag), up(false), record(false) {
        ServiceOptions& o = opt; o.dataDir = tmp.sub("data"); pipe = Str::fmt("vm2b-test-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(ctx.next() % 100000000));
        o.pipeName = pipe; o.tickMs = 40; if (!defaultSddl) o.pipeSddl = "D:(A;;GA;;;WD)"; if (fakeClock) o.clock = FakeClock; o.onShutdown = onShutdown;
        Str err; up = core.start(o, err);
        if (!up) { c.note("service did not start: %s", err.c_str()); c.failed = true; return; }
        if (!cli.connect(pipe, 5000, err)) { c.note("cannot connect: %s", err.c_str()); c.failed = true; }
        Json st = Json::object(); st["kdf_iterations"] = 3000; st["retry_base_ms"] = 1; st["retry_attempts"] = 6; st["segment_mib"] = 1; st["retry_max_sec"] = 1;
        call("settings.set", st);
    }
    ~Svc() { cli.close(); core.stop(); }
    // stops the service and starts a new one on the same data folder and pipe (what a restart of the Windows service does; running jobs are paused)
    bool restart() {
        cli.close(); core.stop(); core.~ServiceCore(); new (&core, vm2b_place_tag()) ServiceCore();
        Str err; up = core.start(opt, err);
        if (!up) { c.note("service did not restart: %s", err.c_str()); c.failed = true; return false; }
        if (!cli.connect(pipe, 5000, err)) { c.note("cannot reconnect: %s", err.c_str()); c.failed = true; return false; }
        return true;
    }
    bool call(const char* cmd, const Json& args, Json* res = nullptr) {
        Json r; Str e; bool ok = cli.call(cmd, args, r, e); lastErr = e; if (res) *res = r;
        if (record) { transcript += cmd; transcript += '\n'; transcript += r.dump(); transcript += '\n'; transcript += e; transcript += '\n'; }
        return ok;
    }
    Json ok(const char* cmd, const Json& args) { Json r; bool k = call(cmd, args, &r); if (!k) { c.note("%s failed: %s", cmd, lastErr.c_str()); c.failed = true; } return r; }
    Str addDirServer(const Str& root, const char* name = "Workstation VMs") {
        Json s = Json::object(); s["type"] = "directory"; s["name"] = name; s["path"] = root; Json a = Json::object(); a["server"] = s;
        Json r = ok("servers.save", a); return r.get("id").str();
    }
    static Json Element(const Str& srv, const char* vm, const Str& tgtDir, const char* codec = "xz", int level = 1, const char* pw = nullptr) {
        Json e = Json::object(); Json s = Json::object(); s["server"] = srv; Json vms = Json::array(); vms.push(vm); s["vms"] = vms; e["source"] = s;
        Json t = Json::object(); t["server"] = "local"; t["directory"] = tgtDir; e["target"] = t;
        Json cp = Json::object(); cp["codec"] = codec; cp["level"] = level; e["compress"] = cp;
        Json en = Json::object(); en["enabled"] = pw != nullptr; if (pw) en["password"] = pw; e["encrypt"] = en;
        e["verify"] = true; e["keep"] = 0; return e;
    }
    Str saveTask(const char* name, const Json& element, const Json& schedule = Json(), const Json& windows = Json()) {
        Json t = Json::object(); t["name"] = name; t["enabled"] = true; Json els = Json::array(); els.push(element); t["elements"] = els;
        if (schedule.is_object()) t["schedule"] = schedule; if (windows.is_array()) t["windows"] = windows;
        Json a = Json::object(); a["task"] = t; Json r = ok("tasks.save", a); return r.get("id").str();
    }
    // waits until the job is over; returns its final state ("" = timeout)
    Str waitJob(const Str& id, int ms = 60000) {
        DWORD t0 = GetTickCount();
        while ((int)(GetTickCount() - t0) < ms) {
            Json jobs = ok("jobs.list", Json::object());
            bool found = false;
            for (size_t i = 0; i < jobs.size(); ++i) if (jobs.at(i).get("id").str() == id) { found = true; Str st = jobs.at(i).get("status").str(); if (st != "Waiting" && st != "Running") return st; }
            if (!found) return "Gone";
            Sleep(30);
        }
        return "";
    }
    int historyCount() { Json a = Json::object(); a["limit"] = 1000; return (int)ok("history.list", a).size(); }
    bool waitHistory(int n, int ms = 60000) { DWORD t0 = GetTickCount(); while ((int)(GetTickCount() - t0) < ms) { if (historyCount() >= n) return true; Sleep(30); } return false; }
};

} // namespace

} // namespace vm2b
