// VM2Backup — the optional administration web site: an HTTPS listener (HTTP/1.1 parser, keep-alive, thread pool, limits and
// timeouts), the embedded single-page UI, and POST /api/<cmd> mapped to ServiceCore::handle() after authentication, role and CSRF
// checks.  Disabled by default; when enabled it listens only where the administrator chose (default 127.0.0.1).
// See docs/WEB.md.  The Windows program configures it through handleAdmin() (commands "web.*", routed by the service).
//
//   web.status              -> {enabled, listening, address, port, bind, entries:[{entry,addresses:[{address,listening,error}],listening,error}], cert{...}, users, sessions}
//   web.settings.get / web.settings.set {enabled, bind:[...], port, require_2fa_admins, session_minutes, trust_proxy} -> {settings, warnings[]}
//   web.interfaces.list     -> {interfaces:[{name, friendly, description, up, loopback, addresses:[{address,family,link_local}]}]}
//   web.cert.generate {names[]} / web.cert.import {pfx_path, password}
//   web.users.list / save {name, role, password?, twofa} / delete {name} / totp_enroll {name} / totp_confirm {name, code} / totp_disable {name}
//   web.audit.tail {lines}  -> {text}
#pragma once
#include "base.h"

namespace vm2b {

class ServiceCore;

class WebServer {
public:
    WebServer();
    ~WebServer();
    // Loads web.json from dataDir and starts listening when it is enabled.  A listener that cannot start is reported by
    // web.status (not a failure of start()); false only for a damaged web.json.
    bool start(ServiceCore* core, const Str& dataDir, Str& err, bool enabledDefault = true);
    void stop();
    bool handleAdmin(const Str& cmd, const Json& args, Json& result, Str& err);
    // the same commands for an admin of the site (over HTTPS), with the lock-out protections; applyLater = a listener restart is wanted after the answer was sent
    bool handleAdminFromWeb(const Str& actor, const Str& session, const Str& ip, const Str& cmd, const Json& args, Json& result, Str& err, bool& applyLater);

    // ---- hooks for the tests ----
    void setClock(i64 (*clock)());                 // unix seconds (sessions, throttling, TOTP); null = real
    void setRetrySeconds(int s);                   // how often unbound addresses are retried (default 30)
    void setInterfaceProvider(Json (*p)());        // replaces the list of adapters (same shape as web.interfaces.list "interfaces"); null = the real ones
    void setKdfIterationsForTests(u32 n);
    int  port() const;

private:
    WebServer(const WebServer&);
    WebServer& operator=(const WebServer&);
    struct Impl;
    Impl* m;
};

} // namespace vm2b
