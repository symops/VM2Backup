// Autotests of the lock-out guard for SCP servers (group "ssh"): a scheduled task with a wrong password / a refused key must reach the real
// sshd with ONE authentication attempt, however many scheduler passes follow; a changed record is tried once again.  The attempts are counted
// in the log of a real OpenSSH server (LogLevel VERBOSE, started by tools/run-ssh-tests.sh as "scp_authlog", the log path is servers.json "x.authlog").
#include "autotest.h"
#include "engine.h"
#include "config.h"
#include "tests_svc.h"
#include "schedule.h"

namespace vm2b {
namespace {

bool HaveAuthLog(Ctx& c)
{
    if (c.servers.is_null() || !c.servers.get("scp_authlog").is_object() || c.servers.get("x").get("authlog").str().empty()) { c.skip("no --servers file with scp_authlog (use tools/run-ssh-tests.sh)"); return false; }
    return true;
}

Str ReadLog(Ctx& c) { Bytes b; Str e; if (!ReadWholeFile(c.servers.get("x").get("authlog").str().c_str(), b, e)) return Str(); return Str((const char*)b.data(), b.size()); }

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

} // namespace vm2b
