// Autotests of the administration web site: pure parts (TOTP vectors, policy, roles, throttling, sessions) in-process, the HTTPS server
// through our own TLS client, and a host mode that Python clients drive over real HTTPS (tools/run-web-tests.sh).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "autotest.h"
#include "webserver.h"
#include "webauth.h"
#include "webtls.h"
#include "svccore.h"
#include "tls.h"
#include "crypto.h"
#include "winutil.h"

namespace vm2b {

namespace {

i64 g_now = 1000000;
i64 FakeNow() { return g_now; }

struct Tmp {
    Str path; bool keep;
    Tmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        RemoveTree(path); Str e; MakeDirs(path, e);
    }
    ~Tmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

Bytes B(const char* s) { Bytes b; b.resize(strlen(s)); memcpy(b.data(), s, strlen(s)); return b; }

// one HTTP exchange over our TLS client (a fresh connection each time)
struct HttpAns { int status; Str head, body; bool ok; Str err; Str tls; };
HttpAns Http(const char* host, int port, const char* method, const Str& path, const Str& body, const Str& extraHeaders, const Str& pin)
{
    HttpAns a; a.status = 0; a.ok = false;
    NetConn c; TlsOptions o; o.fingerprint = pin; o.verify = !pin.empty(); o.timeoutMs = 8000;
    if (!c.connect(host, port, true, o)) { a.err = c.err; return a; }
    a.tls = c.tlsVersion();
    Str hostHdr = Str(host).find(':') >= 0 ? Str("[") + host + "]" : Str(host);
    Str req = Str(method) + " " + path + " HTTP/1.1\r\nHost: " + Str::fmt("%s:%d", hostHdr.c_str(), port) + "\r\nConnection: close\r\n" + extraHeaders;
    if (!body.empty() || Str(method) == "POST") req += Str::fmt("Content-Length: %zu\r\n", body.size());
    req += "\r\n"; req += body;
    if (!c.send(req.data(), req.size())) { a.err = c.err; return a; }
    Str all; char buf[8192];
    for (;;) { i64 k = c.recv(buf, sizeof(buf)); if (k <= 0) break; all.append(buf, (size_t)k); }
    int e = all.find("\r\n\r\n"); if (e < 0) { a.err = "no head"; return a; }
    a.head = all.substr(0, (size_t)e); a.body = all.substr((size_t)e + 4);
    a.status = atoi(a.head.c_str() + 9); a.ok = true;
    return a;
}

CRITICAL_SECTION g_ifCs; bool g_ifInit = false; Json g_ifs;
Json IfProvider() { EnterCriticalSection(&g_ifCs); Json j = g_ifs; LeaveCriticalSection(&g_ifCs); return j; }
void SetIfs(WebServer& w, const Json& j) { if (!g_ifInit) { InitializeCriticalSection(&g_ifCs); g_ifInit = true; } EnterCriticalSection(&g_ifCs); g_ifs = j; LeaveCriticalSection(&g_ifCs); w.setInterfaceProvider(IfProvider); }

Json FakeAdapter(const char* guid, bool up, const char* a1, const char* a2 = nullptr)
{
    Json o = Json::object(); o["name"] = guid; o["friendly"] = "Test"; o["description"] = "Test adapter"; o["up"] = up; o["loopback"] = false;
    Json ad = Json::array(); const char* v[2] = { a1, a2 };
    for (const char* x : v) if (x) { Json e = Json::object(); e["address"] = x; e["family"] = Str(x).find(':') >= 0 ? "ipv6" : "ipv4"; e["link_local"] = false; ad.push(e); }
    o["addresses"] = ad; return o;
}

i64 g_clockOffset = 0;
i64 OffsetNow() { return (i64)UnixNow() + g_clockOffset; }

} // namespace

VM2B_TEST("web", totp_rfc6238_vectors)
{
    Bytes sec = B("12345678901234567890");
    struct V { u64 t; const char* code8; } v[] = { { 59, "94287082" }, { 1111111109ull, "07081804" }, { 1111111111ull, "14050471" }, { 1234567890ull, "89005924" }, { 2000000000ull, "69279037" }, { 20000000000ull, "65353130" } };
    for (const V& x : v) {
        T_CHECK(WebAuth::Totp(sec, x.t, 8) == x.code8);
        T_CHECK(WebAuth::Totp(sec, x.t, 6) == Str(x.code8 + 2));
    }
    T_CHECK(WebAuth::Base32(B("foobar")) == "MZXW6YTBOI");
    T_CHECK(WebAuth::Base32(B("fooba")) == "MZXW6YTB");
    Bytes d; T_CHECK(WebAuth::Base32Decode("MZXW6YTBOI======", d) && d.size() == 6 && memcmp(d.data(), "foobar", 6) == 0);
    T_CHECK(!WebAuth::Base32Decode("MZ1W", d));
    for (int i = 0; i < 30; ++i) { Bytes r; r.resize((size_t)c_.range(0, 40)); c_.fill(r.data(), r.size()); Bytes back; T_CHECK(WebAuth::Base32Decode(WebAuth::Base32(r), back) && back.size() == r.size() && (r.empty() || memcmp(back.data(), r.data(), r.size()) == 0)); }
    return true;
}

VM2B_TEST("web", policy_roles_bind_entries)
{
    Str why;
    T_CHECK(!WebAuth::PasswordOk("alice", "short", why)); T_CHECK(!WebAuth::PasswordOk("alice", "alice12345", why)); T_CHECK(!WebAuth::PasswordOk("alice", "xxxxxxxxxxxx", why));
    T_CHECK(!WebAuth::PasswordOk("bobby", "ALICEbobbyBOBx1", why)); T_CHECK(WebAuth::PasswordOk("alice", "correct horse battery", why));
    T_CHECK(WebAuth::PasswordOk("alice", "ab1234567890", why));
    const char* viewer[] = { "state", "servers.list", "tasks.list", "tasks.next", "jobs.list", "history.list", "history.get", "backups.list" };
    const char* opx[] = { "tasks.run", "jobs.abort", "backup.run", "backups.verify" };
    const char* adm[] = { "tasks.save", "tasks.delete", "servers.save", "servers.delete", "servers.test", "servers.vms", "settings.get", "settings.set", "scheduler.set", "history.clear", "backups.restore", "backups.delete", "resumable.discard" };
    for (const char* c : viewer) { T_CHECK(WebAuth::RoleAllows("viewer", c)); T_CHECK(WebAuth::RoleAllows("operator", c)); T_CHECK(WebAuth::RoleAllows("admin", c)); }
    for (const char* c : opx) { T_CHECK(!WebAuth::RoleAllows("viewer", c)); T_CHECK(WebAuth::RoleAllows("operator", c)); T_CHECK(WebAuth::RoleAllows("admin", c)); }
    for (const char* c : adm) { T_CHECK(!WebAuth::RoleAllows("viewer", c)); T_CHECK(!WebAuth::RoleAllows("operator", c)); T_CHECK(WebAuth::RoleAllows("admin", c)); }
    const char* never[] = { "service.shutdown", "service.stop", "nonsense", "", "web", "web.", "web.bogus" };
    for (const char* c : never) { T_CHECK(!WebAuth::RoleAllows("admin", c)); T_CHECK(!WebAuth::RoleAllows("operator", c)); }
    T_CHECK(!WebAuth::RoleAllows("", "state")); T_CHECK(!WebAuth::RoleAllows("root", "state"));
    const char* good[] = { "localhost", "all", "127.0.0.2", "::1", "192.168.1.10", "fe80::1%12", "adapter:{9A3B-11}", "2001:db8::5" };
    const char* bad[] = { "", "example.com", "adapter:", "adapter:a b", "999.1.1.1", "fe80::1%", "1.2.3.4%5", "all ", "0.0.0.0/0", "adapter:{x}\r\n" };
    for (const char* g : good) T_CHECK(WebAuth::BindEntryOk(g));
    for (const char* b : bad) T_CHECK(!WebAuth::BindEntryOk(b));
    return true;
}

// Every command the service understands (found in svccore.cpp / webserver.cpp at run time) must be classified here, so that adding a command
// without deciding who may call it over the web fails this test.  A=admin+, O=operator+, V=viewer+, F=never over the web.
VM2B_TEST("web", role_matrix_classifies_every_service_command)
{
    struct Cl { const char* cmd; char cls; };
    static const Cl kTable[] = {
        { "state", 'V' }, { "servers.list", 'V' }, { "tasks.list", 'V' }, { "tasks.next", 'V' }, { "jobs.list", 'V' }, { "history.list", 'V' }, { "history.get", 'V' }, { "backups.list", 'V' },
        { "tasks.run", 'O' }, { "jobs.abort", 'O' }, { "backup.run", 'O' }, { "backups.verify", 'O' },
        { "tasks.save", 'A' }, { "tasks.copy", 'A' }, { "tasks.delete", 'A' }, { "servers.save", 'A' }, { "servers.delete", 'A' }, { "servers.test", 'A' }, { "servers.vms", 'A' },
        { "settings.get", 'A' }, { "settings.set", 'A' }, { "scheduler.set", 'A' }, { "history.clear", 'A' }, { "backups.restore", 'A' }, { "backups.delete", 'A' }, { "resumable.discard", 'A' },
        // restore to ESXi / vCenter and the network drive / browse helpers write or connect as SYSTEM: admin only
        { "backups.restore_esxi", 'A' }, { "esxi.restore_options", 'A' }, { "net.drives", 'A' }, { "net.browse", 'A' }, { "fs.browse", 'A' }, { "service.shutdown", 'F' },
        // the site administers itself: the web.* commands are admin only (with lock-out protections in WebServer::handleAdmin); service.shutdown never
        { "web.status", 'A' }, { "web.settings.get", 'A' }, { "web.settings.set", 'A' }, { "web.interfaces.list", 'A' }, { "web.cert.generate", 'A' }, { "web.cert.import", 'A' },
        { "web.users.list", 'A' }, { "web.users.save", 'A' }, { "web.users.delete", 'A' }, { "web.users.totp_enroll", 'A' }, { "web.users.totp_confirm", 'A' },
        { "web.users.totp_disable", 'A' }, { "web.audit.tail", 'A' },
    };
    const size_t N = sizeof(kTable) / sizeof(kTable[0]);
    for (size_t i = 0; i < N; ++i) {
        const char* cmd = kTable[i].cmd; char k = kTable[i].cls;
        T_CHECK(WebAuth::RoleAllows("admin", cmd) == (k == 'A' || k == 'O' || k == 'V'));
        T_CHECK(WebAuth::RoleAllows("operator", cmd) == (k == 'O' || k == 'V'));
        T_CHECK(WebAuth::RoleAllows("viewer", cmd) == (k == 'V'));
        T_CHECK(!WebAuth::RoleAllows("", cmd)); T_CHECK(!WebAuth::RoleAllows("guest", cmd)); T_CHECK(!WebAuth::RoleAllows("ADMIN", cmd));
        T_CHECK(WebAuth::IsReadOnlyCmd(cmd) ? (k == 'V' || Str(cmd) == "settings.get" || (Str(cmd).starts_with("web.") && k == 'A')) : true);   // read-only commands never need a CSRF token; nothing mutating may be listed
    }
    wchar_t env[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"VM2B_SRC_DIR", env, MAX_PATH);
    if (!n || n >= MAX_PATH) { c_.skip("set VM2B_SRC_DIR to src/ (tools/run-web-tests.sh does) to cross-check the table with the sources"); return true; }
    Str dir = FromWide(env); int seen = 0;
    const char* files[] = { "svccore.cpp", "webserver.cpp" };
    for (const char* f : files) {
        Bytes b; Str err; T_CHECK(ReadWholeFile(JoinPath(dir, f).c_str(), b, err)); if (b.empty()) return false;
        Str src((const char*)b.data(), b.size());
        for (size_t pos = 0; ; ) {
            int at = src.find("cmd == \"", pos); if (at < 0) break;
            size_t st = (size_t)at + 8; size_t en = st; while (en < src.size() && src[en] != '"') ++en;
            Str cmd = src.substr(st, en - st); pos = en; ++seen;
            bool known = false; for (size_t i = 0; i < N; ++i) if (cmd == kTable[i].cmd) known = true;
            if (!known) c_.note("UNCLASSIFIED service command: %s", cmd.c_str());
            T_CHECK(known);
        }
    }
    T_CHECK(seen > 30);
    // and nothing stale: every table entry still exists in one of the sources
    Bytes a1, a2; Str err; T_CHECK(ReadWholeFile(JoinPath(dir, "svccore.cpp").c_str(), a1, err)); T_CHECK(ReadWholeFile(JoinPath(dir, "webserver.cpp").c_str(), a2, err));
    Str all((const char*)a1.data(), a1.size()); all += Str((const char*)a2.data(), a2.size());
    for (size_t i = 0; i < N; ++i) { bool ok = all.find((Str("\"") + kTable[i].cmd + "\"").c_str()) >= 0; if (!ok) c_.note("stale table entry: %s", kTable[i].cmd); T_CHECK(ok); }
    return true;
}

VM2B_TEST("web", auth_users_login_throttle_sessions)
{
    Tmp tmp(c_, "webauth"); WebAuth a; Str err;
    g_now = 5000000; a.setClock(FakeNow); T_CHECK(a.open(tmp.sub("d"), err, false)); a.setKdfIterationsForTests(1000);
    WebAuth::LoginResult r = a.login("1.1.1.1", "root", "whatever-pass", "", ""); T_CHECK(r.status == 401 && r.message.find("No users") >= 0);
    Json s = Json::object(); s["name"] = "alice"; s["role"] = "operator"; s["password"] = "correct horse battery"; s["twofa"] = "optional";
    T_CHECK(a.usersSave(s, err));
    Json bad = s; bad["name"] = "bobby"; bad["password"] = "short"; T_CHECK(!a.usersSave(bad, err)); bad["password"] = "bobby-bobby-12"; T_CHECK(!a.usersSave(bad, err));
    Json nopw = Json::object(); nopw["name"] = "carol"; nopw["role"] = "viewer"; T_CHECK(!a.usersSave(nopw, err));
    Json ul = a.usersList(); T_CHECK(ul.size() == 1 && ul.dump().find("hash") < 0 && ul.dump().find("salt") < 0);
    r = a.login("1.1.1.1", "nobody", "correct horse battery", "", ""); T_CHECK(r.status == 401); Str m1 = r.message;
    g_now += 100; r = a.login("1.1.1.1", "alice", "wrong password!", "", ""); T_CHECK(r.status == 401 && r.message == m1);
    r = a.login("1.1.1.1", "alice", "correct horse battery", "", ""); T_CHECK(r.status == 429 && r.retryAfter >= 1);
    g_now += 5; r = a.login("1.1.1.1", "alice", "correct horse battery", "", ""); T_CHECK(r.status == 200 && r.sessionId.size() == 64 && r.csrf.size() == 64);
    WebSession ss; T_CHECK(a.session(r.sessionId, ss) && ss.user == "alice" && ss.role == "operator");
    WebAuth::LoginResult r2 = a.login("9.9.9.9", "ALICE", "correct horse battery", "", ""); T_CHECK(r2.status == 200);
    g_now += 19 * 60; T_CHECK(a.session(r.sessionId, ss));
    g_now += 19 * 60; T_CHECK(a.session(r.sessionId, ss));
    g_now += 21 * 60; T_CHECK(!a.session(r.sessionId, ss));
    for (int i = 0; i < 40; ++i) { g_now += 15 * 60; if (!a.session(r2.sessionId, ss)) break; }
    T_CHECK(!a.session(r2.sessionId, ss));
    r2 = a.login("9.9.9.9", "alice", "correct horse battery", "", ""); T_CHECK(r2.status == 200);
    i64 t0 = g_now; bool alive = true; while (g_now - t0 < 12 * 3600 + 1000 && alive) { g_now += 600; alive = a.session(r2.sessionId, ss); }
    T_CHECK(!alive && g_now - t0 > 11 * 3600);
    { int count = 0, guard = 0; while (count < 8 && guard++ < 100) { r = a.login("2.2.2.2", "alice", "nope nope nope", "", ""); if (r.status == 429) g_now += r.retryAfter; else ++count; } T_CHECK(count == 8); }
    r = a.login("2.2.2.2", "alice", "nope nope nope", "", ""); T_CHECK(r.status == 429 && r.retryAfter > 800);
    r = a.login("3.3.3.3", "alice", "correct horse battery", "", ""); T_CHECK(r.status == 429);
    g_now += 901; r = a.login("3.3.3.3", "alice", "correct horse battery", "", ""); T_CHECK(r.status == 200);
    r = a.login("4.4.4.4", "ghost", "xxxxxxxxxxxxx1", "", ""); T_CHECK(r.status == 401); r = a.login("4.4.4.4", "ghost", "xxxxxxxxxxxxx1", "", ""); T_CHECK(r.status == 429);
    g_now += 3600; r = a.login("5.5.5.5", "alice", "correct horse battery", "", ""); T_CHECK(r.status == 200);
    T_CHECK(!a.changePassword("alice", "bad current!", "new password 123", "5.5.5.5", err));
    g_now += 100; T_CHECK(a.changePassword("alice", "correct horse battery", "new password 123", "5.5.5.5", err)); T_CHECK(!a.session(r.sessionId, ss));
    g_now += 100; r = a.login("5.5.5.5", "alice", "new password 123", "", ""); T_CHECK(r.status == 200);
    Json up = Json::object(); up["name"] = "alice"; up["role"] = "viewer"; T_CHECK(a.usersSave(up, err)); T_CHECK(!a.session(r.sessionId, ss));
    g_now += 100; r = a.login("5.5.5.5", "alice", "new password 123", "", ""); T_CHECK(r.status == 200 && r.role == "viewer");
    T_CHECK(a.usersDelete("alice", err)); T_CHECK(!a.session(r.sessionId, ss));
    WebAuth b; T_CHECK(b.open(tmp.sub("d2"), err, false)); Json u2 = Json::object(); u2["name"] = "dave"; u2["role"] = "admin"; u2["password"] = "Zebra-Quartz-Maple-77";
    T_CHECK(b.usersSave(u2, err));
    Bytes f; T_CHECK(ReadWholeFile(JoinPath(tmp.sub("d2"), "web.json").c_str(), f, err)); Json doc; T_CHECK(Json::parse((const char*)f.data(), f.size(), doc, err));
    T_CHECK(doc.get("users").at(0).get("pw").get("iter").integer() >= 300000); T_CHECK(Str((const char*)f.data(), f.size()).find("Zebra") < 0);
    return true;
}

VM2B_TEST("web", auth_totp_recovery_2fa_rules)
{
    Tmp tmp(c_, "webtotp"); WebAuth a; Str err;
    g_now = 7000000; a.setClock(FakeNow); T_CHECK(a.open(tmp.sub("d"), err, false)); a.setKdfIterationsForTests(1000);
    Json s = Json::object(); s["name"] = "root"; s["role"] = "admin"; s["password"] = "Zebra-Quartz-Maple-77"; s["twofa"] = "optional";
    T_CHECK(a.usersSave(s, err));
    Json st = Json::object(); st["require_2fa_admins"] = true; T_CHECK(a.setSettings(st, err));
    WebAuth::LoginResult r = a.login("1.1.1.1", "root", "Zebra-Quartz-Maple-77", "", ""); T_CHECK(r.status == 401 && r.message.find("Two-factor") >= 0);
    st["require_2fa_admins"] = false; T_CHECK(a.setSettings(st, err));
    r = a.login("1.1.1.1", "root", "Zebra-Quartz-Maple-77", "", ""); T_CHECK(r.status == 200);
    Json en; T_CHECK(a.totpEnroll("root", en, err)); Str b32 = en.get("secret").str(); T_CHECK(b32.size() == 32); T_CHECK(en.get("uri").str().starts_with("otpauth://totp/VM2Backup:root?secret="));
    Bytes sec; T_CHECK(WebAuth::Base32Decode(b32, sec) && sec.size() == 20);
    Bytes f; T_CHECK(ReadWholeFile(JoinPath(tmp.sub("d"), "web.json").c_str(), f, err)); T_CHECK(Str((const char*)f.data(), f.size()).find(b32.c_str()) < 0);
    Json cf; T_CHECK(!a.totpConfirm("root", "000000", cf, err));
    Str code = WebAuth::Totp(sec, (u64)g_now); T_CHECK(a.totpConfirm("root", code, cf, err)); T_CHECK(cf.get("recovery_codes").size() == 10);
    Str rc0 = cf.get("recovery_codes").at(0).str(), rc1 = cf.get("recovery_codes").at(1).str(); T_CHECK(rc0.size() == 11 && rc0[5] == '-');
    T_CHECK(!a.totpEnroll("root", en, err));
    g_now += 100; r = a.login("1.1.1.1", "root", "Zebra-Quartz-Maple-77", "", ""); T_CHECK(r.status == 202);
    g_now += 31; code = WebAuth::Totp(sec, (u64)g_now);
    r = a.login("1.1.1.1", "root", "Zebra-Quartz-Maple-77", code, ""); T_CHECK(r.status == 200);
    r = a.login("1.1.1.1", "root", "Zebra-Quartz-Maple-77", code, ""); T_CHECK(r.status == 401);
    g_now += 10;
    g_now += 60; r = a.login("1.1.1.2", "root", "Zebra-Quartz-Maple-77", WebAuth::Totp(sec, (u64)g_now - 30), ""); T_CHECK(r.status == 200);
    g_now += 60; r = a.login("1.1.1.3", "root", "Zebra-Quartz-Maple-77", WebAuth::Totp(sec, (u64)g_now + 30), ""); T_CHECK(r.status == 200);
    g_now += 60; r = a.login("1.1.1.4", "root", "Zebra-Quartz-Maple-77", WebAuth::Totp(sec, (u64)g_now - 90), ""); T_CHECK(r.status == 401);
    g_now += 1000; r = a.login("1.1.1.5", "root", "Zebra-Quartz-Maple-77", WebAuth::Totp(sec, (u64)g_now + 90), ""); T_CHECK(r.status == 401);
    g_now += 1000; r = a.login("1.1.1.6", "root", "Zebra-Quartz-Maple-77", "", rc0); T_CHECK(r.status == 200);
    g_now += 1000; r = a.login("1.1.1.6", "root", "Zebra-Quartz-Maple-77", "", rc0); T_CHECK(r.status == 401);
    g_now += 1000; Str lowc = rc1.lower(); r = a.login("1.1.1.7", "root", "Zebra-Quartz-Maple-77", "", lowc); T_CHECK(r.status == 200);
    T_CHECK(a.usersList().at(0).get("recovery_left").integer() == 8);
    T_CHECK(a.totpDisable("root", err)); T_CHECK(!a.usersList().at(0).get("totp_enabled").boolean());
    g_now += 1000; r = a.login("1.1.1.8", "root", "Zebra-Quartz-Maple-77", "", ""); T_CHECK(r.status == 200);
    Json v = Json::object(); v["name"] = "view"; v["role"] = "viewer"; v["password"] = "Another-Long-Pass-9"; v["twofa"] = "required"; T_CHECK(a.usersSave(v, err));
    r = a.login("1.1.1.9", "view", "Another-Long-Pass-9", "", ""); T_CHECK(r.status == 401 && r.message.find("Two-factor") >= 0);
    Str log = a.auditTail(500); T_CHECK(log.find("login_ok") >= 0 && log.find("login_failed") >= 0 && log.find("recovery_code_used") >= 0 && log.find("user_saved") >= 0);
    T_CHECK(log.find("Zebra") < 0 && log.find(b32.c_str()) < 0 && log.find(rc0.c_str()) < 0 && log.find("Another") < 0);
    return true;
}

VM2B_TEST("web", default_admin_created_once)
{
    Tmp tmp(c_, "webdef"); Str err; Str d1 = tmp.sub("d1");
    g_now = 7000000;
    {   // a fresh web.json gets exactly the user admin / admin, full admin rights, hashed
        WebAuth a; a.setClock(FakeNow); a.setKdfIterationsForTests(1000); T_CHECK(a.open(d1, err));
        T_CHECK(a.userCount() == 1); Json ul = a.usersList(); T_CHECK(ul.at(0).get("name").str() == "admin" && ul.at(0).get("role").str() == "admin");
        T_CHECK(!a.settings().get("require_2fa_admins").boolean() && a.settings().get("enabled").boolean());
        Bytes f; T_CHECK(ReadWholeFile(JoinPath(d1, "web.json").c_str(), f, err)); Str txt((const char*)f.data(), f.size());
        T_CHECK(txt.find("default_admin_created") >= 0 && txt.find("\"hash\"") >= 0 && txt.find("\"admin\"") >= 0); T_CHECK(txt.find("password") < 0);
        WebAuth::LoginResult r = a.login("127.0.0.1", "admin", "admin", "", ""); T_CHECK(r.status == 200 && r.role == "admin");
        WebSession ss; T_CHECK(a.session(r.sessionId, ss) && WebAuth::RoleAllows(ss.role, "backups.restore"));
        Str au = a.auditTail(50); T_CHECK(au.find("default_admin_created") >= 0 && au.find("login_ok") >= 0 && au.find("admin/admin") < 0 && au.find("password") < 0);
        // the password is changed through web.users.save: the old one stops working
        Json up = Json::object(); up["name"] = "admin"; up["password"] = "Zebra-Quartz-Maple-77"; T_CHECK(a.usersSave(up, err));
        T_CHECK(!a.session(r.sessionId, ss));
        g_now += 100; r = a.login("127.0.0.1", "admin", "admin", "", ""); T_CHECK(r.status == 401);
        g_now += 100; r = a.login("127.0.0.1", "admin", "Zebra-Quartz-Maple-77", "", ""); T_CHECK(r.status == 200);
        T_CHECK(!a.usersSave([] { Json x = Json::object(); x["name"] = "admin"; x["password"] = "admin"; return x; }(), err));       // the policy still applies
        T_CHECK(a.usersDelete("admin", err)); T_CHECK(a.userCount() == 0);
    }
    {   // deleting every user does not bring the default administrator back
        WebAuth b; b.setKdfIterationsForTests(1000); T_CHECK(b.open(d1, err)); T_CHECK(b.userCount() == 0);
        WebAuth::LoginResult r = b.login("1.1.1.1", "admin", "admin", "", ""); T_CHECK(r.status == 401 && r.message.find("No users") >= 0);
    }
    {   // an existing installation (users, no flag) is untouched and gets no default administrator
        Str d2 = tmp.sub("d2");
        { WebAuth o; o.setKdfIterationsForTests(1000); T_CHECK(o.open(d2, err, false)); T_CHECK(o.userCount() == 0);
          Json u = Json::object(); u["name"] = "dave"; u["role"] = "viewer"; u["password"] = "Zebra-Quartz-Maple-77"; T_CHECK(o.usersSave(u, err)); }
        Bytes f1; T_CHECK(ReadWholeFile(JoinPath(d2, "web.json").c_str(), f1, err));
        Json doc; T_CHECK(Json::parse((const char*)f1.data(), f1.size(), doc, err)); doc.remove("default_admin_created");
        Str old = doc.dump(true); T_CHECK(WriteWholeFile(JoinPath(d2, "web.json").c_str(), old.data(), old.size(), err));
        WebAuth n; n.setKdfIterationsForTests(1000); T_CHECK(n.open(d2, err, true));
        T_CHECK(n.userCount() == 1 && n.usersList().at(0).get("name").str() == "dave"); T_CHECK(n.settings().get("require_2fa_admins").boolean());      // the stored value is kept
        Bytes f2; T_CHECK(ReadWholeFile(JoinPath(d2, "web.json").c_str(), f2, err)); T_CHECK(Str((const char*)f2.data(), f2.size()) == old);
        T_CHECK(n.auditTail(100).find("default_admin_created") < 0);
    }
    {   // lockout and rate limiting apply to admin / admin like to any user
        WebAuth c; c.setClock(FakeNow); c.setKdfIterationsForTests(1000); T_CHECK(c.open(tmp.sub("d3"), err));
        int count = 0, guard = 0; WebAuth::LoginResult r;
        while (count < 8 && guard++ < 100) { r = c.login("2.2.2.2", "admin", "wrong-guess-1", "", ""); if (r.status == 429) g_now += r.retryAfter; else ++count; }
        T_CHECK(count == 8);
        r = c.login("2.2.2.2", "admin", "admin", "", ""); T_CHECK(r.status == 429 && r.retryAfter > 800);
        r = c.login("3.3.3.3", "admin", "admin", "", ""); T_CHECK(r.status == 429);               // the user name is locked too
        g_now += 901; r = c.login("3.3.3.3", "admin", "admin", "", ""); T_CHECK(r.status == 200);
    }
    T_CHECK(WebAuth::IsLoopbackAddress("127.0.0.1") && WebAuth::IsLoopbackAddress("127.9.9.9") && WebAuth::IsLoopbackAddress("::1") && WebAuth::IsLoopbackAddress("::ffff:127.0.0.1"));
    T_CHECK(!WebAuth::IsLoopbackAddress("128.0.0.1") && !WebAuth::IsLoopbackAddress("0.0.0.0") && !WebAuth::IsLoopbackAddress("::") && !WebAuth::IsLoopbackAddress("10.0.0.1") && !WebAuth::IsLoopbackAddress("fe80::1") && !WebAuth::IsLoopbackAddress("junk"));
    return true;
}

VM2B_TEST("web", tls_certificate_and_server_basics)
{
    Tmp tmp(c_, "webtls"); Str err;
    WebTlsIdentity id; Vec<Str> names; names.push_back("backup.example.com"); names.push_back("10.1.2.3");
    T_CHECK(WebTlsGenerate(names, id, err)); T_CHECK(id.certDer.size() > 400 && id.keyProtected.starts_with("dpapi:"));
    Json d = WebTlsDescribe(id.certDer); T_CHECK(d.get("names").size() >= 6); T_CHECK(d.get("names").dump().find("backup.example.com") >= 0 && d.get("names").dump().find("10.1.2.3") >= 0 && d.get("names").dump().find("localhost") >= 0);
    i64 yrs = (d.get("not_after").integer() - d.get("not_before").integer()) / 86400; T_CHECK(yrs >= 825 && yrs <= 827);               // 825 days (+ the day of back-dating)
    Vec<Str> badn; badn.push_back("bad name\r\n"); T_CHECK(!WebTlsGenerate(badn, id, err));
    WebTlsGenerate(names, id, err);
    WebTlsServer srv; T_CHECK(srv.init(id, err)); T_CHECK(srv.fingerprint() == WebTlsFingerprint(id.certDer));
    ServiceCore core; ServiceOptions o; o.dataDir = tmp.sub("svc"); o.pipeName = Str::fmt("vm2b-webt-%lu", (unsigned long)GetCurrentProcessId()); o.pipeSddl = "D:(A;;GA;;;WD)"; o.tickMs = 100;
    T_CHECK(core.start(o, err));
    WebServer web; T_CHECK(web.start(&core, o.dataDir, err, false)); web.setKdfIterationsForTests(1000);
    Json r; T_CHECK(web.handleAdmin("web.status", Json::object(), r, err)); T_CHECK(!r.get("enabled").boolean() && !r.get("listening").boolean());
    Json def; web.handleAdmin("web.settings.get", Json::object(), def, err); T_CHECK(def.get("settings").get("bind").size() == 1 && def.get("settings").get("bind").at(0).str() == "127.0.0.1");
    int port = c_.range(24300, 24399);
    Json set = Json::object(); set["enabled"] = true; set["port"] = port;
    T_CHECK(web.handleAdmin("web.settings.set", set, r, err));
    T_CHECK(web.handleAdmin("web.status", Json::object(), r, err)); T_CHECK(r.get("listening").boolean() && r.get("entries").size() == 1);
    if (!r.get("listening").boolean()) { c_.note("status: %s", r.dump().c_str()); core.stop(); return false; }
    Str fp = r.get("cert").get("fingerprint").str(); T_CHECK(fp.starts_with("sha256:"));
    HttpAns h = Http("127.0.0.1", port, "GET", "/", "", "", fp);
    T_CHECK(h.ok && h.status == 200); if (!h.ok) c_.note("http: %s", h.err.c_str());
    T_CHECK(h.tls == "TLS 1.3");                      // OpenSSL on both ends (Schannel under Wine could not do 1.3)
    Str hl = h.head.lower();
    T_CHECK(hl.find("content-security-policy: default-src 'self'") >= 0 && hl.find("x-content-type-options: nosniff") >= 0 && hl.find("x-frame-options: deny") >= 0 && hl.find("referrer-policy:") >= 0 && hl.find("strict-transport-security:") >= 0);
    HttpAns bad = Http("127.0.0.1", port, "GET", "/", "", "", Str("sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")); T_CHECK(!bad.ok);
    HttpAns nf = Http("127.0.0.1", port, "GET", "/nothing", "", "", fp); T_CHECK(nf.status == 404);
    HttpAns me = Http("127.0.0.1", port, "GET", "/api/me", "", "", fp); T_CHECK(me.status == 200 && me.body.find("\"no_users\":true") >= 0);
    HttpAns un = Http("127.0.0.1", port, "POST", "/api/state", "{}", "Content-Type: application/json\r\n", fp); T_CHECK(un.status == 401);
    HttpAns sh = Http("127.0.0.1", port, "POST", "/api/service.shutdown", "{}", "Content-Type: application/json\r\n", fp); T_CHECK(sh.status == 401);
    web.stop(); core.stop();
    HttpAns gone = Http("127.0.0.1", port, "GET", "/", "", "", fp); T_CHECK(!gone.ok);
    return true;
}

VM2B_TEST("web", interfaces_choose_addresses_live)
{
    Tmp tmp(c_, "webif"); Str err;
    ServiceCore core; ServiceOptions o; o.dataDir = tmp.sub("svc"); o.pipeName = Str::fmt("vm2b-webi-%lu", (unsigned long)GetCurrentProcessId()); o.pipeSddl = "D:(A;;GA;;;WD)"; o.tickMs = 100;
    T_CHECK(core.start(o, err));
    WebServer web; T_CHECK(web.start(&core, o.dataDir, err, false)); web.setRetrySeconds(1);
    int port = c_.range(24400, 24499);
    auto status = [&]() { Json r; Str e; web.handleAdmin("web.status", Json::object(), r, e); return r; };
    auto reach = [&](const char* addr) { HttpAns h = Http(addr, port, "GET", "/api/me", "", "", ""); return h.ok && h.status == 200; };
    auto setBind = [&](const char* e1, const char* e2 = nullptr) { Json s = Json::object(); s["enabled"] = true; s["port"] = port; Json b = Json::array(); b.push(e1); if (e2) b.push(e2); s["bind"] = b; Json r; Str e; bool ok = web.handleAdmin("web.settings.set", s, r, e); if (!ok) c_.note("set: %s", e.c_str()); return r; };
    Json s0 = Json::object(); s0["enabled"] = true; s0["bind"] = Json::array(); Json rr; T_CHECK(!web.handleAdmin("web.settings.set", s0, rr, err));      // an empty list is refused when enabled
    Json s1 = Json::object(); s1["bind"] = "999.1.1.1"; T_CHECK(!web.handleAdmin("web.settings.set", s1, rr, err));
    Json s2 = Json::object(); s2["enabled"] = true; s2["port"] = port; T_CHECK(web.handleAdmin("web.settings.set", s2, rr, err));                       // default: 127.0.0.1 only
    T_CHECK(reach("127.0.0.1")); T_CHECK(!reach("127.0.0.2"));
    // choosing a second address; 127.0.0.1 stays unreachable when it is not chosen
    Json r = setBind("127.0.0.2"); T_CHECK(r.get("warnings").size() == 0);          // loopback: no warnings
    T_CHECK(reach("127.0.0.2")); T_CHECK(!reach("127.0.0.1"));
    Json st = status(); T_CHECK(st.get("entries").size() == 1 && st.get("entries").at(0).get("listening").boolean() && st.get("entries").at(0).get("addresses").at(0).get("address").str() == "127.0.0.2");
    setBind("127.0.0.1", "127.0.0.2"); T_CHECK(reach("127.0.0.1") && reach("127.0.0.2"));
    // the "localhost" shortcut: 127.0.0.1 and ::1 (IPv6 may be missing: then that address reports an error, the entry still listens)
    setBind("localhost"); st = status(); T_CHECK(st.get("entries").at(0).get("addresses").size() == 2 && st.get("entries").at(0).get("listening").boolean());
    bool v6 = st.get("entries").at(0).get("addresses").at(1).get("listening").boolean();
    T_CHECK(reach("127.0.0.1"));
    if (v6) { SOCKET s6 = socket(AF_INET6, SOCK_STREAM, 0); sockaddr_in6 a6; memset(&a6, 0, sizeof(a6)); a6.sin6_family = AF_INET6; a6.sin6_addr.s6_addr[15] = 1; a6.sin6_port = htons((u_short)port); T_CHECK(connect(s6, (sockaddr*)&a6, sizeof(a6)) == 0); closesocket(s6); }
    else c_.note("IPv6 loopback is not available here: ::1 not tested");
    // an address that does not exist: reported per entry, the other entry works
    r = setBind("127.0.0.1", "203.0.113.77"); st = status(); T_CHECK(st.get("entries").size() == 2 && st.get("entries").at(0).get("listening").boolean() && !st.get("entries").at(1).get("listening").boolean() && st.get("entries").at(1).get("error").str().size() > 3);
    T_CHECK(reach("127.0.0.1"));
    T_CHECK(r.get("warnings").size() == 2);                                         // non-loopback: self-signed certificate + no 2FA user
    // a taken port is reported and retried: occupy 127.0.0.2:port, then free it
    setBind("127.0.0.1");
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET blocker = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in ba; memset(&ba, 0, sizeof(ba)); ba.sin_family = AF_INET; ba.sin_addr.s_addr = inet_addr("127.0.0.2"); ba.sin_port = htons((u_short)port);
    int one = 1; setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    T_CHECK(bind(blocker, (sockaddr*)&ba, sizeof(ba)) == 0); listen(blocker, 1);
    setBind("127.0.0.1", "127.0.0.2"); st = status(); T_CHECK(!st.get("entries").at(1).get("listening").boolean()); c_.note("busy port error: %s", st.get("entries").at(1).get("error").str().c_str()); T_CHECK(st.get("entries").at(1).get("error").str().find("in use") >= 0);
    closesocket(blocker);
    bool back = false; for (int i = 0; i < 80 && !back; ++i) { Sleep(100); back = status().get("entries").at(1).get("listening").boolean(); }
    T_CHECK(back); T_CHECK(reach("127.0.0.2"));
    // an adapter entry: all its addresses, following changes of the adapter (a changed DHCP address, down, up)
    SetIfs(web, [] { Json a = Json::array(); a.push(FakeAdapter("{AAAA-1111}", true, "127.0.0.1")); return a; }());
    Json il; T_CHECK(web.handleAdmin("web.interfaces.list", Json::object(), il, err)); T_CHECK(il.get("interfaces").size() == 1 && il.get("interfaces").at(0).get("name").str() == "{AAAA-1111}");
    setBind("adapter:{AAAA-1111}"); T_CHECK(reach("127.0.0.1")); T_CHECK(!reach("127.0.0.2"));
    SetIfs(web, [] { Json a = Json::array(); a.push(FakeAdapter("{AAAA-1111}", true, "127.0.0.2")); return a; }());
    bool moved = false; for (int i = 0; i < 80 && !moved; ++i) { Sleep(100); moved = reach("127.0.0.2"); }
    T_CHECK(moved); T_CHECK(!reach("127.0.0.1"));
    SetIfs(web, [] { Json a = Json::array(); a.push(FakeAdapter("{AAAA-1111}", false, "127.0.0.2")); return a; }());
    bool gone = false; for (int i = 0; i < 80 && !gone; ++i) { Sleep(100); gone = !reach("127.0.0.2"); }
    T_CHECK(gone); st = status(); T_CHECK(!st.get("entries").at(0).get("listening").boolean() && st.get("entries").at(0).get("error").str().find("down") >= 0);
    SetIfs(web, [] { Json a = Json::array(); a.push(FakeAdapter("{AAAA-1111}", true, "127.0.0.1", "127.0.0.2")); return a; }());
    bool both = false; for (int i = 0; i < 80 && !both; ++i) { Sleep(100); both = reach("127.0.0.1") && reach("127.0.0.2"); }
    T_CHECK(both);
    SetIfs(web, Json::array()); bool none = false; for (int i = 0; i < 80 && !none; ++i) { Sleep(100); none = !reach("127.0.0.1"); }
    T_CHECK(none); T_CHECK(status().get("entries").at(0).get("error").str().find("not found") >= 0);
    web.stop(); core.stop();
    return true;
}

// ---- the host for the Python clients -----------------------------------------------------------------------------------------------
VM2B_TEST("web", host_for_python_clients)
{
    if (!c_.servers.is_object() || !c_.servers.has("web")) { c_.skip("no --servers file with a web section (tools/run-web-tests.sh)"); return true; }
    if (!g_ifInit) { InitializeCriticalSection(&g_ifCs); g_ifInit = true; }
    Str ctl = c_.servers.get("web").get("ctl_dir").str(), dataDir = c_.servers.get("web").get("data_dir").str(), err;
    MakeDirs(ctl, err);
    ServiceCore core; ServiceOptions o; o.dataDir = JoinPath(dataDir, "svc"); o.pipeName = Str::fmt("vm2b-webh-%lu", (unsigned long)GetCurrentProcessId()); o.pipeSddl = "D:(A;;GA;;;WD)"; o.tickMs = 100;
    T_CHECK(core.start(o, err)); if (c_.failed) return false;
    WebServer web; T_CHECK(web.start(&core, o.dataDir, err, false));
    web.setKdfIterationsForTests((u32)c_.servers.get("web").get("kdf").integer(2000)); web.setClock(OffsetNow); web.setRetrySeconds(1);
    Str ready = JoinPath(ctl, "ready.txt"); Str e2; WriteWholeFile(ready.c_str(), "ready", 5, e2);
    DWORD start = GetTickCount(); bool quit = false;
    while (!quit && GetTickCount() - start < 900000) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(FilePath(JoinPath(ctl, "req-*.json")), &fd);
        if (h == INVALID_HANDLE_VALUE) { Sleep(15); continue; }
        Str name = FromWide(fd.cFileName); FindClose(h);
        Str path = JoinPath(ctl, name); Bytes b; Str e;
        Json req, res = Json::object();
        if (!ReadWholeFile(path.c_str(), b, e)) { Sleep(10); continue; }
        DeleteFileW(FilePath(path));
        if (!Json::parse((const char*)b.data(), b.size(), req, e)) {        // the clients write atomically (rename): unparsable means too deep / damaged: answer, do not loop forever
            Str rid0 = name.substr(4, name.size() - 9); Str txt0 = "{\"ok\":false,\"result\":{},\"error\":\"mailbox request not parsable\"}"; Str e0;
            WriteWholeFile(JoinPath(ctl, "res-" + rid0 + ".json").c_str(), txt0.data(), txt0.size(), e0); continue;
        }
        Json out; Str er; bool ok = true;
        if (req.has("admin")) ok = web.handleAdmin(req.get("admin").str(), req.get("args"), out, er);
        else if (req.has("core")) ok = core.handle(req.get("core").str(), req.get("args"), out, er);
        else if (req.has("clock_add")) { g_clockOffset += req.get("clock_add").integer(); out = Json::object(); out["now"] = OffsetNow(); }
        else if (req.has("ifaces")) { EnterCriticalSection(&g_ifCs); g_ifs = req.get("ifaces"); LeaveCriticalSection(&g_ifCs); web.setInterfaceProvider(req.get("ifaces").is_array() ? IfProvider : nullptr); out = Json::object(); }
        else if (req.get("quit").boolean()) { quit = true; out = Json::object(); }
        else { ok = false; er = "unknown mailbox request"; }
        res["ok"] = ok; res["result"] = out; res["error"] = er;
        Str txt = res.dump(); Str rid = name.substr(4, name.size() - 9);
        WriteWholeFile(JoinPath(ctl, "res-" + rid + ".json").c_str(), txt.data(), txt.size(), e);
    }
    web.stop(); core.stop();
    Bytes vb; Str ve; bool verdict = ReadWholeFile(JoinPath(ctl, "verdict.txt").c_str(), vb, ve) && Str((const char*)vb.data(), vb.size()).starts_with("PASS");
    T_CHECK(verdict);
    return true;
}

} // namespace vm2b
