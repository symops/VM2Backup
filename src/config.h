// VM2Backup — the configuration the service keeps in its data folder (%ProgramData%\VM2Backup): servers (the datacenter: VM
// hosts and backup targets), scheduled tasks, settings, the task history and the catalog of backups. Passwords are stored encrypted
// with DPAPI (machine scope: ANY account of this computer can decrypt them, so the data folder itself is closed to everyone but SYSTEM and the
// administrators (applog.cpp, SecureDataFolders); a copy of the file taken to another computer cannot be decrypted; changing the account the service
// runs as loses nothing) and are never handed out to a client.
//
// server  {id,name,type, ...}   type: directory | esxi | vcenter  (VM sources)      local | ftp | ftpes | ftps | scp | smb  (targets; "sftp" of old configs is kept but unusable)
//   directory: path                                       esxi/vcenter: host,port,user,password,verify,fingerprint,tls_min,transport
//   smb:       path (UNC \\host\share[\dir]), user (DOMAIN\u / u@domain / empty = the service account), password (DPAPI)   [a "directory" with a UNC path takes user, password too]
//   local:     (the built-in "Local Computer", id "local", folder = settings.default_local_path)
//   ftp*:      host,port,user,password,directory,verify,fingerprint,tls_min,passive          scp: host,port,user,password,key_file,key_passphrase,directory,host_key
// task    {id,name,enabled,schedule,windows,elements:[element]}      (schedule / windows: see schedule.h)
// element {type:"backup",enabled,source:{server,vms:[...]},target:{server,directory},compress:{codec,level},encrypt:{enabled,password},
//          keep,verify,allow_live,segment_mib,speed_mbit,on_error_next}
#pragma once
#include "base.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

extern const char* const kLocalServerId;                  // "local"
extern const char* const kMask;                           // what a client sends for "keep the stored password"

class Config {
public:
    Config();
    ~Config();
    bool open(const Str& dataDir, Str& err);              // creates the folder and the files that are missing
    const Str& dataDir() const { return m_dir; }

    // ---- settings ----
    Json settings() const;
    bool setSettings(const Json& patch, Str& err);
    bool schedulerEnabled() const;
    void setSchedulerEnabled(bool on);

    // ---- servers ----
    Json servers() const;                                  // public view: no secrets, "has_password" flags
    bool server(const Str& id, bool reveal, Json& out) const;
    bool saveServer(const Json& in, Json& saved, Str& err);
    bool deleteServer(const Str& id, Str& err);
    // ---- the lock-out guard: servers whose credentials were refused ----
    // A server that answered "wrong password / locked / disabled / expired" is marked in state.json (section "_auth_blocks": {server_id: {code,text,time,creds_fp}}, never in
    // config.json: it is a state, not a setting, and it survives a restart of the service). The mark is valid only while creds_fp equals the fingerprint of the
    // CURRENT record (a hash of type / address / port / user / path / key file and of the protected secrets): any edit of the login data removes it by itself.
    // The fingerprint cannot be turned back into a password (it covers the DPAPI blob, which is salted). "local" has no credentials: never blocked.
    Str credsFingerprint(const Str& serverId) const;         // "" = the server does not exist / has no credentials
    bool authBlock(const Str& serverId, Json& out) const;    // {code,text,time}: a valid mark of this server
    void setAuthBlock(const Str& serverId, const Str& fp, int code, const Str& text, i64 when);   // ignored when the record changed meanwhile (fp differs)
    bool clearAuthBlock(const Str& serverId);                // true when there was a mark

    // ---- tasks ----
    Json tasks() const;                                    // public view
    bool task(const Str& id, bool reveal, Json& out) const;
    bool findTask(const Str& idOrName, Str& id) const;
    bool saveTask(const Json& in, Json& saved, Str& err);
    bool deleteTask(const Str& id, Str& err);
    bool copyTask(const Str& ref, const Str& name, Json& saved, Str& err);       // a new task with the same settings, new ids, the stored (protected) passwords copied
    // The stored state of a task between runs: {last_start, last_status, last_end, paused_archive...}
    Json taskState(const Str& id) const;
    void setTaskState(const Str& id, const Json& st, bool persist = true);   // persist = false: kept in memory until the next persisting call / flushState() (the scheduler's per-second bookkeeping)
    void flushState();

    // ---- history & catalog ----
    void addHistory(const Json& rec);
    Json history(int limit) const;                         // newest first, without the per-VM details
    bool historyRecord(const Str& id, Json& out) const;
    void clearHistory();
    Str newId() const;

    // ---- secrets ----
    static Str Protect(const Str& plain);                  // "dpapi:<base64>"
    static bool Unprotect(const Str& stored, Str& plain);  // a value without the prefix is returned as it is (hand-edited files)

    static bool ValidateElement(Json& el, const Config& cfg, Str& err);

private:
    bool saveCfg(Str& err);
    bool saveHist();
    Json publicServer(const Json& s) const;
    Str fingerprintOf(const Json& rawServer) const;
    bool authBlockLocked(const Str& serverId, Json& out) const;
    void persistState();
    Json publicServerWithBlock(const Json& s) const;
    Json publicTask(const Json& t) const;
    void lock() const { EnterCriticalSection(&m_cs); }
    void unlock() const { LeaveCriticalSection(&m_cs); }
    mutable CRITICAL_SECTION m_cs;
    Str m_dir; Json m_cfg, m_hist, m_state;
};

} // namespace vm2b
