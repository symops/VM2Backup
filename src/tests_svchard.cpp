// Autotests of the service core under stress: job queue limits, history trimming, damaged / half-written data files, odd names.
#include "autotest.h"
#include "svccore.h"
#include "schedule.h"
#include "winutil.h"
#include "tests_svc.h"

namespace vm2b {

static Bytes ReadAll(const Str& p) { Bytes b; Str e; ReadWholeFile(p.c_str(), b, e); return b; }
static void PutAll(const Str& p, const Bytes& b) { Str e; WriteWholeFile(p.c_str(), b.data(), b.size(), e); }
static void PutText(const Str& p, const Str& s) { Str e; WriteWholeFile(p.c_str(), s.data(), s.size(), e); }

VM2B_TEST("service", max_parallel_is_respected)
{
    for (int variant = 0; variant < 2; ++variant) {
        Svc s(c_, "svcmp"); if (!s.up) return false;
        int maxp = variant == 0 ? 1 : 2;
        Json st = Json::object(); st["max_parallel"] = maxp; st["speed_limit_mbit"] = 16.0; s.ok("settings.set", st);        // ~2 MB/s: every job lasts a while
        Str root = s.tmp.sub("vms"); Str srv = s.addDirServer(root);
        Vec<Str> ids; int tasks = 4;
        for (int i = 0; i < tasks; ++i) {
            WriteBytes(JoinPath(root, Str::fmt("vm%d\\disk.vmdk", i)), Random(c_, 1200000));
            Str name = Str::fmt("T%d", i);
            ids.push_back(s.saveTask(name.c_str(), Svc::Element(srv, Str::fmt("vm%d", i).c_str(), JoinPath(s.tmp.sub("store"), "{VM}"), "xz")));
        }
        Vec<Str> jobIds;
        for (int i = 0; i < tasks; ++i) { Json a = Json::object(); a["id"] = ids[(size_t)i]; Json r = s.ok("tasks.run", a); jobIds.push_back(r.get("job").str()); }
        int seenMax = 0; DWORD t0 = GetTickCount(); size_t done = 0;
        while (GetTickCount() - t0 < 90000) {
            Json jobs = s.ok("jobs.list", Json::object()); int running = 0; done = 0;
            for (size_t i = 0; i < jobs.size(); ++i) if (jobs.at(i).get("status").str() == "Running") ++running;
            for (const Str& id : jobIds) { bool alive = false; for (size_t i = 0; i < jobs.size(); ++i) if (jobs.at(i).get("id").str() == id) { Str sts = jobs.at(i).get("status").str(); if (sts == "Waiting" || sts == "Running") alive = true; } if (!alive) ++done; }
            if (running > seenMax) seenMax = running;
            T_CHECK(running <= maxp);
            if (done == jobIds.size()) break;
            Sleep(15);
        }
        T_CHECK(done == jobIds.size());
        T_CHECK(seenMax >= 1); if (maxp == 2) T_CHECK(seenMax == 2);                                                       // and it does use the parallelism it is allowed
        T_CHECK(s.waitHistory(tasks, 20000));
        Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 50; return a; }());
        for (size_t i = 0; i < hl.size(); ++i) T_CHECK(hl.at(i).get("status").str() == "Success");
    }
    return true;
}

VM2B_TEST("service", history_is_trimmed_and_survives_restart)
{
    Svc s(c_, "svchist"); if (!s.up) return false;
    Json st = Json::object(); st["max_history"] = 10; s.ok("settings.set", st);
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm\\a.vmx"), Random(c_, 500)); Str srv = s.addDirServer(root);
    Str task = s.saveTask("H", Svc::Element(srv, "vm", JoinPath(s.tmp.sub("store"), "{VM}"), "xz"));
    Vec<Str> ids;
    for (int i = 0; i < 14; ++i) { Json a = Json::object(); a["id"] = task; Json r; for (int tries = 0; tries < 100 && !s.call("tasks.run", a, &r); ++tries) Sleep(50); Str id = r.get("job").str(); s.waitJob(id); }
    s.waitHistory(10);
    Sleep(300);
    Json a = Json::object(); a["limit"] = 100; Json hl = s.ok("history.list", a);
    T_CHECK(hl.size() == 10);                                                                       // trimmed to max_history
    // newest first and strictly ordered
    for (size_t i = 1; i < hl.size(); ++i) T_CHECK(hl.at(i - 1).get("started").num() >= hl.at(i).get("started").num());
    Str newest = hl.size() ? hl.at(0).get("id").str() : Str();
    // history.get works for the kept ones and says so for a trimmed one
    Json ga = Json::object(); ga["id"] = newest; T_CHECK(s.call("history.get", ga));
    Json gb = Json::object(); gb["id"] = "no-such-record"; T_CHECK(!s.call("history.get", gb));
    // a lower limit than the file holds is applied at the next save, not by deleting the file
    T_CHECK(s.restart());
    Json hl2 = s.ok("history.list", a); T_CHECK(hl2.size() == 10); T_CHECK(hl2.size() && hl2.at(0).get("id").str() == newest);
    Json c = Json::object(); s.ok("history.clear", c); T_CHECK(s.historyCount() == 0);
    T_CHECK(s.restart()); T_CHECK(s.historyCount() == 0);
    return true;
}

// Power loss while a data file is being rewritten, or a damaged disk: the other files' data must survive, and the service must neither
// lose the configuration silently nor start with an empty one.
VM2B_TEST("service", damaged_data_files)
{
    Svc s(c_, "svcdmg"); if (!s.up) return false;
    Str root = s.tmp.sub("vms"); WriteBytes(JoinPath(root, "vm\\a.vmx"), Random(c_, 500)); Str srv = s.addDirServer(root);
    Str task = s.saveTask("Keep me", Svc::Element(srv, "vm", JoinPath(s.tmp.sub("store"), "{VM}"), "xz"));
    { Json a = Json::object(); a["id"] = task; Json r = s.ok("tasks.run", a); s.waitJob(r.get("job").str()); }
    T_CHECK(s.waitHistory(1)); Sleep(300);
    Str data = s.core.config().dataDir();
    Str cfgP = JoinPath(data, "config.json"), hisP = JoinPath(data, "history.json"), stP = JoinPath(data, "state.json");
    Bytes cfg = ReadAll(cfgP), his = ReadAll(hisP), stt = ReadAll(stP);
    T_CHECK(!cfg.empty() && !his.empty());
    s.cli.close(); s.core.stop();
    auto startAgain = [&](Str& err) { ServiceCore* sc = &s.core; sc->~ServiceCore(); new (sc, vm2b_place_tag()) ServiceCore(); return sc->start(s.opt, err); };
    // 1. history.json truncated at random places / empty / garbage: the service starts, the tasks and servers are intact
    for (int t = 0; t < 6; ++t) {
        Bytes h = his;
        if (t == 0) h.resize(0); else if (t == 1) { h.resize(0); PutText(hisP, "not json at all"); }
        else h.resize(c_.rnd((u32)h.size()));
        if (t != 1) PutAll(hisP, h);
        Str err; bool up = startAgain(err); T_CHECK(up); if (!up) { c_.note("start: %s", err.c_str()); continue; }
        T_CHECK(s.core.config().tasks().size() == 1); T_CHECK(s.core.config().servers().size() == 2);
        // new history can be written again
        Json rec = Json::object(); rec["id"] = "x"; rec["status"] = "Success"; s.core.config().addHistory(rec);
        T_CHECK(s.core.config().history(0).size() >= 1);
        s.core.stop();
    }
    // 2. state.json damaged
    for (int t = 0; t < 4; ++t) {
        Bytes h = stt; if (t == 0) h.resize(0); else if (t == 1) PutText(stP, "[1,2,"); else h.resize(h.empty() ? 0 : c_.rnd((u32)h.size()));
        if (t != 1) PutAll(stP, h);
        PutAll(hisP, his);
        Str err; bool up = startAgain(err); T_CHECK(up); if (!up) continue;
        T_CHECK(s.core.config().tasks().size() == 1); T_CHECK(s.core.config().history(0).size() == 1);
        s.core.stop();
    }
    PutAll(stP, stt);
    // 3. a half-written temporary left by a power loss (config.json.tmp / history.json.tmp) is ignored and replaced
    PutText(cfgP + ".tmp", "{\"servers\":[ garbage"); PutText(hisP + ".tmp", "[{"); PutText(stP + ".tmp", "{");
    { Str err; bool up = startAgain(err); T_CHECK(up); if (up) { T_CHECK(s.core.config().tasks().size() == 1); T_CHECK(s.core.config().history(0).size() == 1);
        Json patch = Json::object(); patch["max_history"] = 77; Str e2; T_CHECK(s.core.config().setSettings(patch, e2)); s.core.stop(); } }
    T_CHECK(ReadAll(cfgP).size() > 100);
    // 4. config.json damaged (truncated): the service refuses to start (it must not replace the user's configuration by an empty one)
    //    and leaves the damaged file and the history exactly as they were
    Bytes good = ReadAll(cfgP);
    for (int t = 0; t < 5; ++t) {
        Bytes h = good; if (t == 0) h.resize(0); else h.resize(1 + c_.rnd((u32)h.size() - 1));
        PutAll(cfgP, h);
        Str err; bool up = startAgain(err);
        if (up) { T_CHECK(false); c_.note("started with a damaged config.json (%zu of %zu bytes)", h.size(), good.size()); s.core.stop(); }
        else { T_CHECK(err.find("config.json") >= 0); T_CHECK(ReadAll(cfgP).size() == h.size()); T_CHECK(ReadAll(hisP).size() == his.size()); }
    }
    PutAll(cfgP, good);
    { Str err; bool up = startAgain(err); T_CHECK(up); if (up) { T_CHECK(s.core.config().tasks().size() == 1); s.core.stop(); } }
    // leave a running object for the destructor
    { Str err; startAgain(err); }
    return true;
}


// Two different tasks that back up the SAME VM into the SAME folder at the same time (two schedules that coincide, or a manual run on top of a
// scheduled one): neither may adopt the other's journal or write its volumes; every finished backup must verify and restore.
VM2B_TEST("service", concurrent_tasks_same_vm_same_target)
{
    for (int round = 0; round < 3; ++round) {
        Svc s(c_, "svcconc"); if (!s.up) return false;
        Json st = Json::object(); st["max_parallel"] = 2; st["speed_limit_mbit"] = 24.0; s.ok("settings.set", st);
        Str root = s.tmp.sub("vms"); Str srv = s.addDirServer(root);
        Bytes disk = Random(c_, 3500000); WriteBytes(JoinPath(root, "vm\\disk.vmdk"), disk);
        Str store = JoinPath(s.tmp.sub("store"), "shared");
        const char* codec = "xz"; const char* pw = round == 2 ? "pw" : nullptr;
        Str t1 = s.saveTask("A", Svc::Element(srv, "vm", store, codec, 1, pw)), t2 = s.saveTask("B", Svc::Element(srv, "vm", store, codec, 1, pw));
        Json a1 = Json::object(); a1["id"] = t1; Json a2 = Json::object(); a2["id"] = t2;
        Json r1 = s.ok("tasks.run", a1), r2 = s.ok("tasks.run", a2);
        s.waitJob(r1.get("job").str()); s.waitJob(r2.get("job").str());
        T_CHECK(s.waitHistory(2, 30000));
        Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 10; return a; }());
        int okCount = 0; for (size_t i = 0; i < hl.size(); ++i) { if (hl.at(i).get("status").str() == "Success") ++okCount; else { Json hr = s.ok("history.get", [&] { Json a = Json::object(); a["id"] = hl.at(i).get("id").str(); return a; }()); c_.note("failed: %s", hr.get("results").dump().c_str()); } }
        // whatever happened, the folder holds only good backups, and every task that said Success has its own
        Json qa = Json::object(); qa["server"] = "local"; qa["directory"] = store; Json lst = s.ok("backups.list", qa);
        T_CHECK((int)lst.get("backups").size() == okCount); T_CHECK(lst.get("resumable").size() == 0);
        if (okCount < 2) c_.note("round %d: only %d of 2 succeeded (allowed if the other said why)", round, okCount);
        for (size_t i = 0; i < lst.get("backups").size(); ++i) {
            Json va = Json::object(); va["server"] = "local"; va["directory"] = store; va["archive"] = lst.get("backups").at(i).get("archive").str(); if (pw) va["password"] = pw;
            Json vr = s.ok("backups.verify", va); Str vs = s.waitJob(vr.get("job").str()); T_CHECK(vs == "Success" || vs == "Gone");
            Json rr = va; rr["dest"] = s.tmp.sub(Str::fmt("r%zu", i).c_str()); Json rj = s.ok("backups.restore", rr); Str rs = s.waitJob(rj.get("job").str()); T_CHECK(rs == "Success" || rs == "Gone");
            Bytes got = ReadAll(JoinPath(rr.get("dest").str(), "vm\\disk.vmdk")); T_CHECK(got.size() == disk.size() && !memcmp(got.data(), disk.data(), disk.size()));
        }
        T_CHECK(okCount >= 1);
    }
    return true;
}


// Same VM, same folder, DIFFERENT settings, the second task starting while the first one has a journal: it must not delete the first one's
// volumes ("another setting: the partial backup is of no use") -- both must end up as good backups.
VM2B_TEST("service", second_task_does_not_destroy_a_running_backup)
{
    Svc s(c_, "svcdest"); if (!s.up) return false;
    Json st = Json::object(); st["max_parallel"] = 2; st["speed_limit_mbit"] = 24.0; s.ok("settings.set", st);
    Str root = s.tmp.sub("vms"); Str srv = s.addDirServer(root);
    Bytes disk = Random(c_, 6000000); WriteBytes(JoinPath(root, "vm\\disk.vmdk"), disk);
    Str store = JoinPath(s.tmp.sub("store"), "shared");
    Str t1 = s.saveTask("A", Svc::Element(srv, "vm", store, "xz", 1)), t2 = s.saveTask("B", Svc::Element(srv, "vm", store, "xz", 1, "pw"));
    Json a1 = Json::object(); a1["id"] = t1; Json a2 = Json::object(); a2["id"] = t2;
    Json r1 = s.ok("tasks.run", a1);
    Sleep(1500);                                                      // A has finished a few 1 MiB volumes and holds a journal
    Json r2 = s.ok("tasks.run", a2);
    Str f1 = s.waitJob(r1.get("job").str()), f2 = s.waitJob(r2.get("job").str());
    T_CHECK(f1 == "Success" || f1 == "Gone"); T_CHECK(f2 == "Success" || f2 == "Gone");
    T_CHECK(s.waitHistory(2, 30000));
    Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 10; return a; }());
    for (size_t i = 0; i < hl.size(); ++i) T_CHECK(hl.at(i).get("status").str() == "Success");
    Json qa = Json::object(); qa["server"] = "local"; qa["directory"] = store; Json lst = s.ok("backups.list", qa);
    T_CHECK(lst.get("backups").size() == 2); T_CHECK(lst.get("resumable").size() == 0);
    for (size_t i = 0; i < lst.get("backups").size(); ++i) {
        bool enc = lst.get("backups").at(i).get("encrypted").boolean();
        Json va = Json::object(); va["server"] = "local"; va["directory"] = store; va["archive"] = lst.get("backups").at(i).get("archive").str(); if (enc) va["password"] = "pw";
        Json vr = s.ok("backups.verify", va); Str vs = s.waitJob(vr.get("job").str()); T_CHECK(vs == "Success" || vs == "Gone");
    }
    return true;
}

// Names: long VM names, Unicode, spaces, dots, characters Windows does not allow in a file name, through the whole chain.
VM2B_TEST("service", odd_names_backup_and_restore)
{
    Svc s(c_, "svcnm"); if (!s.up) return false;
    Str root = s.tmp.sub("vms");
    Str longName; for (int i = 0; i < 10; ++i) longName += "\xd0\x92\xd0\xb8\xd1\x80\xd1\x82 \xd0\xbc\xd0\xb0\xd1\x88\xd0\xb8\xd0\xbd\xd0\xb0 "; longName += "end";            // 223 bytes: over the 200-byte cap of generated names, under the 255-byte limit of the file system (Wine: bytes)
    const char* names[] = { "VM with spaces  and  double", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x92\xbe", "dots.in.name.v1.2", "UPPER lower MiXeD", "a", "x y.z (copy)" };
    Vec<Str> vms; for (const char* n : names) vms.push_back(n); vms.push_back(longName);
    Str srv = s.addDirServer(root);
    Bytes data = Random(c_, 30000);
    for (const Str& vm : vms) WriteBytes(JoinPath(JoinPath(root, vm), "disk with space.vmdk"), data);
    Str store = s.tmp.sub("store dir with spaces");
    for (size_t i = 0; i < vms.size(); ++i) {
        Json el = Svc::Element(srv, vms[i].c_str(), JoinPath(store, "{VM}"), "xz", 1, c_.coin() ? "pw" : nullptr);
        Str name = Str::fmt("task \xd0\xb7\xd0\xb0\xd0\xb4\xd0\xb0\xd1\x87\xd0\xb0 %zu ", i) + vms[i];
        Str task = s.saveTask(name.c_str(), el); T_CHECK(!task.empty());
        Json a = Json::object(); a["id"] = task; Json r = s.ok("tasks.run", a); Str fin = s.waitJob(r.get("job").str()); T_CHECK(fin == "Success" || fin == "Gone");
    }
    T_CHECK(s.waitHistory((int)vms.size(), 60000));
    Json hl = s.ok("history.list", [&] { Json a = Json::object(); a["limit"] = 50; return a; }());
    for (size_t i = 0; i < hl.size(); ++i) { Str st = hl.at(i).get("status").str(); if (st != "Success") { Json hr = s.ok("history.get", [&] { Json a = Json::object(); a["id"] = hl.at(i).get("id").str(); return a; }()); c_.note("task %s: %s: %s", hl.at(i).get("name").str().c_str(), st.c_str(), hr.dump().c_str()); T_CHECK(false); } }
    // restore each and compare
    for (size_t i = 0; i < hl.size(); ++i) {
        Json hr = s.ok("history.get", [&] { Json a = Json::object(); a["id"] = hl.at(i).get("id").str(); return a; }());
        T_CHECK(hr.get("results").size() == 1); if (!hr.get("results").size()) continue;
        Json res = hr.get("results").at(0); Str vm = res.get("vm").str();
        Json ra = Json::object(); ra["server"] = "local"; Str dn = TruncateUtf8(vm, kMaxNameBytes); while (dn.ends_with(" ") || dn.ends_with(".")) dn = dn.substr(0, dn.size() - 1); ra["directory"] = JoinPath(store, dn); ra["archive"] = res.get("archive").str(); ra["dest"] = s.tmp.sub(Str::fmt("rest%zu", i).c_str());
        Json el = Json::object(); (void)el;
        bool enc = res.get("archive").str().ends_with(".enc"); if (enc) ra["password"] = "pw";
        Json rj = s.ok("backups.restore", ra); Str rs = s.waitJob(rj.get("job").str()); T_CHECK(rs == "Success" || rs == "Gone");
        Bytes got = ReadAll(JoinPath(JoinPath(ra.get("dest").str(), vm), "disk with space.vmdk"));
        if (!(got.size() == data.size() && !memcmp(got.data(), data.data(), data.size()))) { c_.note("restore of \"%s\" differs", vm.c_str()); T_CHECK(false); }
    }
    return true;
}

} // namespace vm2b
