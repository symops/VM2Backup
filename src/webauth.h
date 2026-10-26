// VM2Backup — authentication and authorization of the administration web site: users (PBKDF2 passwords, optional TOTP + recovery
// codes), sessions, login rate limiting, the role -> command table, the audit log and web.json (settings + users + certificate).
// Users are created only through the local administration channel (WebServer::handleAdmin); the web has no registration.
#pragma once
#include "base.h"
#include "webtls.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

struct WebSession {
    Str id, user, role, csrf, ip;
    i64 created, last;
};

class WebAuth {
public:
    WebAuth();
    ~WebAuth();
    bool open(const Str& dataDir, Str& err, bool enabledDefault = true);                 // loads (or creates) web.json
    void setClock(i64 (*clock)()) { m_clock = clock; }       // unix seconds; null = the real clock
    i64 now() const;

    // ---- settings {enabled, bind:[entries], port, require_2fa_admins, session_minutes, trust_proxy} ----
    // a bind entry: an address ("192.168.1.10", "::1", "fe80::1%12"), "adapter:<AdapterName GUID>", "localhost" (127.0.0.1 + ::1), "all" (0.0.0.0 + ::)
    static bool BindEntryOk(const Str& e);
    Json settings() const;
    bool setSettings(const Json& patch, Str& err);

    // ---- certificate (identity kept in web.json) ----
    bool identity(WebTlsIdentity& id, bool* selfSigned = nullptr) const;   // false: none yet
    bool setIdentity(const WebTlsIdentity& id, bool selfSigned, Str& err);

    // ---- users (local administration channel) ----
    Json usersList() const;
    bool usersSave(const Json& a, Str& err);                 // {name, role, password?, twofa}
    bool usersDelete(const Str& name, Str& err);
    bool totpEnroll(const Str& name, Json& out, Str& err);   // -> {secret, uri}
    bool totpConfirm(const Str& name, const Str& code, Json& out, Str& err, const Str& keepSession = Str());   // -> {recovery_codes:[...]}; keepSession = the caller's own session stays alive
    bool totpDisable(const Str& name, Str& err);
    int userCount() const;
    int adminCount() const;                                  // users with the admin role
    bool userInfo(const Str& name, Str& role, bool& totpEnabled) const;
    // who is acting for the audit trail of the user commands ("" = the local administration channel); set by WebServer under its lock
    void setActor(const Str& user, const Str& ip) { m_actor = user; m_actorIp = ip; }
    // /reset-web: settings back to the defaults (on, 127.0.0.1:8443, no 2FA requirement), the user admin / admin (admin role, 2FA cleared), the other users are kept
    bool resetDefaults(Str& report, Str& err);
    static bool IsLoopbackAddress(const Str& ip);            // 127.0.0.0/8, ::1 (also ::ffff:127.x, a %scope is ignored)

    // ---- login / sessions ----
    // status: 200 ok (a session was made) | 202 a second factor is needed | 401 refused | 429 delayed
    struct LoginResult { int status; Str message; Str sessionId; Str csrf; Str user, role; int retryAfter; };
    LoginResult login(const Str& ip, const Str& user, const Str& password, const Str& code, const Str& recovery);
    bool session(const Str& id, WebSession& out);            // checks the timeouts and the user; touches the idle clock
    void logout(const Str& id);
    int sessionCount();
    bool changePassword(const Str& user, const Str& current, const Str& next, const Str& ip, Str& err);   // the own password, from the web
    int sessionSeconds() const;                              // idle timeout

    // ---- policy ----
    static bool RoleAllows(const Str& role, const Str& cmd);
    static bool IsReadOnlyCmd(const Str& cmd);
    static bool PasswordOk(const Str& user, const Str& pw, Str& why);
    static Str Totp(const Bytes& secret, u64 unixTime, int digits = 6);     // RFC 6238 / 4226, HMAC-SHA1
    static Str Base32(const Bytes& b);
    static bool Base32Decode(const Str& s, Bytes& out);
    void setKdfIterationsForTests(u32 n) { m_kdfIter = n; }

    // ---- audit log (audit.log in the data folder; one line per event, no secrets) ----
    void audit(const char* event, const Str& user, const Str& ip, const Str& detail);
    Str auditTail(int lines);

private:
    bool save(Str& err);                                     // caller holds the lock
    int findUser(const Str& lname) const;
    bool verifyPassword(const Json& ph, const Str& pw);
    Json hashPassword(const Str& pw);
    bool checkTotp(Json& u, const Str& code);
    bool useRecovery(Json& u, const Str& code);
    bool isTwofaRequired(const Json& u) const;
    struct Fail { int count; i64 last; i64 lockedUntil; Str key; };
    int  throttleLeft(const Str& key);                       // seconds the caller must still wait (0 = go)
    void noteFail(const Str& key);
    void clearFail(const Str& key);

    Str m_actor, m_actorIp;
    Str actorName() const { return m_actor.empty() ? Str("local-admin") : m_actor; }
    Str actorIp() const { return m_actor.empty() ? Str("pipe") : m_actorIp; }
    mutable CRITICAL_SECTION m_cs;
    Str m_dir, m_file, m_auditFile;
    Json m_doc;                                              // {settings, users, cert}
    Vec<WebSession> m_sessions;
    Vec<Fail> m_fails;
    i64 (*m_clock)();
    u32 m_kdfIter;
    Json m_dummy;                                            // a password hash to burn the same time for unknown users
};

} // namespace vm2b
