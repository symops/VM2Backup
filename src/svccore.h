// VM2Backup — the heart of the Windows service: runs the jobs (backups, checks, restores), the scheduler with its time windows, and
// answers the commands of the program over the pipe. VM2BackupService.exe is a thin shell around it; the autotests run it in-process.
//
// Commands (cmd -> args -> result), all over ipc.h:
//   state                                   -> {version, data_dir, scheduler, now, jobs:[...], windows_allowed}
//   scheduler.set {enabled}                 settings.get / settings.set {patch}
//   servers.list / servers.save {server} / servers.delete {id} / servers.test {server|id} / servers.vms {id}
//   tasks.list / tasks.save {task} / tasks.delete {id} / tasks.run {id|name} / tasks.next {id}
//   backup.run {element, name?}             a backup that is not a saved task ("Virtual Machine Backup" dialog)
//   jobs.list / jobs.abort {id}
//   history.list {limit} / history.get {id} / history.clear
//   backups.list {server, directory?}       the backups found in a target (Backup Explorer); also the interrupted (resumable) ones
//   backups.verify / backups.restore / backups.delete {server, directory, archive, password?, dest?}
//   resumable.discard {server, directory, archive}
//   net.drives {}                           -> {drives:[{letter:"Z:", unc:"\\\\server\\share", user:"DOMAIN\\name or empty", owner:"Windows account whose drive it is", source:"profile|service"}]}
//        ADMIN ONLY. Drives mapped by the logged-on users (HKEY_USERS\<SID>\Network\<letter>, loaded profiles only) + the service's own remote drives; entries without a UNC path are
//        skipped; no passwords (the registry has none).
//   net.browse {path, user?, password?}     -> {path, kind:"root|server|share|folder", parent, entries:[{name, path, kind:"server|share|folder", comment?}], truncated, hint?}
//        ADMIN ONLY. path "" = the network neighbourhood (may be empty: `hint` says a server name can be typed), "\\\\server" = its disk shares, "\\\\server\\share[\\sub]" = its sub-folders
//        (hidden / system folders not shown, at most 5000 entries, `truncated`). user/password are used only for a temporary connection (cancelled afterwards), never stored,
//        logged, echoed or put into an error. Errors are one English line ending with the Windows code, e.g. "(1326)" wrong password (permanent), "(1460)" timeout after
//        ~15 s or too many lookups pending (temporary). Details: docs/WEB.md, netbrowse.h.
#pragma once
#include "config.h"
#include "engine.h"
#include "ipc.h"
#include "winutil.h"
#include "webserver.h"

namespace vm2b {

struct ServiceOptions {
    Str dataDir;                                   // empty = %ProgramData%\VM2Backup
    Str pipeName;                                  // empty = kDefaultPipeName
    const char* pipeSddl;                          // null = Administrators + SYSTEM
    int tickMs;                                    // the scheduler's pace
    i64 (*clock)();                                // unix time; null = the real one (tests replace it)
    void (*onShutdown)();                          // called when a client sends service.shutdown (the shell stops the service)
    int webDefault;                                // the web site on a first start: -1 = on (127.0.0.1:8443), unless the environment has VM2B_WEB_DEFAULT=0 (tests); 0 = off; 1 = on
    ServiceOptions() : pipeSddl(nullptr), tickMs(1000), clock(nullptr), onShutdown(nullptr), webDefault(-1) {}
};

class Job;

class ServiceCore : public IpcHandler {
public:
    ServiceCore();
    ~ServiceCore();
    bool start(const ServiceOptions& o, Str& err);
    void stop();                                   // running jobs are stopped and kept for resuming
    bool handle(const Str& cmd, const Json& args, Json& result, Str& err) override;
    Config& config() { return m_cfg; }
    i64 now() const { return m_opt.clock ? m_opt.clock() : (i64)UnixNow(); }
    int runningJobs() const;
    static Str DefaultDataDir();

private:
    friend class Job;
    static DWORD WINAPI SchedulerThread(LPVOID p);
    static DWORD WINAPI JobThread(LPVOID p);
    void schedulerLoop();
    void tick();
    void runJob(Job* j);
    Str startTaskJob(const Str& taskId, const char* trigger, Str& err);
    Str startElementJob(const Json& element, const Str& name, const char* trigger, Str& err);
    Str startOpJob(const char* kind, const Json& args, Str& err);
    void finishJob(Job* j, Json& record);
    Job* findJob(const Str& id);
    void reapJobs();
    double effectiveMbit(Job* j, i64 nowUnix);
    bool makeTarget(const Str& serverId, const Str& directory, const Str& vm, double when, Target*& out, Str& err);
    Str expandDirectory(const Json& server, const Str& tmpl, const Str& vm, double when);
    void backupElement(Job* j, const Json& el, const Str& taskName, Json& results, bool& stopTask);
    bool listTarget(const Json& args, Json& result, Str& err);
    void remember(const Str& serverId, const Str& directory);
    // the lock-out guard (see config.h): a server whose credentials were refused is not contacted again by anything that runs on its own
    Str authSkipMessage(const Str& serverId, i64 when, const Str& text);
    bool authGate(Job* j, const Str& serverId, Str& message);                              // true = do not touch the server (message set)
    void authFailedNow(Job* j, const Str& serverId, const Str& fp, int code, const Str& text);
    void authOk(const Str& serverId);

    Config m_cfg; ServiceOptions m_opt; IpcServer m_ipc; WebServer m_web; HANDLE m_sched; volatile LONG m_stop;
    mutable CRITICAL_SECTION m_cs; Vec<Job*> m_jobs; HANDLE m_slots; int m_slotCount;
    Vec<Str> m_claims;                             // "server|folder|vm" of the backups running now: two jobs never write the same VM into the same folder at once
    Json m_catalog;                                // [{server, directory}] places where backups were made
    i64 m_lastTick, m_statePersisted; bool m_started;      // m_statePersisted: when the scheduler last wrote state.json (last_check is kept in memory in between)
};

} // namespace vm2b
