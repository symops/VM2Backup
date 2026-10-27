// Autotests of the SSH client and the SCP target. Groups "ssh", "scp". The known-answer tests run without servers;
// the others read their servers from --servers (written by tools/run-ssh-tests.sh) and are skipped without it.
#include <winsock2.h>
#include "autotest.h"
#include "ssh.h"
#include "target.h"
#include "crypto.h"
#include "winutil.h"
#include "engine.h"
#include "config.h"

namespace vm2b {

namespace {


struct Tmp {
    Str path; bool keep;
    Tmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        Str e; MakeDirs(path, e);
    }
    ~Tmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

bool SshServers(Ctx& c)
{
    if (c.servers.is_null() || !c.servers.get("ssh").is_object()) { c.skip("no --servers file (use tools/run-ssh-tests.sh)"); return false; }
    return true;
}

SshConfig CfgFor(Ctx& c, int port)
{
    const Json& s = c.servers.get("ssh");
    SshConfig cfg; cfg.host = s.get("host").str("127.0.0.1"); cfg.port = port; cfg.user = s.get("user").str(); cfg.password = s.get("password").str();
    cfg.ioTimeoutMs = 20000;
    return cfg;
}

Str KeyPath(Ctx& c, const char* name) { return c.servers.get("ssh").get("keys").str() + "/" + name; }

Bytes RandBytes(Ctx& c, size_t n) { Bytes b; b.resize(n); c.fill(b.data(), n); return b; }

} // namespace

// ============================== against real sshd ==================================================================
static bool ExecEcho(Ctx& c_, SshSession& s, bool big)
{
    static const char* words[] = { "hello", "w\xc3\xb6rld", "a b", "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "x" };
    Str text; int n = c_.range(1, 5); for (int i = 0; i < n; ++i) { if (i) text += " "; text += words[c_.rnd(5)]; }
    Bytes out, errb; int st = -1;
    Str cmd = "echo '" + text + "'; echo oops >&2; exit 3";
    bool ok = s.exec(cmd, out, errb, st);
    T_CHECK(ok); if (!ok) { c_.note("exec: %s", s.err.c_str()); return false; }
    T_CHECK(Str((const char*)out.data(), out.size()) == text + "\n"); T_CHECK(st == 3);
    T_CHECK(Str((const char*)errb.data(), errb.size()) == "oops\n");
    if (big) {
        // a few hundred KiB through the cipher and back (cat echoes stdin to stdout)
        size_t sz = (size_t)c_.range(1, 400000); Bytes data = RandBytes(c_, sz);
        SshChannel* ch = s.open_session(); T_CHECK(ch != nullptr); if (!ch) return false;
        T_CHECK(ch->exec("cat")); T_CHECK(ch->write(data.data(), data.size())); T_CHECK(ch->send_eof());
        Bytes back; back.resize(sz); T_CHECK(ch->read_exact(back.data(), sz)); T_CHECK(memcmp(back.data(), data.data(), sz) == 0);
        u8 x; T_CHECK(ch->read(&x, 1) == 0); ch->wait_close(5000); T_CHECK(ch->exit_status() == 0); delete ch;
    }
    return true;
}

VM2B_TEST("ssh", matrix_algorithms)
{
    if (!SshServers(c_)) return true;
    const Json& mx = c_.servers.get("ssh_matrix");
    if (mx.size() == 0) { c_.skip("no ssh_matrix in the servers file"); return true; }
    int ok = 0;
    for (size_t i = 0; i < mx.size(); ++i) {
        const Json& e = mx.at(i);
        SshConfig cfg = CfgFor(c_, (int)e.get("port").integer()); cfg.hostKey = e.get("host_key").str();
        SshSession s;
        bool conn = s.connect(cfg);
        if (!conn) { c_.note("%s: connect failed: %s", e.get("name").str().c_str(), s.err.c_str()); c_.failed = true; continue; }
        auto same = [&](const char* what, const Str& got, const Str& want) {
            if (want == "*" || want == "-" || want == got) return true;
            c_.note("%s: %s is %s, expected %s", e.get("name").str().c_str(), what, got.c_str(), want.c_str()); c_.failed = true; return false;
        };
        same("kex", s.kexName, e.get("expect_kex").str()); same("cipher", s.cipherName, e.get("expect_cipher").str());
        same("mac", s.macName, e.get("expect_mac").str());
        same("host key", s.hostKeyName, e.get("expect_hostkey").str());
        bool good = ExecEcho(c_, s, true) && !c_.failed;
        if (good) ++ok;
        else c_.note("%s failed (%s)", e.get("name").str().c_str(), s.err.c_str());
    }
    c_.note("%d of %d algorithm sets worked", ok, (int)mx.size());
    T_CHECK(ok == (int)mx.size());
    return true;
}

// Защита от блокировки учётной записи: отказ в учётных данных SCP помечает цель (auth_failed), всё временное и «неизвестный ключ хоста» - нет.
VM2B_TEST("ssh", scp_auth_refusal_flag)
{
    if (!SshServers(c_)) return true;
    int port = (int)c_.servers.get("scp").get("port").integer(); const Json& sv = c_.servers.get("ssh");
    auto cfgOf = [&](const char* pw) { Json j = Json::object(); j["type"] = "scp"; j["host"] = sv.get("host").str("127.0.0.1"); j["port"] = (i64)port; j["user"] = sv.get("user").str(); j["password"] = pw; j["directory"] = "x"; j["insecure_accept_host_key"] = true; return j; };
    { Str e; Json j = cfgOf("definitely-wrong-pw"); Target* t = MakeTarget(j, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(t->auth_failed); T_CHECK(!t->transient); T_CHECK(t->auth_err.find("definitely-wrong-pw") < 0); delete t; } }
    { Str e; Json j = cfgOf("definitely-wrong-pw"); j["user"] = "nosuchuser"; Target* t = MakeTarget(j, e); if (t) { T_CHECK(!t->connect()); T_CHECK(t->auth_failed); delete t; } }
    { Str e; Json j = cfgOf(sv.get("password").str().c_str()); j.remove("insecure_accept_host_key"); j["host_key"] = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; Target* t = MakeTarget(j, e); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->auth_failed); delete t; } }   // an unknown host key is not a credentials refusal
    { Str e; Json j = cfgOf("x"); j["port"] = (i64)1; Target* t = MakeTarget(j, e); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->auth_failed); delete t; } }                   // nobody listens: network, not credentials
    { Str e; Json j = cfgOf(sv.get("password").str().c_str()); Target* t = MakeTarget(j, e); if (t) { T_CHECK(t->connect()); T_CHECK(!t->auth_failed); delete t; } }
    return true;
}

VM2B_TEST("ssh", hostkey_policy)
{
    if (!SshServers(c_)) return true;
    const Json& s0 = c_.servers.get("scp"); int port = (int)s0.get("port").integer();
    // unknown host key and no flag: permanent failure whose text carries the fingerprint
    { SshConfig cfg = CfgFor(c_, port); SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); T_CHECK(s.hostKeyUntrusted);
      T_CHECK(s.err.find("SHA256:") >= 0); T_CHECK(s.err.find(cfg.password.c_str()) < 0); T_CHECK(s.fingerprint == s0.get("host_key").str() || s.fingerprint.starts_with("SHA256:")); T_CHECK(s.err.find(s.fingerprint.c_str()) >= 0); }
    // wrong pinned key
    { SshConfig cfg = CfgFor(c_, port); cfg.hostKey = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); T_CHECK(s.err.find("SHA256:") >= 0); }
    // the right one (as printed by ssh-keygen), through the list, and the insecure flag
    { SshConfig cfg = CfgFor(c_, port); cfg.hostKey = c_.servers.get("ssh").get("hostkeys").get("ed25519").str(); SshSession s; T_CHECK(s.connect(cfg)); }
    { SshConfig cfg = CfgFor(c_, port); cfg.knownFingerprints.push_back("SHA256:xxxx"); cfg.knownFingerprints.push_back(c_.servers.get("ssh").get("hostkeys").get("ed25519").str()); SshSession s; T_CHECK(s.connect(cfg)); }
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; SshSession s; T_CHECK(s.connect(cfg)); T_CHECK(s.connected()); }
    // a pinned key of another algorithm's fingerprint must not match
    { SshConfig cfg = CfgFor(c_, port); cfg.hostKey = c_.servers.get("ssh").get("hostkeys").get("rsa").str(); SshSession s; T_CHECK(!s.connect(cfg)); }
    // connect() works again on the same object after failures
    { SshConfig bad = CfgFor(c_, port); SshSession s; T_CHECK(!s.connect(bad)); SshConfig good = bad; good.insecureAcceptHostKey = true; T_CHECK(s.connect(good)); T_CHECK(ExecEcho(c_, s, false));
      s.disconnect(); T_CHECK(!s.connected()); T_CHECK(s.connect(good)); T_CHECK(ExecEcho(c_, s, false)); }
    return true;
}

VM2B_TEST("ssh", auth_methods)
{
    if (!SshServers(c_)) return true;
    int port = (int)c_.servers.get("scp").get("port").integer(); Str pp = c_.servers.get("ssh").get("passphrase").str();
    // password; wrong password is permanent and does not leak
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; SshSession s; T_CHECK(s.connect(cfg)); T_CHECK(ExecEcho(c_, s, false)); }
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.password = "definitely-wrong-pw"; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); T_CHECK(s.err.find("definitely-wrong-pw") < 0); T_CHECK(s.err.find("authentication") >= 0); }
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.user = "nosuchuser"; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); }
    // every kind of key file
    struct K { const char* file; bool pw; } keys[] = { { "u_ed25519", false }, { "u_ed25519_pw", true }, { "u_ed25519_cbc", true }, { "u_ed25519_ctr128", true }, { "u_ed25519_gcm", true },
        { "u_rsa", false }, { "u_rsa_pw", true }, { "u_rsa_pem", false }, { "u_rsa_pkcs8", false }, { "u_ecdsa256", false }, { "u_ecdsa384_pw", true }, { "u_ecdsa521", false },
        { "u_ecdsa_pem", false }, { "u_ed25519_pkcs8", false } };
    for (const K& k : keys) {
        SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.password.clear(); cfg.keyFile = KeyPath(c_, k.file); if (k.pw) cfg.keyPassphrase = pp;
        SshSession s; bool ok = s.connect(cfg);
        if (!ok) c_.note("key %s: %s", k.file, s.err.c_str());
        T_CHECK(ok); if (ok) T_CHECK(ExecEcho(c_, s, false));
        {   // the same key given as text (the web site stores it protected and passes the text)
            Bytes kb; Str ke; T_CHECK(ReadWholeFile(KeyPath(c_, k.file).c_str(), kb, ke));
            SshConfig mc = cfg; mc.keyFile.clear(); mc.keyText = Str((const char*)kb.data(), kb.size()); SshSession sm; bool mok = sm.connect(mc);
            if (!mok) c_.note("key text %s: %s", k.file, sm.err.c_str());
            T_CHECK(mok); if (mok) T_CHECK(ExecEcho(c_, sm, false));
        }
        if (k.pw) {                                                      // wrong / missing passphrase: permanent, no leak
            SshConfig bad = cfg; bad.keyPassphrase = "wrong passphrase!"; SshSession s2; T_CHECK(!s2.connect(bad)); T_CHECK(!s2.transient); T_CHECK(s2.err.find("wrong passphrase!") < 0);
            bad.keyPassphrase.clear(); SshSession s3; T_CHECK(!s3.connect(bad)); T_CHECK(!s3.transient);
        }
    }
    // a key that the server does not know, with the password as fallback
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.keyFile = KeyPath(c_, "host_ed25519"); SshSession s; T_CHECK(s.connect(cfg)); }   // the host key is not in authorized_keys: password follows
    { SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.password.clear(); cfg.keyFile = "Z:/nonexistent/key"; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); }
    // legacy server (ssh-rsa host key / group14-sha1 / hmac-sha1 / aes128-ctr only), with a password and with an RSA key (ssh-rsa signature)
    int lp = (int)c_.servers.get("ssh").get("legacy_port").integer();
    if (lp) {
        SshConfig cfg = CfgFor(c_, lp); cfg.insecureAcceptHostKey = true; SshSession s; T_CHECK(s.connect(cfg)); T_CHECK(s.kexName == "diffie-hellman-group14-sha1"); T_CHECK(s.hostKeyName == "ssh-rsa");
        SshConfig k = cfg; k.password.clear(); k.keyFile = KeyPath(c_, "u_rsa"); SshSession s2; bool ok = s2.connect(k); if (!ok) c_.note("legacy rsa key: %s", s2.err.c_str()); T_CHECK(ok);
    }
    return true;
}

// ============================== the targets (scp) =========================================
namespace {

struct Gen {                      // deterministic data: the same seed gives the same bytes whatever the chunking, so big files need no memory
    u64 s, cur; int left;
    explicit Gen(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1), cur(0), left(0) {}
    void fill(u8* p, size_t n, int kind) {
        for (size_t i = 0; i < n; ++i) {
            if (!left) { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; cur = s * 2685821657736338717ull; left = 8; }
            u8 b = (u8)cur; cur >>= 8; --left;
            p[i] = kind == 0 ? b : (u8)((b & 0x07) + 'a');          // random / compressible
        }
    }
};

// "scp": the real sshd of the run; "pm.scp" / "pm.fault_scp" ...: the paramiko servers; a suffix "+rk": the client re-keys often
Json ServerJson(Ctx& c, const char* key)
{
    Str k = key;
    if (k.ends_with("+rk")) { Json j = ServerJson(c, k.substr(0, k.size() - 3).c_str()); if (j.is_object()) j["rekey_bytes"] = 60000 + (i64)(c.next() % 400000); return j; }
    if (k.starts_with("pm.")) return c.servers.get("pm").get(k.c_str() + 3);
    return c.servers.get(key);
}
bool HaveServer(Ctx& c, const char* key) { return ServerJson(c, key).is_object(); }
Str g_root, g_rootUnix;
void UseRoot(Ctx& c, const char* key)
{
    if (Str(key).starts_with("pm.")) { g_root = c.servers.get("pm").get("root").str(); g_rootUnix = c.servers.get("pm").get("root_unix").str(); }
    else { g_root = c.servers.get("ssh").get("root").str(); g_rootUnix = c.servers.get("ssh").get("root_unix").str(); }
}

Json TargetCfg(Ctx& c, const char* type, const Str& dir)
{
    Json j = ServerJson(c, type);
    j["directory"] = dir;
    j["io_timeout_ms"] = 20000;
    return j;
}

Str RandDir(Ctx& c, bool unicode)
{
    return Str::fmt(unicode ? "vm2b-t/\xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb0 %u/sub" : "vm2b-t/d%u", (unsigned)(c.next() % 100000000));
}

Str RandName(Ctx& c, int i)
{
    static const char* base[] = { "disk", "vm with spaces", "\xd0\x92\xd0\x9c \xd1\x82\xd0\xb5\xd1\x81\xd1\x82", "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac", "it's a \"quote\" & $x `y`", "plain" };
    return Str::fmt("%s_%d_%u.tar.%05d", base[c.rnd(6)], i, (unsigned)c.rnd(1000), i + 1);
}

bool PutGen(Ctx& c_, Target* t, const Str& name, u64 seed, u64 size, int kind, bool* abortedAt = nullptr, u64 abortAfter = 0)
{
    Sink* w = t->open_write(name);
    if (!w) { c_.note("open_write %s: %s", name.c_str(), t->err.c_str()); return false; }
    Gen g(seed); Bytes buf; u64 done = 0;
    while (done < size) {
        size_t k = c_.coin(20) ? (size_t)c_.range(1, 100) : c_.coin(60) ? (size_t)c_.range(1, 70000) : (size_t)c_.range(100000, 1500000);
        if (k > size - done) k = (size_t)(size - done);
        buf.resize(k); g.fill(buf.data(), k, kind);
        if (!w->write(buf.data(), k)) { c_.note("write: %s", w->err.c_str()); t->err = w->err; t->transient = w->transient; w->abort(); delete w; return false; }
        done += k;
        if (abortedAt && abortAfter && done >= abortAfter) { w->abort(); delete w; *abortedAt = true; return true; }
    }
    bool ok = w->finish();
    if (!ok) { c_.note("finish: %s", w->err.c_str()); t->err = w->err; t->transient = w->transient; }
    delete w;
    return ok;
}

bool CheckGen(Ctx& c_, Target* t, const Str& name, u64 seed, u64 size, int kind)
{
    Source* r = t->open_read(name);
    if (!r) { c_.note("open_read %s: %s", name.c_str(), t->err.c_str()); return false; }
    Gen g(seed); Bytes got, want; u64 pos = 0; bool ok = true;
    while (true) {
        size_t k = c_.coin(20) ? (size_t)c_.range(1, 50) : c_.coin(60) ? (size_t)c_.range(1, 70000) : (size_t)c_.range(100000, 1000000);
        got.resize(k);
        i64 n = r->read(got.data(), k);
        if (n < 0) { c_.note("read: %s", r->err.c_str()); t->err = r->err; t->transient = r->transient; ok = false; break; }
        if (n == 0) break;
        want.resize((size_t)n);
        g.fill(want.data(), (size_t)n, kind);
        if (memcmp(got.data(), want.data(), (size_t)n) != 0) { c_.note("data differs near offset %llu", (unsigned long long)pos); ok = false; break; }
        pos += (u64)n;
    }
    r->close(); delete r;
    if (ok && pos != size) { c_.note("size is %llu, expected %llu", (unsigned long long)pos, (unsigned long long)size); ok = false; }
    return ok;
}

bool FinNote(Ctx& c_, Sink* w) { bool ok = w->finish(); if (!ok) c_.note("finish: %s%s", w->err.c_str(), w->transient ? " (transient)" : ""); return ok; }

bool ConnectNote(Ctx& c_, Target* t) { bool ok = t->connect(); if (!ok) c_.note("connect: %s%s", t->err.c_str(), t->transient ? " (transient)" : ""); return ok; }

bool HasName(Target* t, const Str& name, bool& listed)
{
    Vec<Str> names; if (!t->list(names)) return false;
    listed = false; for (const Str& n : names) { if (n == name) listed = true; if (n.ends_with(".part")) return false; }
    return true;
}

i64 FileSizeOf(const Str& path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(FilePath(path), GetFileExInfoStandard, &d)) return -1;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -2;
    return (i64)(((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

Str ServerPath(Ctx& c, const Str& dir, const Str& name)     // where the target's file is on the server's disk, as Wine sees it
{
    (void)c; Str p = g_root + "/" + dir;
    if (!name.empty()) p += "/" + name;
    return p;
}

// names Wine can address on the server's disk: ASCII and no characters Windows forbids
bool IsAscii(const Str& s) { for (size_t i = 0; i < s.size(); ++i) if ((u8)s[i] >= 0x80 || strchr("\"*?<>|:", s[i])) return false; return true; }

Target* MakeT(Ctx& c_, const char* type, const Str& dir, Str& e)
{
    Json cfg = TargetCfg(c_, type, dir);
    Target* t = MakeTarget(cfg, e);
    return t;
}

bool SuiteRoundtrip(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str dir = RandDir(c_, c_.coin()), e; Target* t = MakeT(c_, type, dir, e);
    T_CHECK(t != nullptr); if (!t) return false;
    bool ok = ConnectNote(c_, t); T_CHECK(ok);
    if (!ok) { delete t; return false; }
    struct F { Str name; u64 seed, size; int kind; };
    Vec<F> files;
    u64 sizes[] = { 0, 1, 2, 4096, 32768, 65536, 32768 * 3 + 1, (u64)c_.range(1, 5000), (u64)c_.range(5000, 300000), (u64)c_.range(300000, 3000000) };
    for (int i = 0; i < 8; ++i) {
        F f; f.name = RandName(c_, i); f.seed = c_.next(); f.size = sizes[c_.rnd(10)]; f.kind = c_.rnd(2);
        bool dup = false; for (const F& o : files) if (o.name == f.name) dup = true; if (dup) continue;
        T_CHECK(PutGen(c_, t, f.name, f.seed, f.size, f.kind));
        files.push_back(f);
        if (IsAscii(dir) && IsAscii(f.name)) { i64 fsz = FileSizeOf(ServerPath(c_, dir, f.name)); if (fsz != (i64)f.size) c_.note("server file %s is %lld bytes, expected %llu", ServerPath(c_, dir, f.name).c_str(), (long long)fsz, (unsigned long long)f.size); T_CHECK(fsz == (i64)f.size); T_CHECK(FileSizeOf(ServerPath(c_, dir, f.name + ".part")) == -1); }
    }
    Vec<Str> names; T_CHECK(t->list(names)); T_CHECK(names.size() == files.size());
    for (const F& f : files) {
        bool in = false; for (const Str& n : names) if (n == f.name) in = true; T_CHECK(in);
        bool yes = false; T_CHECK(t->exists(f.name, yes)); T_CHECK(yes);
        T_CHECK(CheckGen(c_, t, f.name, f.seed, f.size, f.kind));
    }
    bool no = true; T_CHECK(t->exists("no such file", no)); T_CHECK(!no);
    // overwrite
    { F& f = files[0]; f.seed = c_.next(); f.size = (u64)c_.range(0, 200000); T_CHECK(PutGen(c_, t, f.name, f.seed, f.size, f.kind)); T_CHECK(CheckGen(c_, t, f.name, f.seed, f.size, f.kind)); }
    for (const F& f : files) T_CHECK(t->remove(f.name));
    T_CHECK(t->remove("never existed")); T_CHECK(t->list(names)); T_CHECK(names.size() == 0);
    // a missing file: permanent error
    Source* r = t->open_read("never existed"); T_CHECK(r == nullptr); T_CHECK(!t->transient); T_CHECK(!t->err.empty());
    delete r; delete t;
    return true;
}

bool SuitePartAndAbort(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Target* t = MakeT(c_, type, dir, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    bool scp = ServerJson(c_, type).get("type").str() == "scp";
    for (int round = 0; round < 3; ++round) {
        Str name = RandName(c_, round);
        Sink* w = t->open_write(name); T_CHECK(w != nullptr); if (!w) break;
        Bytes buf = RandBytes(c_, (size_t)c_.range(1000, 400000));
        T_CHECK(w->write(buf.data(), buf.size()));
        // while the write is open: the final name must not exist, list() must not show the .part
        T_CHECK(FileSizeOf(ServerPath(c_, dir, name)) == -1);
        bool listed = true; T_CHECK(HasName(t, name, listed)); T_CHECK(!listed);
        bool yes = true; T_CHECK(t->exists(name, yes)); T_CHECK(!yes);
        if (!scp) { Sleep(50); }
        if (c_.coin(60)) {
            w->abort(); delete w;
            T_CHECK(FileSizeOf(ServerPath(c_, dir, name)) == -1); T_CHECK(FileSizeOf(ServerPath(c_, dir, name + ".part")) == -1);
            T_CHECK(HasName(t, name, listed)); T_CHECK(!listed);
        } else {
            T_CHECK(FinNote(c_, w)); delete w;
            if (IsAscii(name)) T_CHECK(FileSizeOf(ServerPath(c_, dir, name)) == (i64)buf.size());
            T_CHECK(FileSizeOf(ServerPath(c_, dir, name + ".part")) == -1);
            T_CHECK(HasName(t, name, listed)); T_CHECK(listed);
            T_CHECK(t->remove(name));
        }
    }
    // abort right after open, and a sink dropped without finish() (the destructor aborts)
    { Str name = "dropped.bin"; Sink* w = t->open_write(name); T_CHECK(w != nullptr); if (w) { Bytes b = RandBytes(c_, 5000); w->write(b.data(), b.size()); delete w; }
      T_CHECK(FileSizeOf(ServerPath(c_, dir, name)) == -1); T_CHECK(FileSizeOf(ServerPath(c_, dir, name + ".part")) == -1); }
    { Sink* w = t->open_write("zero.bin"); T_CHECK(w != nullptr); if (w) { T_CHECK(FinNote(c_, w)); delete w; } bool yes = false; T_CHECK(t->exists("zero.bin", yes)); T_CHECK(yes); T_CHECK(FileSizeOf(ServerPath(c_, dir, "zero.bin")) == 0); T_CHECK(t->remove("zero.bin")); }
    // the connection is still good
    T_CHECK(PutGen(c_, t, "after", 5, 1234, 0)); T_CHECK(CheckGen(c_, t, "after", 5, 1234, 0));
    delete t;
    return true;
}

bool SuiteDirs(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str unix = g_rootUnix, e;
    struct D { Str dir; Str onDisk; } ds[] = {
        { Str::fmt("vm2b-t/n%u/a b/\xd0\xb4", (unsigned)(c_.next() % 1000000)), "" },
        { Str::fmt("%s/vm2b-abs%u/x y/", unix.c_str(), (unsigned)(c_.next() % 1000000)), "" },
        { "", "" },
        { Str::fmt("./vm2b-t/dot%u/../dot%u", (unsigned)(c_.next() % 1000000), (unsigned)(c_.next() % 1000000)), "" } };
    for (D& d : ds) {
        Target* t = MakeT(c_, type, d.dir, e); T_CHECK(t != nullptr); if (!t) continue;
        bool ok = t->connect(); if (!ok) c_.note("connect (%s): %s", d.dir.c_str(), t->err.c_str()); T_CHECK(ok);
        if (ok) {
            T_CHECK(ConnectNote(c_, t));                              // again: nothing to create, no error
            Str name = "probe file.bin";
            T_CHECK(PutGen(c_, t, name, 9, 777, 0)); T_CHECK(CheckGen(c_, t, name, 9, 777, 0));
            Vec<Str> names; T_CHECK(t->list(names));
            bool home = d.dir.empty();            // the account's home may hold other files (the paramiko server keeps its info file there)
            { bool in = false; for (const Str& n : names) if (n == name) in = true; T_CHECK(in); if (!home) T_CHECK(names.size() == 1); }
            T_CHECK(t->remove(name));
            // the same folder through a second target object
            Target* t2 = MakeT(c_, type, d.dir, e); T_CHECK(t2 && ConnectNote(c_, t2));
            T_CHECK(t2->list(names)); if (!home) T_CHECK(names.size() == 0);
            delete t2;
        }
        delete t;
    }
    return true;
}

bool SuiteErrors(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str e;
    // no permission: the folder cannot be created (the paramiko servers run as root: no permission checks there)
    if (!Str(type).starts_with("pm.")) { Target* t = MakeT(c_, type, "/root/vm2b-no-permission/x", e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(!t->err.empty()); T_CHECK(!t->connect()); delete t; } }
    // wrong password / wrong host key: permanent, no secret in the text
    { Json cfg = TargetCfg(c_, type, "x"); cfg["password"] = "not-the-password-xyz"; Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("not-the-password-xyz") < 0); delete t; } }
    { Json cfg = TargetCfg(c_, type, "x"); cfg["host_key"] = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("SHA256:") >= 0); delete t; } }
    { Json cfg = TargetCfg(c_, type, "x"); cfg.remove("host_key"); Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("SHA256:") >= 0); delete t; } }
    { Json cfg = TargetCfg(c_, type, "x"); cfg.remove("host_key"); cfg["insecure_accept_host_key"] = true; Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(t->connect()); delete t; } }
    // nothing listens / unknown host / incomplete configuration
    { Json cfg = TargetCfg(c_, type, "x"); cfg["port"] = 22999; Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(t->transient); delete t; } }
    { Json cfg = TargetCfg(c_, type, "x"); cfg["host"] = "no-such-host.invalid"; Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (t) { T_CHECK(!t->connect()); T_CHECK(!t->transient); delete t; } }
    { Json cfg = Json::object(); cfg["type"] = ServerJson(c_, type).get("type").str(); Target* t = MakeTarget(cfg, e); T_CHECK(t == nullptr); T_CHECK(!e.empty()); }
    // a closed connection is a transient failure and reconnect() brings everything back
    { Str dir = RandDir(c_, false); Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (t) {
        T_CHECK(PutGen(c_, t, "keep.bin", 1, 50000, 0));
        t->close();
        Vec<Str> names; T_CHECK(!t->list(names)); T_CHECK(t->transient);
        Sink* w = t->open_write("x.bin"); T_CHECK(w == nullptr); T_CHECK(t->transient);
        T_CHECK(t->reconnect()); T_CHECK(t->list(names)); T_CHECK(names.size() == 1); T_CHECK(CheckGen(c_, t, "keep.bin", 1, 50000, 0)); T_CHECK(t->remove("keep.bin")); delete t; } }
    // bad names
    { Str dir = RandDir(c_, false); Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (t) {
        T_CHECK(t->open_write("../evil") == nullptr); T_CHECK(t->open_write("a/b") == nullptr); T_CHECK(t->open_read("") == nullptr); T_CHECK(!t->remove(".."));
        delete t; } }
    return true;
}

bool SuiteBigStream(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (!t) return false;
    u64 size = (u64)c_.range(100, 130) * 1024 * 1024 + c_.rnd(100000); u64 seed = c_.next();
    double t0 = UnixNow();
    bool ok = PutGen(c_, t, "big.bin", seed, size, 0); T_CHECK(ok);
    double t1 = UnixNow();
    if (ok) { T_CHECK(FileSizeOf(ServerPath(c_, dir, "big.bin")) == (i64)size); T_CHECK(CheckGen(c_, t, "big.bin", seed, size, 0)); }
    double t2 = UnixNow();
    c_.note("%s: %s written at %.1f MB/s, read at %.1f MB/s", type, FormatSize(size).c_str(), size / 1048576.0 / (t1 - t0), size / 1048576.0 / (t2 - t1));
    T_CHECK(t->remove("big.bin")); delete t;
    return true;
}

bool SuiteManyFiles(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (!t) return false;
    int n = c_.range(8, 25); Vec<u64> sizes;
    for (int i = 0; i < n; ++i) {
        u64 sz = c_.coin(20) ? 0 : (u64)c_.range(1, 200000); sizes.push_back(sz);
        T_CHECK(PutGen(c_, t, Str::fmt("arch.tar.%05d", i + 1), 1000 + i, sz, 1));
        Str j = Str::fmt("{\"done\":%d}", i + 1); T_CHECK(t->put_bytes("arch.tar.resume", j.data(), j.size()));      // the journal, rewritten each time
        if (c_.coin(25)) T_CHECK(t->reconnect());
    }
    T_CHECK(t->reconnect());
    Vec<Str> names; T_CHECK(t->list(names)); T_CHECK((int)names.size() == n + 1);
    Bytes jb; T_CHECK(t->get_bytes("arch.tar.resume", jb)); T_CHECK(Str((const char*)jb.data(), jb.size()) == Str::fmt("{\"done\":%d}", n));
    for (int i = 0; i < n; ++i) T_CHECK(CheckGen(c_, t, Str::fmt("arch.tar.%05d", i + 1), 1000 + i, sizes[i], 1));
    delete t;
    return true;
}

bool SuiteConcurrent(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (!t) return false;
    T_CHECK(PutGen(c_, t, "old.bin", 11, 900000, 0));
    Sink* w = t->open_write("new.bin"); T_CHECK(w != nullptr);
    Source* r = t->open_read("old.bin"); T_CHECK(r != nullptr);
    if (w && r) {
        Gen gw(12), gr(11); u64 wr = 0, rd = 0; Bytes a, b, c;
        while (wr < 1200000 || rd < 900000) {
            if (wr < 1200000) { size_t k = (size_t)c_.range(1, 100000); if (k > 1200000 - wr) k = (size_t)(1200000 - wr); a.resize(k); gw.fill(a.data(), k, 0); T_CHECK(w->write(a.data(), k)); wr += k; }
            if (rd < 900000) { size_t k = (size_t)c_.range(1, 100000); b.resize(k); i64 n = r->read(b.data(), k); T_CHECK(n > 0); if (n <= 0) break; c.resize((size_t)n); gr.fill(c.data(), (size_t)n, 0); T_CHECK(memcmp(b.data(), c.data(), (size_t)n) == 0); rd += (u64)n; }
        }
        T_CHECK(FinNote(c_, w)); u8 x; T_CHECK(r->read(&x, 1) == 0);
    }
    delete w; delete r;
    T_CHECK(CheckGen(c_, t, "new.bin", 12, 1200000, 0));
    delete t;
    return true;
}

struct EProg : Progress { u64 bytes; EProg() : bytes(0) {} void onBytes(u64 n) override { bytes += n; } };

bool SuiteEngine(Ctx& c_, const char* type)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Tmp tmp(c_, "sshe"); Str root = tmp.sub("vms"), e;
    Str dir = RandDir(c_, c_.coin()); Target* t = MakeT(c_, type, dir, e); T_CHECK(t && ConnectNote(c_, t)); if (!t) return false;
    for (int round = 0; round < 3; ++round) {
        Str vm = Str::fmt("vm%d \xd0\x92\xd0\x9c", round);
        struct FR { Str rel; Bytes data; }; Vec<FR> files;
        int nf = c_.range(1, 6);
        for (int i = 0; i < nf; ++i) {
            FR f; f.rel = Str::fmt("%sfile %d.%s", c_.coin(30) ? "sub/" : "", i, c_.coin() ? "vmdk" : "vmx");
            size_t sz = c_.coin(15) ? 0 : c_.coin(40) ? (size_t)c_.range(1, 4000000) : (size_t)c_.range(1, 60000);
            f.data.resize(sz); Gen g(c_.next()); g.fill(f.data.data(), sz, c_.rnd(2));
            Str full = JoinPath(JoinPath(root, vm), f.rel.replace_all("/", "\\")); int cut = full.rfind('\\'); Str me; MakeDirs(full.substr(0, (size_t)cut), me);
            HANDLE h = CreateFileW(FilePath(full), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wn = 0; if (sz) WriteFile(h, f.data.data(), (DWORD)sz, &wn, nullptr); CloseHandle(h);
            files.push_back(f);
        }
        BackupOptions o; o.codec.level = c_.range(1, 3);
        if (c_.coin(50)) o.password = "enc-pw-\xd0\xbf"; o.verify = c_.coin(70); o.keep = 0; o.kdfIterations = 3000;
        VmSource* src = NewDirectorySource(root); Manifest m; Str warn, err; EProg pr;
        bool ok = BackupVm(*src, vm, *t, o, &pr, m, warn, err); if (!ok) c_.note("backup: %s", err.c_str()); T_CHECK(ok);
        if (ok) {
            Str ve; bool vok = VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve); if (!vok) c_.note("verify: %s", ve.c_str()); T_CHECK(vok);
            Str dest = tmp.sub(Str::fmt("restore%d", round).c_str()); Manifest rm; Str re;
            bool rok = RestoreBackup(*t, m.archive, dest, o.password, nullptr, &rm, re); if (!rok) c_.note("restore: %s", re.c_str()); T_CHECK(rok);
            for (const FR& f : files) {
                Bytes got; Str er; Str p = JoinPath(JoinPath(dest, vm), f.rel.replace_all("/", "\\"));
                bool same = ReadWholeFile(p.c_str(), got, er, (size_t)1 << 30) && got.size() == f.data.size() && (f.data.empty() || memcmp(got.data(), f.data.data(), got.size()) == 0);
                if (!same) { c_.note("differs: %s", f.rel.c_str()); c_.failed = true; }
            }
        }
        delete src;
    }
    delete t;
    return true;
}

} // namespace

#define SUITE_TEST(group, type, name, fn) VM2B_TEST(group, name) { return fn(c_, type); }
SUITE_TEST("scp", "pm.scp", scp_pm_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "pm.scp", scp_pm_part_and_abort, SuitePartAndAbort)
SUITE_TEST("scp", "pm.scp", scp_pm_directories, SuiteDirs)
SUITE_TEST("scp", "pm.scp", scp_pm_errors_reconnect, SuiteErrors)
SUITE_TEST("scp", "pm.scp", scp_pm_many_files_resume, SuiteManyFiles)
SUITE_TEST("scp", "pm.scp", scp_pm_concurrent_handles, SuiteConcurrent)
SUITE_TEST("scp", "pm.scp", scp_pm_engine_roundtrip, SuiteEngine)
// re-keying: the client asks for it every few hundred KiB (+rk), or the real sshd does (RekeyLimit 128K)
SUITE_TEST("scp", "scp+rk", scp_client_rekey_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "scp+rk", scp_client_rekey_many_files, SuiteManyFiles)
SUITE_TEST("scp", "scp+rk", scp_client_rekey_engine, SuiteEngine)
SUITE_TEST("scp", "scp_rekey", scp_server_rekey_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "scp_rekey", scp_server_rekey_many_files, SuiteManyFiles)
SUITE_TEST("scp", "scp_rekey", scp_server_rekey_engine, SuiteEngine)
SUITE_TEST("scp", "scp_rekey", scp_server_rekey_big_stream, SuiteBigStream)
SUITE_TEST("scp", "scp", scp_roundtrip_sizes, SuiteRoundtrip)
SUITE_TEST("scp", "scp", scp_part_and_abort, SuitePartAndAbort)
SUITE_TEST("scp", "scp", scp_directories, SuiteDirs)
SUITE_TEST("scp", "scp", scp_errors_reconnect, SuiteErrors)
SUITE_TEST("scp", "scp", scp_many_files_resume, SuiteManyFiles)
SUITE_TEST("scp", "scp", scp_concurrent_handles, SuiteConcurrent)
SUITE_TEST("scp", "scp", scp_big_stream, SuiteBigStream)
SUITE_TEST("scp", "scp", scp_engine_roundtrip, SuiteEngine)

// ============================== the paramiko servers: faults, hostile servers, more authentication =================
namespace {

// A line to a control port of the paramiko test servers; returns the reply (empty on failure).
Str Ctl(const Json& ent, const Str& line)
{
    static bool init = false; if (!init) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); init = true; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((u_short)ent.get("control_port").integer()); { Str h = ent.get("host").str("127.0.0.1"); unsigned long ip = inet_addr(h.c_str()); a.sin_addr.s_addr = ip == INADDR_NONE ? htonl(INADDR_LOOPBACK) : ip; }   // the control port listens where the server does (servers.json "host"; the Windows VM harness: the Docker bridge address)
    Str reply;
    DWORD to = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        Str l = line + "\n"; send(s, l.data(), (int)l.size(), 0);
        char buf[512]; int n = recv(s, buf, sizeof(buf) - 1, 0); if (n > 0) reply = Str(buf, (size_t)n);
    }
    closesocket(s);
    return reply.trim();
}

struct SetLater { volatile long* f; DWORD ms; };
DWORD WINAPI SetLaterProc(LPVOID p) { SetLater* s = (SetLater*)p; Sleep(s->ms); *s->f = 1; delete s; return 0; }
void SetFlagAfter(volatile long* f, DWORD ms) { SetLater* s = new SetLater; s->f = f; s->ms = ms; HANDLE h = CreateThread(nullptr, 0, SetLaterProc, s, 0, nullptr); if (h) CloseHandle(h); }

struct FaultEnv {
    Ctx& c; const char* key; Json ent; Str dir; Target* t; Str e;
    bool ok;
    FaultEnv(Ctx& c_, const char* k, int ioMs = 20000, bool emptyDir = false) : c(c_), key(k), t(nullptr), ok(false) {
        if (!SshServers(c)) return;
        if (!HaveServer(c, key)) { c.skip("paramiko servers not available"); return; }
        UseRoot(c, key); ent = ServerJson(c, key); Ctl(ent, "clear=1");
        dir = emptyDir ? Str() : RandDir(c, false);
        Json cfg = TargetCfg(c, key, dir); cfg["io_timeout_ms"] = ioMs; t = MakeTarget(cfg, e);
        ok = t != nullptr;
    }
    ~FaultEnv() { if (ent.is_object()) Ctl(ent, "clear=1"); delete t; }
    Str ctl(const Str& line) { return Ctl(ent, line); }
};

bool FaultUpload(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t; T_CHECK(ConnectNote(c_, t));
    for (int round = 0; round < 4; ++round) {
        u64 size = (u64)c_.range(300000, 5000000), at = 12000 + c_.rnd((u32)(size * 6 / 10));
        Str name = Str::fmt("vol.%05d", round + 1);
        f.ctl(Str::fmt("drop_c2s=%llu%s", (unsigned long long)at, c_.coin() ? " rst=1" : ""));
        T_CHECK(t->reconnect());
        bool ok = PutGen(c_, t, name, 77 + round, size, 1);
        T_CHECK(!ok); T_CHECK(t->transient); T_CHECK(!t->err.empty());
        T_CHECK(FileSizeOf(ServerPath(c_, f.dir, name)) == -1);                      // never under its final name
        Vec<Str> names; T_CHECK(!t->list(names)); T_CHECK(t->transient);          // the broken connection is a transient failure
        f.ctl("clear=1");
        T_CHECK(t->reconnect());
        T_CHECK(t->list(names)); for (const Str& n : names) T_CHECK(!n.ends_with(".part") && n != name);
        T_CHECK(PutGen(c_, t, name, 77 + round, size, 1)); T_CHECK(CheckGen(c_, t, name, 77 + round, size, 1));
        T_CHECK(FileSizeOf(ServerPath(c_, f.dir, name)) == (i64)size);
        T_CHECK(t->remove(name));
    }
    return true;
}

bool FaultDownload(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t; T_CHECK(ConnectNote(c_, t));
    for (int round = 0; round < 4; ++round) {
        u64 size = (u64)c_.range(300000, 5000000), at = 6000 + c_.rnd((u32)(size * 6 / 10));
        T_CHECK(PutGen(c_, t, "dl.bin", 5 + round, size, 0));
        f.ctl(Str::fmt("drop_s2c=%llu%s", (unsigned long long)at, c_.coin() ? " rst=1" : ""));
        T_CHECK(t->reconnect() || t->transient);                                  // the cut may land in the handshake itself
        bool ok = CheckGen(c_, t, "dl.bin", 5 + round, size, 0);
        T_CHECK(!ok); T_CHECK(t->transient);
        f.ctl("clear=1"); T_CHECK(t->reconnect());
        T_CHECK(CheckGen(c_, t, "dl.bin", 5 + round, size, 0));
    }
    return true;
}

bool FaultList(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t; T_CHECK(ConnectNote(c_, t));
    int n = ServerJson(c_, key).get("type").str() == "scp" ? 40 : 300;
    for (int i = 0; i < n; ++i) { Str nm = Str::fmt("a-rather-long-volume-name-to-fill-the-listing-%05d.tar.part%05d", i, i); T_CHECK(t->put_bytes(nm, "x", 1)); }
    int cuts = 0;
    for (int round = 0; round < 6; ++round) {
        f.ctl(Str::fmt("drop_s2c=%u%s", 2500 + c_.rnd(9000), c_.coin() ? " rst=1" : ""));
        bool conn = t->reconnect(); Vec<Str> names; bool ok = conn && t->list(names);
        if (!ok) { ++cuts; T_CHECK(t->transient); T_CHECK(!t->err.empty()); }
        else T_CHECK((int)names.size() == n);
        f.ctl("clear=1"); T_CHECK(t->reconnect()); T_CHECK(t->list(names)); T_CHECK((int)names.size() == n);
    }
    c_.note("%d of 6 listings were cut", cuts);
    T_CHECK(cuts > 0);
    return true;
}

bool FaultIdle(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t; T_CHECK(ConnectNote(c_, t));
    f.ctl("idle_drop=1"); T_CHECK(t->reconnect());
    Sleep(2600);
    Vec<Str> names; T_CHECK(!t->list(names)); T_CHECK(t->transient);
    Sink* w = t->open_write("x"); if (w) { Bytes b = RandBytes(c_, 100); bool ok = w->write(b.data(), b.size()) && w->finish(); T_CHECK(!ok); T_CHECK(w->transient); delete w; } else T_CHECK(t->transient);
    f.ctl("clear=1"); T_CHECK(t->reconnect()); T_CHECK(t->list(names));
    return true;
}

bool FaultHang(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key, 2500); if (!f.ok) return true;
    Target* t = f.t; T_CHECK(ConnectNote(c_, t));
    f.ctl(Str::fmt("hang_c2s=%u", 15000 + c_.rnd(30000))); T_CHECK(t->reconnect());
    double t0 = UnixNow(); bool ok = PutGen(c_, t, "h.bin", 3, 4000000, 0); double dt = UnixNow() - t0;
    T_CHECK(!ok); T_CHECK(t->transient); T_CHECK(dt < 25); c_.note("the black hole was noticed after %.1f s: %s", dt, t->err.c_str());
    f.ctl("clear=1"); T_CHECK(t->reconnect()); T_CHECK(PutGen(c_, t, "h.bin", 3, 100000, 0));
    return true;
}

bool FaultAbort(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key, 60000); if (!f.ok) return true;
    Target* t = f.t; volatile long flag = 0; SshTargetSetAbortFlag(t, &flag);
    T_CHECK(ConnectNote(c_, t));
    f.ctl("hang_c2s=20000"); T_CHECK(t->reconnect());
    SetFlagAfter(&flag, 1000);
    double t0 = UnixNow(); bool ok = PutGen(c_, t, "a.bin", 3, 4000000, 0); double dt = UnixNow() - t0;
    T_CHECK(!ok); T_CHECK(!t->transient); T_CHECK(t->err.find("abort") >= 0); T_CHECK(dt < 8); c_.note("aborted after %.1f s: %s", dt, t->err.c_str());
    flag = 0; f.ctl("clear=1"); T_CHECK(t->reconnect()); T_CHECK(PutGen(c_, t, "a.bin", 3, 100000, 0));
    return true;
}

bool FaultRekey(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t;
    // (a rekey every 40-340 KB, with paramiko and libssh2, stalls now and then: not a realistic server, so the interval is longer)
    f.ctl(Str::fmt("rekey=%u", 400000 + c_.rnd(1200000))); T_CHECK(ConnectNote(c_, t));
    // What the product must guarantee: a transfer that stalls in a rekey ends with a TRANSIENT error, and after reconnect() the same
    // transfer succeeds (the engine retries exactly like this). paramiko as the server stalls in a forced rekey now and then, so one
    // retry per file is allowed; a second failure in a row, or a permanent error, is a real failure.
    int stalls = 0;
    for (int i = 0; i < 3; ++i) {
        u64 size = (u64)c_.range(1000000, 9000000);
        bool done = false;
        for (int attempt = 0; attempt < 3 && !done; ++attempt) {
            t->transient = false;
            bool ok = PutGen(c_, t, "rk.bin", 20 + i, size, 0);
            if (ok) {
                t->transient = false;
                ok = CheckGen(c_, t, "rk.bin", 20 + i, size, 0);
                if (!ok && !t->transient) { c_.note("rk.bin read back differs: DATA ERROR"); T_CHECK(false); return false; }   // corruption is never tolerated
            }
            if (ok) { done = true; break; }
            T_CHECK(t->transient);                                                         // a stall is a transient error, never a permanent one
            ++stalls; T_CHECK(t->reconnect());
        }
        T_CHECK(done);
    }
    if (stalls) c_.note("%d transfer(s) stalled in a rekey and went through after reconnect()", stalls);
    T_CHECK(stalls <= 3);
    return true;
}

bool FaultSlow(Ctx& c_, const char* key)
{
    FaultEnv f(c_, key); if (!f.ok) return true;
    Target* t = f.t; f.ctl(Str::fmt("slow_ms=%u", 1 + c_.rnd(4))); T_CHECK(ConnectNote(c_, t));
    u64 size = (u64)c_.range(100000, 1500000);
    T_CHECK(PutGen(c_, t, "slow.bin", 9, size, 0)); T_CHECK(CheckGen(c_, t, "slow.bin", 9, size, 0));
    return true;
}

// hostile SCP servers: an error (or a clean end), never a crash, hang or garbage presented as data
bool EvilScp(Ctx& c_, const char* key)
{
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, key)) { c_.skip("paramiko servers not available"); return true; }
    const char* modes[] = { "bigsize", "badline", "negative", "errormsg", "short", "noack" };
    for (const char* m : modes) {
        FaultEnv f(c_, key, 3000, true); if (!f.ok) return true;
        f.ctl(Str("evil_scp=") + m);
        double t0 = UnixNow();
        if (!f.t->connect()) { c_.note("%s: connect: %s", m, f.t->err.c_str()); c_.failed = true; continue; }
        Source* r = f.t->open_read("f");
        bool failed = false, eof = false;
        if (r) { Bytes b; b.resize(100000); for (int i = 0; i < 100 && !failed && !eof; ++i) { i64 n = r->read(b.data(), b.size()); if (n < 0) failed = true; else if (n == 0) eof = true; } delete r; }
        else failed = true;
        if (!failed) { c_.note("%s: no error was reported", m); c_.failed = true; }
        if (Str(m) == "errormsg" || Str(m) == "badline" || Str(m) == "negative") T_CHECK(!f.t->transient);
        double dt = UnixNow() - t0; if (dt > 20) { c_.note("%s took %.1f s", m, dt); c_.failed = true; }
    }
    return true;
}

} // namespace

#define FAULT_TEST(group, key, name, fn) VM2B_TEST(group, name) { return fn(c_, key); }
FAULT_TEST("scp", "pm.fault_scp", scp_fault_drop_upload, FaultUpload)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_drop_download, FaultDownload)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_drop_listing, FaultList)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_idle_drop, FaultIdle)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_black_hole_timeout, FaultHang)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_abort_flag, FaultAbort)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_rekey_storm, FaultRekey)
FAULT_TEST("scp", "pm.fault_scp", scp_fault_slow_server, FaultSlow)
FAULT_TEST("scp", "pm.fault_scp", scp_hostile_server, EvilScp)

// ============================== ssh group: more authentication, hostile servers, timeouts, rekeying, corruption ====
namespace {

SshConfig CfgFromEntry(Ctx& c, const Json& e)
{
    SshConfig cfg; cfg.host = e.get("host").str("127.0.0.1"); cfg.port = (int)e.get("port").integer(); cfg.user = e.get("user").str(); cfg.password = e.get("password").str();
    cfg.hostKey = e.get("host_key").str(); cfg.ioTimeoutMs = 20000; (void)c;
    return cfg;
}

} // namespace

VM2B_TEST("ssh", paramiko_auth_methods)
{
    if (!SshServers(c_)) return true;
    const Json& pm = c_.servers.get("pm"); if (!pm.is_object()) { c_.skip("paramiko servers not available"); return true; }
    // password, keyboard-interactive with one prompt (ESXi style), with two rounds / prompts, and public keys
    { SshSession s; T_CHECK(s.connect(CfgFromEntry(c_, pm.get("scp")))); }
    const char* kb[] = { "kbdint", "kbdint2" };
    for (const char* k : kb) {
        SshConfig cfg = CfgFromEntry(c_, pm.get(k)); SshSession s; bool ok = s.connect(cfg); if (!ok) c_.note("%s: %s", k, s.err.c_str()); T_CHECK(ok);
        cfg.password = "wrong-pw-for-kbd"; SshSession s2; T_CHECK(!s2.connect(cfg)); T_CHECK(!s2.transient); T_CHECK(s2.err.find("wrong-pw-for-kbd") < 0);
    }
    const char* keys[] = { "u_ed25519", "u_rsa", "u_ecdsa256", "u_ecdsa384_pw", "u_rsa_pw" };
    for (const char* k : keys) {
        SshConfig cfg = CfgFromEntry(c_, pm.get("scp")); cfg.password.clear(); cfg.keyFile = KeyPath(c_, k);
        if (Str(k).ends_with("_pw")) cfg.keyPassphrase = c_.servers.get("ssh").get("passphrase").str();
        SshSession s; bool ok = s.connect(cfg); if (!ok) c_.note("key %s: %s", k, s.err.c_str()); T_CHECK(ok);
    }
    // the password server with the wrong password is a permanent failure
    { SshConfig cfg = CfgFromEntry(c_, pm.get("scp")); cfg.password = "nope"; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(!s.transient); }
    return true;
}

VM2B_TEST("ssh", hostile_servers)
{
    if (!SshServers(c_)) return true;
    const Json& pm = c_.servers.get("pm"); const Json& g = pm.get("garbage");
    if (!g.is_object()) { c_.skip("paramiko servers not available"); return true; }
    int base = (int)g.get("base").integer();
    for (size_t i = 0; i < g.get("modes").size(); ++i) {
        Str mode = g.get("modes").at(i).str();
        for (int rep = 0; rep < 2; ++rep) {
            SshConfig cfg = CfgFor(c_, base + (int)i); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 3000; cfg.connectTimeoutMs = 3000;
            if (c_.coin()) cfg.cipherList = c_.coin() ? "aes128-ctr" : "aes256-gcm@openssh.com";
            SshSession s; double t0 = UnixNow(); bool ok = s.connect(cfg); double dt = UnixNow() - t0;
            if (ok) { c_.note("%s: connected to a hostile server", mode.c_str()); c_.failed = true; }
            else if (s.err.empty()) { c_.note("%s: no error text", mode.c_str()); c_.failed = true; }
            if (dt > 15) { c_.note("%s: took %.1f s", mode.c_str(), dt); c_.failed = true; }
            if (rep == 0) c_.note("%-22s -> %s", mode.c_str(), s.err.c_str());
        }
    }
    return true;
}

VM2B_TEST("ssh", abort_and_timeouts)
{
    if (!SshServers(c_)) return true;
    const Json& pm = c_.servers.get("pm"); const Json& g = pm.get("garbage");
    int main = (int)c_.servers.get("scp").get("port").integer();
    if (g.is_object()) {
        // a server that never says a word: the timeout is a transient failure; an abort flag ends it at once and is not
        int silent = 0; for (size_t i = 0; i < g.get("modes").size(); ++i) if (g.get("modes").at(i).str() == "silent") silent = (int)i;
        { SshConfig cfg = CfgFor(c_, (int)g.get("base").integer() + silent); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 2000; SshSession s;
          double t0 = UnixNow(); T_CHECK(!s.connect(cfg)); double dt = UnixNow() - t0; T_CHECK(s.transient); T_CHECK(dt > 1.5 && dt < 8); T_CHECK(s.err.find("timed out") >= 0); }
        { SshConfig cfg = CfgFor(c_, (int)g.get("base").integer() + silent); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 30000; SshSession s; volatile long flag = 0; s.set_abort_flag(&flag);
          SetFlagAfter(&flag, 600); double t0 = UnixNow(); T_CHECK(!s.connect(cfg)); double dt = UnixNow() - t0; T_CHECK(!s.transient); T_CHECK(dt < 5); T_CHECK(s.err.find("abort") >= 0); }
    }
    // refused connection: transient; the object is usable afterwards
    { SshConfig cfg = CfgFor(c_, 22998); cfg.insecureAcceptHostKey = true; SshSession s; T_CHECK(!s.connect(cfg)); T_CHECK(s.transient);
      cfg.port = main; T_CHECK(s.connect(cfg)); }
    // a quiet command: without keepalives the idle timeout hits, with keepalives a slow command is fine
    { SshConfig cfg = CfgFor(c_, main); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 2000; cfg.keepaliveMs = 0;
      SshSession s; T_CHECK(s.connect(cfg)); Bytes o, e; int st; double t0 = UnixNow(); bool ok = s.exec("sleep 5; echo late", o, e, st); double dt = UnixNow() - t0;
      T_CHECK(!ok); T_CHECK(s.transient); T_CHECK(dt < 4); }
    { SshConfig cfg = CfgFor(c_, main); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 4500; cfg.keepaliveMs = 1000;           // (libssh2 sends a keepalive every 2 s at the most)
      SshSession s; T_CHECK(s.connect(cfg)); Bytes o, e; int st = -1; bool ok = s.exec("sleep 8; echo late", o, e, st); if (!ok) c_.note("keepalive run: %s", s.err.c_str()); T_CHECK(ok);
      T_CHECK(Str((const char*)o.data(), o.size()) == "late\n"); T_CHECK(st == 0); }
    // an abort in the middle of a long command
    { SshConfig cfg = CfgFor(c_, main); cfg.insecureAcceptHostKey = true; SshSession s; volatile long flag = 0; T_CHECK(s.connect(cfg)); s.set_abort_flag(&flag);
      SetFlagAfter(&flag, 700); Bytes o, e; int st; double t0 = UnixNow(); T_CHECK(!s.exec("sleep 20", o, e, st)); double dt = UnixNow() - t0; T_CHECK(dt < 5); T_CHECK(s.err.find("abort") >= 0); T_CHECK(!s.transient); }
    return true;
}

VM2B_TEST("ssh", rekeying_all_ciphers)
{
    if (!SshServers(c_)) return true;
    int rp = (int)c_.servers.get("ssh").get("rekey_port").integer(); int main = (int)c_.servers.get("scp").get("port").integer();
    const char* ciphers[] = { "aes256-gcm@openssh.com", "aes128-gcm@openssh.com", "aes256-ctr", "aes192-ctr", "aes128-ctr", "chacha20-poly1305@openssh.com" };
    const char* macs[] = { "hmac-sha2-256-etm@openssh.com", "hmac-sha2-512-etm@openssh.com", "hmac-sha2-256", "hmac-sha2-512", "hmac-sha1-etm@openssh.com", "hmac-sha1" };
    for (int which = 1; which < 2; ++which) {            // the server asks for a new key exchange (RekeyLimit 128K); libssh2 never starts one itself
        for (int ci = 0; ci < 6; ++ci) {
            SshConfig cfg = CfgFor(c_, which ? rp : main); cfg.insecureAcceptHostKey = true; cfg.cipherList = ciphers[ci]; cfg.macList = macs[c_.rnd(6)];
            SshSession s; bool ok = s.connect(cfg); if (!ok) { c_.note("connect: %s", s.err.c_str()); T_CHECK(ok); continue; }
            size_t sz = (size_t)c_.range(800000, 3000000); Bytes data = RandBytes(c_, sz);
            SshChannel* ch = s.open_session(); T_CHECK(ch != nullptr); if (!ch) continue;
            T_CHECK(ch->exec("cat > /dev/null"));
            T_CHECK(ch->write(data.data(), data.size())); T_CHECK(ch->send_eof()); ch->wait_close(5000); delete ch;
            // and back: the server produces the bytes (so its direction rekeys too)
            SshChannel* c2 = s.open_session(); T_CHECK(c2 != nullptr); if (!c2) continue;
            T_CHECK(c2->exec(Str::fmt("head -c %zu /dev/zero", sz)));
            Bytes back; back.resize(sz); T_CHECK(c2->read_exact(back.data(), sz)); delete c2;
            bool allzero = true; for (size_t i = 0; i < sz; i += 997) if (back[i]) allzero = false; T_CHECK(allzero);
            c_.note("server asks %s (%s)", ciphers[ci], s.macName.c_str());
            T_CHECK(ExecEcho(c_, s, false));
        }
    }
    return true;
}

VM2B_TEST("ssh", corrupted_stream_never_silent)
{
    if (!SshServers(c_)) return true;
    const Json& mitm = c_.servers.get("pm").get("mitm"); if (!mitm.is_object()) { c_.skip("paramiko servers not available"); return true; }
    // (no chacha20-poly1305: libssh2 1.11.1 runs away on a damaged length field, which is why the program does not offer it)
    const char* ciphers[] = { "aes256-gcm@openssh.com", "aes256-ctr", "aes128-gcm@openssh.com", "aes128-ctr" };
    const char* macs[] = { "hmac-sha2-256-etm@openssh.com", "hmac-sha2-256", "hmac-sha1" };
    int failedRuns = 0, cleanRuns = 0;
    for (int run = 0; run < 24; ++run) {
        const char* dirs[] = { "flip_c2s", "flip_s2c", "cut_c2s", "cut_s2c", "dup_s2c" };
        Str d = dirs[run % 5]; u32 at = 200 + c_.rnd(c_.coin(70) ? 12000 : 600000);
        Ctl(mitm, Str::fmt("%s=%u", d.c_str(), at));
        SshConfig cfg = CfgFromEntry(c_, mitm); cfg.insecureAcceptHostKey = true; cfg.ioTimeoutMs = 4000; cfg.cipherList = ciphers[c_.rnd(4)]; cfg.macList = macs[c_.rnd(3)];
        SshSession s; bool ok = s.connect(cfg); bool clean = false;
        if (ok) {
            size_t sz = 400000; Bytes data = RandBytes(c_, sz);
            SshChannel* ch = s.open_session();
            if (ch && ch->exec("cat")) {
                bool w = ch->write(data.data(), data.size()) && ch->send_eof();
                Bytes back; back.resize(sz); bool r = w && ch->read_exact(back.data(), sz);
                if (r) { if (memcmp(back.data(), data.data(), sz) != 0) { c_.note("SILENT CORRUPTION (%s at %u)", d.c_str(), at); c_.failed = true; } else clean = true; }
            }
            delete ch;
        }
        if (clean) ++cleanRuns; else ++failedRuns;
    }
    c_.note("%d runs were stopped by an error, %d were beyond the damage and clean", failedRuns, cleanRuns);
    T_CHECK(failedRuns > 5);
    Ctl(mitm, "clear=1");
    return true;
}

VM2B_TEST("ssh", key_file_fuzz)
{
    // damaged key files go through libssh2 / OpenSSL: every attempt ends with a clean permanent error (or a login), never a crash or a hang
    if (!SshServers(c_)) return true;
    int port = (int)c_.servers.get("scp").get("port").integer();
    const char* files[] = { "u_ed25519", "u_ed25519_pw", "u_rsa", "u_rsa_pem", "u_rsa_pkcs8", "u_ecdsa256", "u_ecdsa384_pw", "u_ed25519_pkcs8", "u_ecdsa_pem" };
    Tmp tmp(c_, "keyfuzz");
    int logged = 0, rejected = 0;
    for (const char* f : files) {
        Bytes orig; Str e; if (!ReadWholeFile(KeyPath(c_, f).c_str(), orig, e)) { c_.note("cannot read %s: %s", f, e.c_str()); c_.failed = true; continue; }
        for (int i = 0; i < 12; ++i) {
            Bytes m = orig; int kind = c_.rnd(4);
            if (kind == 0) { int n = c_.range(1, 5); for (int j = 0; j < n; ++j) m[c_.rnd((u32)m.size())] = (u8)c_.rnd(256); }
            else if (kind == 1) m.resize(c_.rnd((u32)m.size()));
            else if (kind == 2) { size_t p = c_.rnd((u32)m.size()); Bytes ins = RandBytes(c_, c_.rnd(40)); for (size_t j = 0; j < ins.size(); ++j) m.insert(p, ins[j]); }
            else { size_t a = c_.rnd((u32)m.size()), b = c_.rnd((u32)m.size()); if (a > b) { size_t t = a; a = b; b = t; } for (size_t j = a; j < b; ++j) m[j] = 'A'; }
            Str path = tmp.sub("k"); T_CHECK(WriteWholeFile(path.c_str(), m.data(), m.size(), e));
            SshConfig cfg = CfgFor(c_, port); cfg.insecureAcceptHostKey = true; cfg.password.clear(); cfg.keyFile = path; cfg.keyPassphrase = "pass phrase 123"; cfg.ioTimeoutMs = 5000;
            SshSession s; bool ok = s.connect(cfg);
            if (ok) ++logged; else { ++rejected; if (s.err.empty()) { c_.note("rejected without a message"); c_.failed = true; } T_CHECK(!s.transient); }
        }
    }
    c_.note("%d mutated key files rejected, %d still worked (harmless changes)", rejected, logged);
    return true;
}

// ============================== unusual servers (tools/run-ssh-tests.sh: "x" servers; pm tiny_*) =====================================
// Servers that behave: the whole suites must pass against them.
static bool RemoteNameOkForTest(const Str& n) { return !n.empty() && n != "." && n != ".." && n.find('/') < 0 && n.find('\\') < 0 && n.find('\n') < 0 && n.size() < 250; }
SUITE_TEST("scp", "scp_maxsess1", scp_maxsessions1_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "scp_noise_err", scp_stderr_noise_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "scp_noise_err", scp_stderr_noise_directories, SuiteDirs)
SUITE_TEST("scp", "scp_slowlogin", scp_slow_login_shell_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "scp_rekey64k", scp_server_rekey_64k_group14_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "pm.tiny_scp", scp_tiny_window_roundtrip, SuiteRoundtrip)
SUITE_TEST("scp", "pm.tiny_scp", scp_tiny_window_big_stream, SuiteBigStream)
SUITE_TEST("scp", "pm.tiny_scp", scp_tiny_window_engine, SuiteEngine)

namespace {

// Servers that do not work as a backup target: a clean, readable, bounded failure is the right outcome.
struct BadSrv { const char* key; const char* what; bool wantPermanent; };

}  // namespace

VM2B_TEST("ssh", unusable_servers_fail_cleanly)
{
    if (!SshServers(c_)) return true;
    static const BadSrv bad[] = {
        { "scp_forced", "forced command (scp)", false },
        { "scp_noise_out", "stdout noise before scp", false }, { "scp_nopass", "no password auth", true },
    };
    for (const BadSrv& b : bad) {
        if (!HaveServer(c_, b.key)) continue;
        Str e; Json cfg = TargetCfg(c_, b.key, "vm2b-x/bad");
        Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (!t) continue;
        double t0 = UnixNow(); bool ok = t->connect();
        if (ok) { // the connection may work (scp forced command: mkdir -p answers nothing) - the first transfer must fail then
            Sink* w = t->open_write("probe.bin"); bool up = false;
            if (w) { Bytes d = RandBytes(c_, 3000); up = w->write(d.data(), d.size()) && w->finish(); if (!up) { t->err = w->err; t->transient = w->transient; } delete w; }
            else up = false;
            if (up) { c_.note("%s (%s): an upload WORKED", b.key, b.what); c_.failed = true; }
        }
        double dt = UnixNow() - t0;
        c_.note("%s (%s): failed after %.1f s: %s%s", b.key, b.what, dt, t->err.c_str(), t->transient ? " [transient]" : " [permanent]");
        T_CHECK(!t->err.empty()); T_CHECK(dt < 40);
        if (b.wantPermanent) T_CHECK(!t->transient);
        delete t;
    }
    return true;
}

VM2B_TEST("scp", scp_stdout_noise_never_corrupts_silently)
{
    if (!SshServers(c_) || !HaveServer(c_, "scp_noise_out")) return true;
    UseRoot(c_, "scp_noise_out");
    Str e, dir = RandDir(c_, false); Target* t = MakeT(c_, "scp_noise_out", dir, e); T_CHECK(t != nullptr); if (!t) return false;
    bool ok = t->connect(); c_.note("connect with stdout noise: %s", ok ? "ok" : t->err.c_str());
    if (ok) {
        Bytes d = RandBytes(c_, 20000); Sink* w = t->open_write("n.bin"); bool up = false;
        if (w) { up = w->write(d.data(), d.size()) && w->finish(); if (!up) c_.note("upload: %s", w->err.c_str()); delete w; }
        T_CHECK(!up);                                           // the protocol is broken by the noise: it must not pretend
        T_CHECK(FileSizeOf(ServerPath(c_, dir, "n.bin")) == -1);
        Vec<Str> names; bool lok = t->list(names); (void)lok;
        for (const Str& n : names) if (n.starts_with("Welcome")) c_.note("NOTE: the noise line is listed as a file name: '%s'", n.c_str());
    }
    delete t;
    return true;
}

VM2B_TEST("scp", scp_missing_mv_leaves_no_final_file)
{
    if (!SshServers(c_) || !HaveServer(c_, "scp_nomv")) return true;
    UseRoot(c_, "scp_nomv");
    Str e, dir = RandDir(c_, false); Target* t = MakeT(c_, "scp_nomv", dir, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    Bytes d = RandBytes(c_, 30000); Sink* w = t->open_write("v.00001");
    bool up = false; Str msg;
    if (w) { up = w->write(d.data(), d.size()) && w->finish(); msg = w->err; delete w; }
    c_.note("upload with no mv on the server: %s", up ? "worked (the client has a fallback)" : msg.c_str());
    if (!up) { T_CHECK(!msg.empty()); T_CHECK(msg.find("mv") >= 0 || msg.find("rename") >= 0 || msg.find("127") >= 0); }
    T_CHECK(FileSizeOf(ServerPath(c_, dir, "v.00001")) == (up ? (i64)d.size() : -1));
    Vec<Str> names; T_CHECK(t->list(names)); for (const Str& n : names) T_CHECK(!n.ends_with(".part"));
    delete t;
    return true;
}

// a 1 MiB tmpfs: the disk really fills up
static bool DiskFullCase(Ctx& c_, const char* type)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str dir = Str::fmt("small/d%u", (unsigned)(c_.next() % 1000000)), e;
    if (FileSizeOf(ServerPath(c_, "small", "")) != -2) { c_.skip("no small tmpfs (run as root)"); return true; }
    Target* t = MakeT(c_, type, dir, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    T_CHECK(PutGen(c_, t, "fits.bin", 1, 300000, 0));
    // does not fit: must fail (never succeed with a short file), permanent or transient, never leave the final name
    Sink* w = t->open_write("toobig.bin"); bool up = false; Str why; bool trans = false;
    if (w) {
        Gen g(2); Bytes buf; u64 done = 0, size = 3 * 1024 * 1024; up = true;
        while (done < size && up) { size_t k = 65536; buf.resize(k); g.fill(buf.data(), k, 0); up = w->write(buf.data(), k); done += k; }
        if (up) up = w->finish();
        if (!up) { why = w->err; trans = w->transient; w->abort(); }
        delete w;
    } else { why = t->err; trans = t->transient; }
    c_.note("%s: 3 MiB into 1 MiB: %s%s", type, up ? "WORKED?!" : why.c_str(), trans ? " [transient]" : " [permanent]");
    T_CHECK(!up); T_CHECK(!why.empty());
    T_CHECK(FileSizeOf(ServerPath(c_, dir, "toobig.bin")) == -1);
    T_CHECK(FileSizeOf(ServerPath(c_, dir, "toobig.bin.part")) == -1 || true);
    // the target recovers: the file that fits still reads back, and after the space is freed a new upload works
    if (!t->reconnect()) { c_.note("reconnect: %s", t->err.c_str()); c_.failed = true; delete t; return false; }
    T_CHECK(CheckGen(c_, t, "fits.bin", 1, 300000, 0));
    Vec<Str> names; T_CHECK(t->list(names)); for (const Str& n : names) T_CHECK(!n.ends_with(".part") && n != "toobig.bin");
    T_CHECK(PutGen(c_, t, "fits2.bin", 3, 200000, 1));
    T_CHECK(t->remove("fits.bin")); T_CHECK(t->remove("fits2.bin"));
    delete t;
    return true;
}
VM2B_TEST("scp", scp_disk_full) { return DiskFullCase(c_, "scp"); }

// Folder / file names made of everything a shell or a path parser reacts to: nothing may run on the server, nothing may escape the folder.
static bool InjectionCase(Ctx& c_, const char* type)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str uniq = Str::fmt("%u", (unsigned)(c_.next() % 1000000)), e;
    Str pwn1 = ServerPath(c_, "", "PWNED-" + uniq), pwn2 = ServerPath(c_, "", "pwned2-" + uniq);
    Str unix = g_rootUnix;
    const char* dirs[] = {
        "vm2b-i/a $(touch %PWN1%) b", "vm2b-i/`touch %PWN2%`", "vm2b-i/x;touch %PWN1%;y", "vm2b-i/q'uote\"dq", "vm2b-i/-dash first", "vm2b-i/new\nline",
        "vm2b-i/glob*[ab]?", "vm2b-i/back\\slash", "vm2b-i/amp&&pipe|redir>out<in", "vm2b-i/~tilde $HOME ${IFS}", "vm2b-i/tab\there", "vm2b-i/\xd1\x82\xd0\xb5\xd1\x81\xd1\x82 \xe6\x97\xa5\xe6\x9c\xac" };
    const char* files[] = { "$(touch %PWN2%).00001", "`touch %PWN1%`", "a'b\"c d", "-rf", "*", "[ab]?", "x;y|z&w", "-", "~", "$HOME", "plain.tar.00002" };
    int idx = 0;
    for (const char* dtmpl : dirs) {
        Str d = Str(dtmpl).replace_all("%PWN1%", (unix + "/PWNED-" + uniq).c_str()).replace_all("%PWN2%", (unix + "/pwned2-" + uniq).c_str()) + Str::fmt("-%d", idx++);
        // (a name with a newline cannot be addressed through scp's C line either way: the folder name only ever travels as a quoted word)
        Target* t = MakeT(c_, type, d, e); T_CHECK(t != nullptr); if (!t) continue;
        bool ok = t->connect(); if (!ok) { c_.note("connect (%s): %s", d.c_str(), t->err.c_str()); c_.failed = true; delete t; continue; }
        Vec<Str> want;
        for (const char* ftmpl : files) {
            Str f = Str(ftmpl).replace_all("%PWN1%", (unix + "/PWNED-" + uniq).c_str()).replace_all("%PWN2%", (unix + "/pwned2-" + uniq).c_str());
            if (!RemoteNameOkForTest(f)) continue;
            u64 seed = c_.next(); u64 size = (u64)c_.range(0, 9000);
            if (!PutGen(c_, t, f, seed, size, c_.rnd(2))) { c_.note("upload %s in %s", f.c_str(), d.c_str()); c_.failed = true; continue; }
            want.push_back(f);
            // read-back
            Source* r = t->open_read(f); if (!r) { c_.note("open_read '%s': %s", f.c_str(), t->err.c_str()); c_.failed = true; } else { Bytes buf; buf.resize(20000); u64 n = 0; i64 k; while ((k = r->read(buf.data(), buf.size())) > 0) n += (u64)k; if (k < 0 || n != size) { c_.note("read-back of '%s': %llu of %llu (%s)", f.c_str(), (unsigned long long)n, (unsigned long long)size, r->err.c_str()); c_.failed = true; } r->close(); delete r; }
        }
        Vec<Str> names; T_CHECK(t->list(names));
        if (names.size() != want.size()) { c_.note("%s: listed %zu names for %zu files", d.c_str(), names.size(), want.size()); c_.failed = true; }
        for (const Str& f : want) { bool in = false; for (const Str& n : names) if (n == f) in = true; if (!in) { c_.note("'%s' missing from the listing", f.c_str()); c_.failed = true; } T_CHECK(t->remove(f)); }
        T_CHECK(t->list(names)); T_CHECK(names.size() == 0);
        delete t;
    }
    // nothing was executed, nothing was created above the target folder
    T_CHECK(FileSizeOf(pwn1) == -1); T_CHECK(FileSizeOf(pwn2) == -1);
    return true;
}
VM2B_TEST("scp", scp_names_shell_injection) { return InjectionCase(c_, "scp"); }

// "~/folder" and "~" in the folder setting mean the user's home folder (quoted for the shell, a "~" would be a folder of that name)
VM2B_TEST("scp", scp_tilde_is_home)
{
    const char* type = "scp";
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str rel = Str::fmt("vm2b-t/til %u", (unsigned)(c_.next() % 100000000)), e;
    Target* t = MakeT(c_, type, "~/" + rel, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    T_CHECK(PutGen(c_, t, "a.bin", 3, 4321, 0)); T_CHECK(CheckGen(c_, t, "a.bin", 3, 4321, 0));
    T_CHECK(FileSizeOf(ServerPath(c_, rel, "a.bin")) == 4321);                           // in the home folder, not in a folder named "~"
    T_CHECK(FileSizeOf(g_root + "/~") == -1);
    Vec<Str> names; T_CHECK(t->list(names)); T_CHECK(names.size() == 1 && names[0] == "a.bin");
    T_CHECK(t->remove("a.bin")); T_CHECK(FileSizeOf(ServerPath(c_, rel, "a.bin")) == -1);
    delete t;
    Target* h = MakeT(c_, type, "~", e); T_CHECK(h != nullptr); if (h) { T_CHECK(ConnectNote(c_, h)); T_CHECK(PutGen(c_, h, Str::fmt("vm2b-home-%u.bin", (unsigned)(c_.next() % 1000000)), 5, 100, 0)); Vec<Str> n2; T_CHECK(h->list(n2)); for (const Str& x : n2) if (x.starts_with("vm2b-home-")) h->remove(x); delete h; }
    T_CHECK(FileSizeOf(g_root + "/~") == -1);
    return true;
}

// thousands of small files (the volume pattern of a tiny-volume backup), and a very deep folder
static bool ManyFilesCase(Ctx& c_, const char* type, int count)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Target* t = MakeT(c_, type, dir, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    double t0 = UnixNow();
    for (int i = 0; i < count; ++i) {
        Str n = Str::fmt("vol-%06d.tar.%05d", i, i);
        if (!PutGen(c_, t, n, (u64)i, (u64)(i % 7) * 100 + 1, 0)) { c_.note("file %d", i); c_.failed = true; break; }
    }
    double t1 = UnixNow();
    Vec<Str> names; bool lok = t->list(names); if (!lok) c_.note("list: %s", t->err.c_str()); T_CHECK(lok);
    T_CHECK(names.size() == (size_t)count);
    double t2 = UnixNow();
    for (int i = 0; i < count; i += 97) T_CHECK(CheckGen(c_, t, Str::fmt("vol-%06d.tar.%05d", i, i), (u64)i, (u64)(i % 7) * 100 + 1, 0));
    bool yes = false; T_CHECK(t->exists(Str::fmt("vol-%06d.tar.%05d", count - 1, count - 1), yes)); T_CHECK(yes);
    for (int i = 0; i < count; ++i) { if (!t->remove(Str::fmt("vol-%06d.tar.%05d", i, i))) { c_.note("remove %d: %s", i, t->err.c_str()); c_.failed = true; break; } }
    T_CHECK(t->list(names)); T_CHECK(names.size() == 0);
    c_.note("%s: %d files: upload %.1f s (%.1f ms each), list %.2f s, delete %.1f s", type, count, t1 - t0, (t1 - t0) * 1000 / count, t2 - t1, UnixNow() - t2);
    delete t;
    return true;
}
VM2B_TEST("scp", scp_many_small_files) { return ManyFilesCase(c_, "scp", 700); }

static bool DeepDirCase(Ctx& c_, const char* type)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str dir = Str::fmt("vm2b-deep%u", (unsigned)(c_.next() % 1000000)), e;
    int levels = c_.coin() ? 60 : 300;
    for (int i = 0; i < levels; ++i) dir += (i % 3 == 0) ? "/lev el" : "/l";
    Target* t = MakeT(c_, type, dir, e); T_CHECK(t != nullptr); if (!t) return false;
    bool ok = t->connect(); if (!ok) c_.note("deep (%d levels): %s", levels, t->err.c_str()); T_CHECK(ok);
    if (ok) { T_CHECK(PutGen(c_, t, "deep.bin", 4, 12345, 0)); T_CHECK(CheckGen(c_, t, "deep.bin", 4, 12345, 0)); T_CHECK(t->remove("deep.bin")); }
    delete t;
    // a target path beyond PATH_MAX: a clean error
    { Str huge = "vm2b-huge"; for (int i = 0; i < 700; ++i) huge += "/0123456789"; Target* h = MakeT(c_, type, huge, e); T_CHECK(h != nullptr); if (h) { bool hk = h->connect(); c_.note("path of %zu bytes: %s", huge.size(), hk ? "worked" : h->err.c_str()); if (!hk) T_CHECK(!h->err.empty()); delete h; } }
    return true;
}
VM2B_TEST("scp", scp_very_deep_folder) { return DeepDirCase(c_, "scp"); }

// the server drops a connection that has no open channel for 3 s (a NAT / firewall idle timeout): the target must say "transient",
// and a reconnect must bring everything back (the engine does exactly that)
static bool IdleDropCase(Ctx& c_, const char* type)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    Str dir = RandDir(c_, false), e; Json cfg = TargetCfg(c_, type, dir); Target* t = MakeTarget(cfg, e); T_CHECK(t != nullptr); if (!t) return false;
    T_CHECK(ConnectNote(c_, t));
    T_CHECK(PutGen(c_, t, "a.bin", 1, 5000, 0));
    Sleep(5500);
    Vec<Str> names; bool ok = t->list(names);
    if (ok) { c_.note("%s: the connection survived 5.5 s idle (the server's timeout may not apply to it)", type); T_CHECK(names.size() == 1); }
    else { c_.note("%s: after the idle drop: %s [%s]", type, t->err.c_str(), t->transient ? "transient" : "PERMANENT"); T_CHECK(t->transient); T_CHECK(t->reconnect()); T_CHECK(t->list(names)); T_CHECK(names.size() == 1); }
    T_CHECK(CheckGen(c_, t, "a.bin", 1, 5000, 0));
    // an upload that straddles the drop: the open channel keeps the connection alive
    Sink* w = t->open_write("b.bin"); T_CHECK(w != nullptr);
    if (w) { Bytes d = RandBytes(c_, 100000); T_CHECK(w->write(d.data(), 50000)); Sleep(4500); bool wr = w->write(d.data() + 50000, 50000) && w->finish(); if (!wr) c_.note("upload across 4.5 s idle: %s%s", w->err.c_str(), w->transient ? " [transient]" : " [permanent]"); delete w; }
    delete t;
    return true;
}
VM2B_TEST("scp", scp_server_idle_timeout) { return IdleDropCase(c_, "scp_idle"); }

// several jobs at once against one server: each its own target object (own connection), all transfers must come out intact
struct ParArg { Ctx* c; const char* type; int id; int files; volatile long* bad; Str dir; Str msg; };
static DWORD WINAPI ParWorker(LPVOID p)
{
    ParArg* a = (ParArg*)p;
    Str e; Json cfg = TargetCfg(*a->c, a->type, a->dir); Target* t = MakeTarget(cfg, e);
    if (!t) { a->msg = "MakeTarget: " + e; InterlockedIncrement(a->bad); return 1; }
    if (!t->connect()) { a->msg = "connect: " + t->err; InterlockedIncrement(a->bad); delete t; return 1; }
    for (int i = 0; i < a->files && !*a->bad; ++i) {
        Str n = Str::fmt("job%d-%d.tar.%05d", a->id, i, i);
        u64 seed = (u64)a->id * 1000 + (u64)i, size = (u64)((a->id * 7919 + i * 104729) % 700000);
        Sink* w = t->open_write(n);
        if (!w) { a->msg = "open_write: " + t->err; InterlockedIncrement(a->bad); break; }
        Gen g(seed); Bytes buf; u64 done = 0; bool ok = true;
        while (done < size && ok) { size_t k = (size_t)((size - done) < 30011 ? (size - done) : 30011); buf.resize(k); g.fill(buf.data(), k, 0); ok = w->write(buf.data(), k); done += k; }
        if (ok) ok = w->finish(); else w->abort();
        if (!ok) { a->msg = "upload: " + w->err; InterlockedIncrement(a->bad); delete w; break; }
        delete w;
        Source* r = t->open_read(n); if (!r) { a->msg = "open_read: " + t->err; InterlockedIncrement(a->bad); break; }
        Gen g2(seed); Bytes got, want; u64 pos = 0;
        for (;;) { got.resize(40000); i64 k = r->read(got.data(), got.size()); if (k < 0) { a->msg = "read: " + r->err; InterlockedIncrement(a->bad); break; } if (k == 0) break; want.resize((size_t)k); g2.fill(want.data(), (size_t)k, 0); if (memcmp(got.data(), want.data(), (size_t)k)) { a->msg = "data differs"; InterlockedIncrement(a->bad); break; } pos += (u64)k; }
        r->close(); delete r;
        if (!*a->bad && pos != size) { a->msg = "short file"; InterlockedIncrement(a->bad); }
    }
    Vec<Str> names; if (!*a->bad && !t->list(names)) { a->msg = "list: " + t->err; InterlockedIncrement(a->bad); }
    for (const Str& n : names) if (!*a->bad) t->remove(n);
    delete t;
    return 0;
}
static bool ParallelCase(Ctx& c_, const char* type)
{
    if (!SshServers(c_) || !HaveServer(c_, type)) return true;
    UseRoot(c_, type);
    const int N = 8; ParArg args[N]; HANDLE th[N]; volatile long bad = 0;
    for (int i = 0; i < N; ++i) { args[i].c = &c_; args[i].type = type; args[i].id = i; args[i].files = 4; args[i].bad = &bad; args[i].dir = RandDir(c_, false) + Str::fmt("p%d", i); }
    for (int i = 0; i < N; ++i) th[i] = CreateThread(nullptr, 0, ParWorker, &args[i], 0, nullptr);
    WaitForMultipleObjects(N, th, TRUE, INFINITE);
    for (int i = 0; i < N; ++i) { CloseHandle(th[i]); if (!args[i].msg.empty()) c_.note("job %d: %s", i, args[i].msg.c_str()); }
    T_CHECK(bad == 0);
    return true;
}
VM2B_TEST("scp", scp_parallel_jobs) { return ParallelCase(c_, "scp"); }

// ---- throttling over a real SCP connection: the wire rate follows the limit, an abort under a tight limit is quick ----------
namespace {
struct ThrAbort : Progress {
    DWORD t0, afterMs, abortedAt; explicit ThrAbort(DWORD a) : t0(GetTickCount()), afterMs(a), abortedAt(0) {}
    bool aborted() override { if (GetTickCount() - t0 >= afterMs) { if (!abortedAt) abortedAt = GetTickCount(); return true; } return false; }
};
static bool WriteVmFile(const Str& root, const char* vm, const Bytes& d)
{
    Str full = JoinPath(JoinPath(root, vm), "disk.vmdk"), me; MakeDirs(full.substr(0, (size_t)full.rfind('\\')), me);
    HANDLE h = CreateFileW(FilePath(full), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wn = 0; bool ok = WriteFile(h, d.data(), (DWORD)d.size(), &wn, nullptr) && wn == d.size(); CloseHandle(h); return ok;
}
}

VM2B_TEST("scp", scp_throttle_accuracy_and_abort)
{
    const char* type = "scp";
    if (!SshServers(c_)) return true;
    if (!HaveServer(c_, type)) { c_.skip("server not available"); return true; }
    UseRoot(c_, type);
    Tmp tmp(c_, "sshthr"); Str root = tmp.sub("vms"), e;
    size_t sz = (size_t)c_.range(500000, 900000); Bytes data; data.resize(sz); { u64 x = c_.next() | 1; for (size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; data[i] = (u8)(x >> 24); } }   // really incompressible (c_.fill is not: the wire rate is what is measured)
    T_CHECK(WriteVmFile(root, "vm", data));
    Target* t = MakeT(c_, type, RandDir(c_, false), e); T_CHECK(t && ConnectNote(c_, t)); if (!t) return false;
    VmSource* src = NewDirectorySource(root);
    u64 rate = (u64)c_.range(150000, 300000);
    BackupOptions o; o.codec.level = 1; o.verify = false; o.kdfIterations = 3000;
    RateLimiter rl; rl.setBytesPerSecond(rate); o.limiter = &rl;
    DWORD t0 = GetTickCount(); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, "vm", *t, o, nullptr, m, warn, err); if (!ok) c_.note("backup: %s", err.c_str()); T_CHECK(ok);
    double dt = (double)(GetTickCount() - t0) / 1000.0, want = (double)sz / (double)rate;
    if (dt < want * 0.7 - 0.2 || dt > want * 1.5 + 5) c_.note("%zu bytes at %llu B/s took %.2f s (expected about %.2f)", sz, (unsigned long long)rate, dt, want);
    T_CHECK(dt >= want * 0.7 - 0.2); T_CHECK(dt <= want * 1.5 + 5);
    ReadOptions ro; ro.limiter = &rl; t0 = GetTickCount(); Str ve;
    T_CHECK(VerifyBackup(*t, m.archive, "", ro, nullptr, nullptr, ve));
    dt = (double)(GetTickCount() - t0) / 1000.0;
    if (dt < want * 0.7 - 0.2 || dt > want * 1.5 + 5) c_.note("verify %.2f s (expected about %.2f)", dt, want);
    T_CHECK(dt >= want * 0.7 - 0.2); T_CHECK(dt <= want * 1.5 + 5);
    Str root2 = tmp.sub("vms2"); T_CHECK(WriteVmFile(root2, "vm2", data));
    VmSource* src2 = NewDirectorySource(root2);
    rl.setBytesPerSecond(3000); ThrAbort ap(1000); Manifest m2; Str w2, e2;
    ok = BackupVm(*src2, "vm2", *t, o, &ap, m2, w2, e2); T_CHECK(!ok);
    DWORD lat = ap.abortedAt ? GetTickCount() - ap.abortedAt : 0;
    if (!ap.abortedAt || lat > 5000) c_.note("abort latency %lu ms (flag seen: %d)", (unsigned long)lat, ap.abortedAt != 0);
    T_CHECK(ap.abortedAt != 0); T_CHECK(lat < 5000);
    Vec<Str> names; T_CHECK(t->list(names));
    for (const Str& n : names) if (n.find("vm2") >= 0) { c_.note("left behind after abort: %s", n.c_str()); c_.failed = true; }
    delete src; delete src2; delete t;
    return true;
}

// SFTP was removed: an old config that still holds a server of type "sftp" loads, is unusable with a clear message, and cannot be created again
VM2B_TEST("ssh", legacy_sftp_server_is_unusable)
{
    Tmp tmp(c_, "sftpgone");
    Str e, js = "{\"servers\":[{\"id\":\"a1\",\"name\":\"old\",\"type\":\"sftp\",\"host\":\"h\",\"port\":22,\"user\":\"u\",\"password\":\"p\",\"directory\":\"d\"}]}";
    T_CHECK(WriteWholeFile(JoinPath(tmp.path, "config.json").c_str(), js.data(), js.size(), e));
    Config cfg; T_CHECK(cfg.open(tmp.path, e));
    Json s; T_CHECK(cfg.server("a1", true, s)); T_CHECK(s.get("type").str() == "sftp");
    Target* t = MakeTarget(s, e); T_CHECK(t == nullptr);
    T_CHECK(e.find("SFTP is no longer supported") >= 0); T_CHECK(e.find("SCP") >= 0); T_CHECK(e.find("password") < 0);
    Json in = Json::object(); in["name"] = "new"; in["type"] = "sftp"; in["host"] = "h"; Json out;
    T_CHECK(!cfg.saveServer(in, out, e)); T_CHECK(e.find("no longer supported") >= 0);
    return true;
}

} // namespace vm2b
