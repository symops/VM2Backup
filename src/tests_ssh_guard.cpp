// Autotests of the lock-out guard for SCP servers (group "ssh"): a scheduled task with a wrong password / a refused key must reach the real
// sshd with ONE authentication attempt, however many scheduler passes follow; a changed record is tried once again.  The attempts are counted
// in the log of a real OpenSSH server (LogLevel VERBOSE, started by tools/run-ssh-tests.sh as "scp_authlog", the log path is servers.json "x.authlog").
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "tests_svc.h"
#include "schedule.h"
#include "ssh.h"
#include <winsock2.h>
#include <ws2tcpip.h>

namespace vm2b {
namespace {

bool HaveAuthLog(Ctx& c)
{
    if (c.servers.is_null() || !c.servers.get("scp_authlog").is_object() || c.servers.get("x").get("authlog").str().empty()) { c.skip("no --servers file with scp_authlog (use tools/run-ssh-tests.sh)"); return false; }
    return true;
}

// The log is read in one of two ways.  "x.authlog_tcp" (host:port, tools/testservers/logread.py) is a direct TCP read of the file as the host has it
// RIGHT NOW: on the Windows VM the log lies on a shared folder of the host, and the SMB client of the guest answered from its cache (the counters
// stayed 0 with real attempts in the log), so the VM harness always gives the TCP address.  Without it the file is read by its path (Wine, a local run).
bool ReadLogTcp(const Str& addr, Str& out)
{
    int colon = addr.rfind(':'); if (colon <= 0) return false;
    Str host = addr.substr(0, (size_t)colon), port = addr.substr((size_t)colon + 1);
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    addrinfo hints = {}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol); bool ok = false;
    if (s != INVALID_SOCKET) {
        DWORD to = 10000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
            char buf[16384]; out.clear(); ok = true;
            for (;;) { int n = recv(s, buf, sizeof buf, 0); if (n < 0) { ok = false; break; } if (n == 0) break; out.append(buf, (size_t)n); }
        }
        closesocket(s);
    }
    freeaddrinfo(res); return ok;
}

Str ReadLog(Ctx& c)
{
    Str tcp = c.servers.get("x").get("authlog_tcp").str();
    if (!tcp.empty()) { Str t; for (int i = 0; i < 3; ++i) { if (ReadLogTcp(tcp, t)) return t; Sleep(500); } c.note("the log could not be read from %s", tcp.c_str()); return Str(); }
    Bytes b; Str e; if (!ReadWholeFile(c.servers.get("x").get("authlog").str().c_str(), b, e)) return Str(); return Str((const char*)b.data(), b.size());
}

int CountOf(const Str& text, const char* needle)
{
    int n = 0; size_t nl = strlen(needle); int pos = 0;
    for (;;) { int f = text.find(needle, (size_t)pos); if (f < 0) break; ++n; pos = f + (int)nl; }
    return n;
}

struct AuthCounts { int failedPw, failedKey, accepted, connections; };
// what the server logged after byte offset `from`
AuthCounts Counts(Ctx& c, size_t from)
{
    Sleep(700);                                                   // sshd writes its log asynchronously
    Str all = ReadLog(c); Str t = all.size() > from ? all.substr(from) : Str();
    AuthCounts r; r.failedPw = CountOf(t, "Failed password for"); r.failedKey = CountOf(t, "Failed publickey for");
    r.accepted = CountOf(t, "Accepted password for") + CountOf(t, "Accepted publickey for"); r.connections = CountOf(t, "Connection from ");
    return r;
}

Json ScpServerJson(Ctx& c, const char* name)
{
    const Json& s = c.servers.get("scp_authlog");
    Json sv = Json::object(); sv["type"] = "scp"; sv["name"] = name; sv["host"] = s.get("host").str("127.0.0.1"); sv["port"] = (i64)s.get("port").integer(); sv["user"] = s.get("user").str();
    sv["host_key"] = s.get("host_key").str(); sv["directory"] = "guard";
    return sv;
}

Json EveryTen(i64 t0)
{
    Json sch = Json::object(); sch["enabled"] = true; sch["mode"] = "every"; Json ev = Json::object(); ev["value"] = 10; ev["unit"] = "minutes"; sch["every"] = ev;
    sch["first_time"] = FormatLocalMin(UnixToLocalMin(t0)).replace_all(" ", "T"); return sch;
}
Json ElementTo(const Str& src, const Str& tgtServer) { Json el = Svc::Element(src, "vm1", "{VM}"); Json tg = Json::object(); tg["server"] = tgtServer; tg["directory"] = "{VM}"; el["target"] = tg; el["verify"] = false; return el; }
void Passes(Svc& s, int n) { for (int i = 0; i < n; ++i) { int h = s.historyCount(); g_fakeNow += 10 * 60; s.waitHistory(h + 1, 30000); } }
Json LastRecord(Svc& s)
{
    Json l = s.ok("history.list", [] { Json a = Json::object(); a["limit"] = 1; return a; }());
    if (l.size() == 0) return Json::object();
    Json a = Json::object(); a["id"] = l.at(0).get("id").str(); return s.ok("history.get", a);
}
Json ServerOf(Svc& s, const Str& id)
{
    Json l = s.ok("servers.list", Json::object()); for (size_t i = 0; i < l.size(); ++i) if (l.at(i).get("id").str() == id) return l.at(i);
    return Json::object();
}

// one scenario: `bad` is the record with the refused credentials, `good` the same record with working ones
bool Scenario(Ctx& c_, const char* tag, const Json& bad, const Json& good, bool keyKind)
{
    Svc s(c_, tag, true); if (!s.up) return false;
    s.record = true;
    Str vms = s.tmp.sub("vms"); WriteBytes(JoinPath(vms, "vm1\\disk.vmdk"), Random(c_, 30000)); Str src = s.addDirServer(vms);
    Json sa = Json::object(); sa["server"] = bad; Str id = s.ok("servers.save", sa).get("id").str(); T_CHECK(!id.empty());
    g_fakeNow = (i64)UnixNow() + 3600; i64 t0 = g_fakeNow;
    Str task = s.saveTask("ToScp", ElementTo(src, id), EveryTen(t0)); T_CHECK(!task.empty());
    size_t off = ReadLog(c_).size();
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = true; return a; }()); Sleep(200);
    const int kPasses = 6;
    Passes(s, kPasses);
    AuthCounts n = Counts(c_, off);
    c_.note("%s: %d scheduler passes with refused credentials: sshd logged %d failed password, %d failed publickey, %d accepted, %d connection(s)", tag, kPasses, n.failedPw, n.failedKey, n.accepted, n.connections);
    T_CHECK(n.accepted == 0);
    T_CHECK(keyKind ? n.connections <= 1 : (n.failedPw == 1 && n.connections <= 1));       // one connection, one password attempt: the other five passes did not touch the server
    if (!keyKind) T_CHECK(n.failedKey == 0);
    Json rec = LastRecord(s); T_CHECK(rec.get("status").str() == "Failed");
    T_CHECK(rec.get("results").size() == 1 && rec.get("results").at(0).get("skipped").boolean());
    T_CHECK(rec.get("results").at(0).get("message").str().find("were rejected") >= 0);
    T_CHECK(ServerOf(s, id).get("auth_blocked").is_object());
    // the record is changed (right password / key): the next pass goes to the server once more, and works
    Json g = good; g["id"] = id; Json sb = Json::object(); sb["server"] = g; s.ok("servers.save", sb);
    T_CHECK(!ServerOf(s, id).get("auth_blocked").is_object());
    size_t off2 = ReadLog(c_).size();
    Passes(s, 1);
    AuthCounts m = Counts(c_, off2);
    c_.note("%s: after the record was fixed, one pass: %d failed, %d accepted login(s), %d connection(s)", tag, m.failedPw + m.failedKey, m.accepted, m.connections);
    T_CHECK(m.failedPw == 0 && m.failedKey == 0); T_CHECK(m.accepted >= 1);
    T_CHECK(LastRecord(s).get("status").str() == "Success");
    // and a second wrong round: the mark comes back after exactly one more refusal
    Json b2 = bad; b2["id"] = id; Json sc = Json::object(); sc["server"] = b2; s.ok("servers.save", sc);
    size_t off3 = ReadLog(c_).size();
    Passes(s, 4);
    AuthCounts k = Counts(c_, off3);
    c_.note("%s: broken again, 4 passes: %d failed attempt(s), %d connection(s)", tag, k.failedPw + k.failedKey, k.connections);
    T_CHECK(k.connections <= 1); T_CHECK(!keyKind ? k.failedPw == 1 : true);
    { Str bp = bad.get("password").str(); if (!bp.empty()) T_CHECK(s.transcript.find(bp.c_str()) < 0); }
    s.ok("scheduler.set", [] { Json a = Json::object(); a["enabled"] = false; return a; }());
    return true;
}

} // namespace

VM2B_TEST("ssh", auth_guard_scp_password_one_attempt)
{
    if (!HaveAuthLog(c_)) return true;
    const Json& s0 = c_.servers.get("scp_authlog");
    Json bad = ScpServerJson(c_, "Guard SCP"); bad["password"] = "Wr0ng-Scp-Pw-Lockout";
    Json good = ScpServerJson(c_, "Guard SCP"); good["password"] = s0.get("password").str();
    return Scenario(c_, "scpag1", bad, good, false);
}

VM2B_TEST("ssh", auth_guard_scp_key_one_attempt)
{
    if (!HaveAuthLog(c_)) return true;
    const Json& sv = c_.servers.get("ssh"); Str keys = sv.get("keys").str();
    // a private key the server does not know
    Json bad = ScpServerJson(c_, "Guard SCP key"); bad["key_file"] = keys + "/n_unauthorized";
    Json good = ScpServerJson(c_, "Guard SCP key"); good["key_file"] = keys + "/u_ed25519";
    return Scenario(c_, "scpag2", bad, good, true);
}

VM2B_TEST("ssh", auth_guard_scp_key_wrong_passphrase_one_attempt)
{
    if (!HaveAuthLog(c_)) return true;
    const Json& sv = c_.servers.get("ssh"); Str keys = sv.get("keys").str();
    Json bad = ScpServerJson(c_, "Guard SCP pass"); bad["key_file"] = keys + "/u_ed25519_pw"; bad["key_passphrase"] = "wrong passphrase 999";
    Json good = ScpServerJson(c_, "Guard SCP pass"); good["key_file"] = keys + "/u_ed25519_pw"; good["key_passphrase"] = sv.get("passphrase").str();
    return Scenario(c_, "scpag3", bad, good, true);
}

// The log must show what sshd has just done, with no client-side cache in between (the SMB client of a Windows guest used to answer from its cache:
// the guard tests counted 0 attempts with real ones in the log).  A refused login is made here and the counter has to grow within a few seconds.
VM2B_TEST("ssh", authlog_shows_a_fresh_refused_login_at_once)
{
    if (!HaveAuthLog(c_)) return true;
    const Json& s0 = c_.servers.get("scp_authlog");
    for (int round = 0; round < 3; ++round) {
        size_t off = ReadLog(c_).size();
        SshConfig cfg; cfg.host = s0.get("host").str("127.0.0.1"); cfg.port = (int)s0.get("port").integer(); cfg.user = s0.get("user").str(); cfg.password = "Wr0ng-Authlog-Pw";
        cfg.hostKey = s0.get("host_key").str(); cfg.ioTimeoutMs = 20000;
        { SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); }
        AuthCounts n = {0, 0, 0, 0}; double t0 = UnixNow();
        while (UnixNow() - t0 < 6) { n = Counts(c_, off); if (n.failedPw >= 1) break; }
        c_.note("round %d: the log grew by %d failed password(s) within %.1f s%s", round, n.failedPw, UnixNow() - t0, c_.servers.get("x").get("authlog_tcp").str().empty() ? " (read by path)" : " (read over TCP)");
        T_CHECK(n.failedPw == 1);
    }
    return true;
}

} // namespace vm2b
