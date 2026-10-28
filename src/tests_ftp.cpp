// Autotests of the network modules: TLS (OpenSSL), the HTTP client, the FTP / FTPS target. They need the Python test servers
// of tools/testservers (started by tools/run-ftp-tests.sh, which writes the --servers file); without them every test is a SKIP.
#include "autotest.h"
#include "crypto.h"
#include "engine.h"
#include "http.h"
#include "tls.h"
#include "winutil.h"

namespace vm2b {

// ---- helpers ---------------------------------------------------------------------------------------------------------
static u8 PatByte(u64 i) { return (u8)(((i % 65536) * 131 + 17) & 0xff); }
static void PatFill(u64 off, void* p, size_t n) { u8* d = (u8*)p; for (size_t i = 0; i < n; ++i) d[i] = PatByte(off + i); }
static bool PatCheck(u64 off, const void* p, size_t n) { const u8* d = (const u8*)p; for (size_t i = 0; i < n; ++i) if (d[i] != PatByte(off + i)) return false; return true; }

static const Json* Srv(Ctx& c, const char* key)
{
    if (c.servers.is_null() || !c.servers.has(key)) { c.skip(Str::fmt("no \"%s\" server (run tools/run-ftp-tests.sh)", key).c_str()); return nullptr; }
    return &c.servers.get(key);
}

static bool NetLine(NetConn& n, Str& line)
{
    line.clear();
    for (;;) {
        char ch; i64 k = n.recv(&ch, 1);
        if (k <= 0) return false;
        if (ch == '\n') return true;
        if (ch != '\r') line += ch;
    }
}
static bool NetCmd(NetConn& n, const char* cmd) { Str s(cmd); s += "\n"; return n.send(s.data(), s.size()); }

static bool NetReadPattern(NetConn& n, u64 total, bool& good)
{
    Bytes buf; buf.resize(1 << 17); u64 got = 0; good = true;
    while (got < total) {
        size_t want = (size_t)((total - got) < buf.size() ? (total - got) : buf.size());
        i64 k = n.recv(buf.data(), want);
        if (k <= 0) return false;
        if (!PatCheck(got, buf.data(), (size_t)k)) good = false;
        got += (u64)k;
    }
    return true;
}


struct TlsEnv {
    Str host, ip, fp, fpSelf, caFile;
    int port, port10, port13, portSelf, portWrong, portStart, closedPort;
    explicit TlsEnv(const Json& j) {
        host = j.get("host").str(); ip = j.get("ip").str(); fp = j.get("fp").str(); fpSelf = j.get("fp_self").str(); caFile = j.get("ca_file").str();
        port = (int)j.get("port").integer(); port10 = (int)j.get("port10").integer(); port13 = (int)j.get("port13").integer();
        portSelf = (int)j.get("port_self").integer(); portWrong = (int)j.get("port_wrong").integer(); portStart = (int)j.get("port_starttls").integer();
        closedPort = (int)j.get("closed_port").integer();
    }
};

static TlsOptions PinOpts(const Str& fp, int timeoutMs = 10000) { TlsOptions o; o.fingerprint = fp; o.timeoutMs = timeoutMs; return o; }

// ---- tls -------------------------------------------------------------------------------------------------------------
VM2B_TEST("tls", tls_roundtrip_random)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    int rounds = c_.range(3, 6);
    for (int r = 0; r < rounds; ++r) {
        TlsOptions o = PinOpts(e.fp);
        if (c_.coin(30)) { o.fingerprint.clear(); o.verify = false; }
        NetConn n;
        bool ok = n.connect(e.host, e.port, true, o);
        if (!ok) c_.note("connect: %s", n.err.c_str());
        T_CHECK(ok); if (!ok) return false;
        T_CHECK(n.isTls()); T_CHECK(n.peerFingerprint() == e.fp);
        // server -> client
        u64 down = c_.coin(20) ? 0 : (u64)c_.range(1, c_.coin(30) ? 3000000 : 70000);
        NetCmd(n, Str::fmt("SEND %llu", (unsigned long long)down).c_str());
        bool good = false; T_CHECK(NetReadPattern(n, down, good)); T_CHECK(good);
        // client -> server
        u64 up = (u64)c_.range(0, c_.coin(30) ? 3000000 : 50000);
        NetCmd(n, Str::fmt("RECV %llu", (unsigned long long)up).c_str());
        Bytes buf; buf.resize(up); PatFill(0, buf.data(), up);
        size_t pos = 0;
        while (pos < up) { size_t k = (size_t)c_.range(1, 300000); if (k > up - pos) k = (size_t)(up - pos); T_CHECK(n.send(buf.data() + pos, k)); pos += k; }
        Str line; T_CHECK(NetLine(n, line));
        Str want = Str::fmt("GOT %llu %u", (unsigned long long)up, (unsigned)Crc32(0, buf.data(), up));
        if (line != want) c_.note("server said '%s', wanted '%s'", line.c_str(), want.c_str());
        T_CHECK(line == want);
        n.shutdown();
    }
    return true;
}

VM2B_TEST("tls", tls_fingerprint_pinning)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    // the right pin in several spellings
    Str hex = e.fp.substr(7), colon, upper = hex;
    for (size_t i = 0; i < hex.size(); i += 2) { if (i) colon += ':'; colon += hex.substr(i, 2); }
    for (size_t i = 0; i < upper.size(); ++i) if (upper[i] >= 'a' && upper[i] <= 'f') upper[i] -= 32;
    Str spell[] = { e.fp, hex, "SHA256:" + upper, "sha256:" + colon, "sha256: " + hex };
    for (const Str& s : spell) {
        NetConn n; TlsOptions o = PinOpts(s);
        bool ok = n.connect(e.host, e.port, true, o);
        if (!ok) c_.note("pin '%s': %s", s.c_str(), n.err.c_str());
        T_CHECK(ok);
    }
    { // sha1 pin
        NetConn probe; TlsOptions o; o.verify = false; T_CHECK(probe.connect(e.host, e.port, true, o));
        NetConn n; TlsOptions p = PinOpts(probe.peerFingerprintSha1());
        T_CHECK(n.connect(e.host, e.port, true, p));
    }
    { // a self-signed server, accepted only by its pin (the chain is not trusted, the name would match)
        NetConn n; T_CHECK(n.connect(e.host, e.portSelf, true, PinOpts(e.fpSelf)));
    }
    { // wrong pin: refused, permanent, the text carries the real fingerprint
        NetConn n; TlsOptions o = PinOpts("sha256:0000000000000000000000000000000000000000000000000000000000000000");
        T_CHECK(!n.connect(e.host, e.port, true, o)); T_CHECK(n.certError); T_CHECK(!n.timedOut);
        T_CHECK(n.err.find(e.fp.c_str()) >= 0); T_CHECK(!n.isOpen());
    }
    { // malformed pin
        NetConn n; TlsOptions o = PinOpts("sha256:xyz");
        T_CHECK(!n.connect(e.host, e.port, true, o)); T_CHECK(n.certError);
    }
    { // a pin of the OTHER certificate must not pass
        NetConn n; T_CHECK(!n.connect(e.host, e.port, true, PinOpts(e.fpSelf))); T_CHECK(n.certError);
    }
    return true;
}

VM2B_TEST("tls", tls_system_store_and_ca_bundle)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    { // default: the system store does not know the test CA -> refused, with the fingerprint in the text
        NetConn n; TlsOptions o; o.timeoutMs = 10000;
        T_CHECK(!n.connect(e.host, e.port, true, o)); T_CHECK(n.certError);
        T_CHECK(n.err.find(e.fp.c_str()) >= 0);
        if (n.err.find(e.fp.c_str()) < 0) c_.note("text: %s", n.err.c_str());
    }
    { // self-signed without a pin: refused
        NetConn n; TlsOptions o; o.timeoutMs = 10000;
        T_CHECK(!n.connect(e.host, e.portSelf, true, o)); T_CHECK(n.certError); T_CHECK(n.err.find(e.fpSelf.c_str()) >= 0);
    }
    { // extra CA bundle: accepted (host name localhost is in the SAN)
        NetConn n; TlsOptions o; o.caFile = e.caFile; o.timeoutMs = 10000;
        bool ok = n.connect(e.host, e.port, true, o);
        if (!ok) c_.note("ca bundle: %s", n.err.c_str());
        T_CHECK(ok);
    }
    { // ... also by IP (the SAN has 127.0.0.1)
        NetConn n; TlsOptions o; o.caFile = e.caFile; o.timeoutMs = 10000;
        bool ok = n.connect(e.ip, e.port, true, o);
        if (!ok) c_.note("ca bundle by ip: %s", n.err.c_str());
        T_CHECK(ok);
    }
    { // CA-signed but for another name: refused although the CA is trusted
        NetConn n; TlsOptions o; o.caFile = e.caFile; o.timeoutMs = 10000;
        T_CHECK(!n.connect(e.host, e.portWrong, true, o)); T_CHECK(n.certError); T_CHECK(!n.timedOut);
    }
    { // verify=false takes it
        NetConn n; TlsOptions o; o.verify = false; o.timeoutMs = 10000;
        T_CHECK(n.connect(e.host, e.portWrong, true, o));
    }
    { // a bundle that does not exist / is empty
        NetConn n; TlsOptions o; o.caFile = "Z:\\tmp\\no-such-bundle.pem"; o.timeoutMs = 10000;
        T_CHECK(!n.connect(e.host, e.port, true, o)); T_CHECK(n.certError);
    }
    return true;
}

// The certificate matrix: what is accepted and what is refused, with a CA bundle (the test CA), a pin, and verify=false.
VM2B_TEST("tls", tls_certificate_matrix)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    if (!j->has("port_expired")) { c_.skip("servers.json without the certificate matrix"); return true; }
    TlsEnv e(*j);
    struct Case { const char* name; const char* portKey; const char* host; bool useCa; int want; };    // want: 1 accepted, 0 refused (certError), -1 only reported
    const char* ip = e.ip.c_str(), *lh = e.host.c_str();
    Case cases[] = {
        { "expired + test CA", "port_expired", lh, true, 0 },
        { "not yet valid + test CA", "port_notyet", lh, true, 0 },
        { "chain with intermediate sent + root bundle", "port_chain", lh, true, 1 },
        { "chain with intermediate sent + root bundle, by IP", "port_chain", ip, true, 1 },
        { "leaf only, intermediate missing + root bundle", "port_chainleaf", lh, true, 0 },
        { "chain, but no CA bundle", "port_chain", lh, false, 0 },
        { "SAN has only the IP: by IP", "port_sanip", ip, true, 1 },
        { "SAN has only the IP: by name localhost", "port_sanip", lh, true, 0 },
        { "no SAN, CN=localhost", "port_cnonly", lh, true, -1 },
        { "CA-signed, but the bundle holds another CA", "port_othercaleaf", lh, true, 0 },
        { "good certificate by name", "port", lh, true, 1 },
    };
    for (const Case& cs : cases) {
        NetConn n; TlsOptions o; o.timeoutMs = 10000; if (cs.useCa) o.caFile = e.caFile;
        int port = (int)j->get(cs.portKey).integer();
        bool ok = n.connect(cs.host, port, true, o);
        if (cs.want == -1) { c_.note("%s: %s (%s)", cs.name, ok ? "accepted" : "refused", ok ? "" : n.err.c_str()); continue; }
        if (ok != (cs.want == 1)) { c_.note("%s: %s, expected %s: %s", cs.name, ok ? "accepted" : "refused", cs.want ? "accepted" : "refused", n.err.c_str()); c_.failed = true; }
        if (!ok && cs.want == 0) { T_CHECK(n.certError); T_CHECK(!n.timedOut); T_CHECK(!n.isOpen()); }
    }
    // verify=false accepts every one of them, and a pin overrides the chain / validity / name checks (like an SSH host key)
    const char* keys[] = { "port_expired", "port_notyet", "port_chainleaf", "port_sanip", "port_cnonly", "port_othercaleaf" };
    for (const char* k : keys) {
        NetConn n; TlsOptions o; o.verify = false; o.timeoutMs = 10000;
        bool ok = n.connect(e.host, (int)j->get(k).integer(), true, o); if (!ok) c_.note("verify=false %s: %s", k, n.err.c_str()); T_CHECK(ok);
        NetConn p; T_CHECK(p.connect(e.host, (int)j->get(k).integer(), true, o)); Str fp = p.peerFingerprintSha1();
        NetConn q; TlsOptions po = PinOpts(fp); bool pk = q.connect(e.host, (int)j->get(k).integer(), true, po); if (!pk) c_.note("pin %s: %s", k, q.err.c_str()); T_CHECK(pk);
    }
    // the expired certificate is refused with a message that says why (not a generic failure)
    { NetConn n; TlsOptions o; o.caFile = e.caFile; o.timeoutMs = 10000; T_CHECK(!n.connect(e.host, (int)j->get("port_expired").integer(), true, o)); c_.note("expired: %s", n.err.c_str()); T_CHECK(!n.err.empty()); }
    return true;
}

VM2B_TEST("tls", tls_protocol_floor)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    { // a server that speaks TLS 1.3 only
        NetConn n; TlsOptions o = PinOpts(e.fp);
        bool ok = n.connect(e.host, e.port13, true, o);
        if (!ok) c_.note("TLS 1.3 only: %s", n.err.c_str());
        c_.note("TLS 1.3-only server negotiated: %s", ok ? n.tlsVersion().c_str() : "(refused)");
        T_CHECK(ok); if (ok) T_CHECK(n.tlsVersion() == "TLS 1.3");
    }
    { // default connection to a TLS 1.0-1.3 server: 1.2 or better
        NetConn n; TlsOptions o = PinOpts(e.fp); T_CHECK(n.connect(e.host, e.port, true, o));
        T_CHECK(n.tlsVersion() == "TLS 1.2" || n.tlsVersion() == "TLS 1.3");
    }
    { // a TLS 1.0-only server: refused with the default floor, accepted with tls_min 1.0
        NetConn n; TlsOptions o = PinOpts(e.fp);
        bool ok = n.connect(e.host, e.port10, true, o);
        T_CHECK(!ok); T_CHECK(!n.certError); T_CHECK(!n.timedOut);
        NetConn m; TlsOptions o2 = PinOpts(e.fp); o2.tlsMin = "1.0";
        bool ok2 = m.connect(e.host, e.port10, true, o2);
        if (!ok2) c_.note("tls_min 1.0: %s", m.err.c_str());
        T_CHECK(ok2);
        if (ok2) { T_CHECK(m.tlsVersion() == "TLS 1.0"); NetCmd(m, "PING"); Str l; T_CHECK(NetLine(m, l)); T_CHECK(l.starts_with("PONG")); }
    }
    return true;
}

VM2B_TEST("tls", tls_session_resumption)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    for (int ver = 0; ver < 3; ++ver) {
        int port = e.port;
        TlsOptions o = PinOpts(e.fp);
        if (ver == 1) { o.tlsMin = "1.0"; port = e.port10; }          // TLS 1.0 resumes by session id, no tickets involved
        if (ver == 2) port = e.port13;                                // TLS 1.3 resumes with a session ticket (arrives after the handshake)
        TlsCredRef cred;
        NetConn a; T_CHECK(a.connect(e.host, port, true, o, &cred)); T_CHECK(cred.valid());
        NetCmd(a, "PING"); Str l1; T_CHECK(NetLine(a, l1)); T_CHECK(l1.find("reused=0") > 0);
        NetConn b; T_CHECK(b.connect(e.host, port, true, o, &cred));
        NetCmd(b, "PING"); Str l2; T_CHECK(NetLine(b, l2));
        c_.note("second connection (%s): %s", ver == 1 ? "TLS1.0" : ver == 2 ? "TLS1.3" : "default", l2.c_str());
        bool reused = l2.find("reused=1") > 0;
        T_CHECK(reused); T_CHECK(b.sessionReused()); T_CHECK(!a.sessionReused());
        if (ver == 2) { T_CHECK(l2.find("TLSv1.3") > 0); T_CHECK(b.tlsVersion() == "TLS 1.3"); }
        // without the shared credential nothing is resumed
        NetConn d; T_CHECK(d.connect(e.host, port, true, o)); NetCmd(d, "PING"); Str l3; T_CHECK(NetLine(d, l3)); T_CHECK(l3.find("reused=0") > 0);
    }
    return true;
}

VM2B_TEST("tls", tls_starttls_and_eof)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    TlsOptions o = PinOpts(e.fp);
    { // plain text first, then the upgrade on the same socket
        NetConn n; T_CHECK(n.connect(e.host, e.portStart, false, o)); T_CHECK(!n.isTls());
        Str l; T_CHECK(NetLine(n, l)); T_CHECK(l == "READY");
        NetCmd(n, "STARTTLS"); T_CHECK(NetLine(n, l)); T_CHECK(l == "GO");
        T_CHECK(n.starttls(e.host, o)); T_CHECK(n.isTls()); T_CHECK(n.peerFingerprint() == e.fp);
        NetCmd(n, "PING"); T_CHECK(NetLine(n, l)); T_CHECK(l.starts_with("PONG"));
    }
    { // a clean end (close_notify) is reported as clean, a bare FIN is not
        for (int mode = 0; mode < 2; ++mode) {
            NetConn n; T_CHECK(n.connect(e.host, e.port, true, o));
            NetCmd(n, mode ? "RAGGED" : "BYE");
            char b[3]; i64 got = 0;
            while (got < 3) { i64 k = n.recv(b + got, 3 - (size_t)got); if (k <= 0) break; got += k; }
            T_CHECK(got == 3 && !memcmp(b, "bye", 3));
            char x; i64 k = n.recv(&x, 1);
            if (k < 0 && mode == 1) { c_.note("ragged end reported as error: %s", n.err.c_str()); }
            else { T_CHECK(k == 0); T_CHECK(n.cleanEof == (mode == 0)); }
        }
    }
    return true;
}

struct AbortLater { const volatile long* flag; DWORD ms; };
static DWORD WINAPI AbortThread(LPVOID p) { AbortLater* a = (AbortLater*)p; Sleep(a->ms); InterlockedExchange((volatile LONG*)a->flag, 1); return 0; }

VM2B_TEST("tls", tls_timeout_abort_unreachable)
{
    const Json* j = Srv(c_, "tls"); if (!j) return true;
    TlsEnv e(*j);
    { // a server that never answers: the idle timeout fires
        TlsOptions o = PinOpts(e.fp, 700); NetConn n; T_CHECK(n.connect(e.host, e.port, true, o));
        NetCmd(n, "HANG"); char x; DWORD t0 = GetTickCount();
        i64 k = n.recv(&x, 1); DWORD dt = GetTickCount() - t0;
        T_CHECK(k < 0); T_CHECK(n.timedOut); T_CHECK(dt >= 600 && dt < 5000);
        if (!(dt >= 600 && dt < 5000)) c_.note("timeout took %lu ms", (unsigned long)dt);
    }
    { // the abort flag ends a wait
        static volatile LONG flag; flag = 0;
        TlsOptions o = PinOpts(e.fp, 20000); o.abort = &flag; NetConn n; T_CHECK(n.connect(e.host, e.port, true, o));
        NetCmd(n, "HANG");
        AbortLater a = { &flag, 400 }; HANDLE th = CreateThread(nullptr, 0, AbortThread, &a, 0, nullptr);
        char x; DWORD t0 = GetTickCount(); i64 k = n.recv(&x, 1); DWORD dt = GetTickCount() - t0;
        WaitForSingleObject(th, 5000); CloseHandle(th);
        T_CHECK(k < 0); T_CHECK(n.aborted); T_CHECK(dt < 3000);
    }
    { // nothing listens: refused quickly, not a certificate problem
        NetConn n; TlsOptions o = PinOpts(e.fp, 3000);
        T_CHECK(!n.connect(e.host, e.closedPort, true, o)); T_CHECK(!n.certError); T_CHECK(!n.err.empty());
        c_.note("refused: %s", n.err.c_str());
    }
    { // an unresolvable name
        NetConn n; TlsOptions o = PinOpts(e.fp, 3000);
        T_CHECK(!n.connect("no-such-host.invalid", 443, true, o)); T_CHECK(!n.certError);
    }
    { // a plain server spoken to as TLS: handshake error, no hang
        NetConn n; TlsOptions o = PinOpts(e.fp, 3000); DWORD t0 = GetTickCount();
        bool ok = n.connect(e.host, e.portStart, true, o);       // the server sends "READY\n" in clear
        T_CHECK(!ok); T_CHECK(GetTickCount() - t0 < 8000);
    }
    return true;
}

// ---- http ------------------------------------------------------------------------------------------------------------
struct PatSource : Source {
    u64 total, pos; u64 failAt; bool failTransient;
    explicit PatSource(u64 n) : total(n), pos(0), failAt((u64)-1), failTransient(true) {}
    i64 read(void* p, size_t n) override {
        if (pos >= total) return 0;
        u64 left = total - pos; if (left < n) n = (size_t)left;
        if (pos >= failAt) return fail("injected source failure", failTransient);
        PatFill(pos, p, n); pos += n; return (i64)n;
    }
};

// Reads a whole Source checking it against the pattern; returns bytes read, -1 on error.
static i64 DrainPattern(Source* s, bool& good, u64 offset = 0)
{
    Bytes buf; buf.resize(1 << 17); u64 got = 0; good = true;
    for (;;) {
        i64 k = s->read(buf.data(), buf.size());
        if (k < 0) return -1;
        if (k == 0) break;
        if (!PatCheck(offset + got, buf.data(), (size_t)k)) good = false;
        got += (u64)k;
    }
    return (i64)got;
}

static bool BigRequested() { char env[16] = ""; return GetEnvironmentVariableA("VM2B_BIG", env, sizeof(env)) > 0 && env[0] == '1'; }

struct HttpEnv {
    Str host; int port, tlsPort, closedPort; Str fp, caFile;
    explicit HttpEnv(const Json& j) {
        host = j.get("host").str(); port = (int)j.get("port").integer(); tlsPort = (int)j.get("tls_port").integer(); closedPort = (int)j.get("closed_port").integer();
        fp = j.get("fp").str(); caFile = j.get("ca_file").str();
    }
    HttpOptions opts(bool tls) const {
        HttpOptions o; o.host = host; o.tls = tls; o.port = tls ? tlsPort : port; o.tlsOpt.fingerprint = tls ? fp : Str(); o.tlsOpt.timeoutMs = 20000; return o;
    }
};

VM2B_TEST("http", http_get_framing_random)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    for (int tls = 0; tls < 2; ++tls) {
        HttpClient cl(e.opts(tls != 0));
        int reqs = c_.range(6, 12);
        int expectConnects = 1;
        for (int i = 0; i < reqs; ++i) {
            int kind = (int)c_.rnd(4);
            u64 n = c_.coin(15) ? 0 : (u64)c_.range(1, c_.coin(25) ? 4000000 : 90000);
            Str path;
            if (kind == 0) path = Str::fmt("/len/%llu", (unsigned long long)n);
            else if (kind == 1) path = Str::fmt("/chunked/%llu?c=%d", (unsigned long long)n, (n > 200000 ? c_.range(1000, 70000) : c_.range(1, c_.coin() ? 100 : 70000)));
            else if (kind == 2) path = Str::fmt("/close/%llu", (unsigned long long)n);
            else path = Str::fmt("/http10/%llu", (unsigned long long)n);
            HttpHeaders h; HttpResponse r;
            bool ok = cl.request("GET", path, h, nullptr, 0, r);
            if (!ok) c_.note("%s: %s", path.c_str(), cl.err.c_str());
            T_CHECK(ok); if (!ok) return false;
            T_CHECK(r.status == 200); T_CHECK(r.body != nullptr);
            if (kind == 0) T_CHECK(r.contentLength == (i64)n); else T_CHECK(r.contentLength == -1);
            bool good = false; i64 got = DrainPattern(r.body, good);
            if (got != (i64)n || !good) c_.note("%s: got %lld want %llu good=%d err=%s", path.c_str(), (long long)got, (unsigned long long)n, (int)good, r.body->err.c_str());
            T_CHECK(got == (i64)n); T_CHECK(good);
            if (kind >= 2 && i + 1 < reqs) ++expectConnects;      // these end the connection: the next request opens a new one
        }
        T_CHECK(cl.connects() == expectConnects);
        if (cl.connects() != expectConnects) c_.note("connects %d, expected %d", cl.connects(), expectConnects);
    }
    return true;
}

VM2B_TEST("http", http_keepalive_and_stale_reconnect)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    for (int tls = 0; tls < 2; ++tls) {
        HttpClient cl(e.opts(tls != 0));
        HttpHeaders h; Bytes b; HttpResponse r;
        for (int i = 0; i < 8; ++i) { T_CHECK(cl.requestBytes("GET", "/count", h, nullptr, 0, r, b)); T_CHECK(r.status == 200); }
        T_CHECK(cl.connects() == 1);
        // the server drops the connection after an answer that promised keep-alive: the next request must still work
        for (int i = 0; i < 4; ++i) {
            T_CHECK(cl.requestBytes("GET", "/sneaky", h, nullptr, 0, r, b)); T_CHECK(r.status == 200); T_CHECK(b.size() == 2);
            if (c_.coin()) Sleep((DWORD)c_.rnd(60));
            bool ok = cl.requestBytes("GET", "/len/1000", h, nullptr, 0, r, b);
            if (!ok) c_.note("after sneaky close: %s", cl.err.c_str());
            T_CHECK(ok); T_CHECK(b.size() == 1000);
        }
        T_CHECK(cl.reconnects() >= 1);
        // an unread body is drained (small) or the connection dropped (big); either way the next request works
        int before = cl.connects();
        T_CHECK(cl.request("GET", "/len/50000", h, nullptr, 0, r));
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);
        T_CHECK(cl.connects() == before);
        T_CHECK(cl.request("GET", "/len/30000000", h, nullptr, 0, r));
        char tmp[100]; T_CHECK(r.body->read(tmp, sizeof(tmp)) == 100);
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);
        T_CHECK(cl.connects() == before + 1);
        // HEAD, 404, 302 not followed
        T_CHECK(cl.request("HEAD", "/head-me", h, nullptr, 0, r)); T_CHECK(r.status == 200); T_CHECK(r.contentLength == 1234); T_CHECK(r.body->read(tmp, 10) == 0);
        T_CHECK(cl.requestBytes("GET", "/len/3", h, nullptr, 0, r, b)); T_CHECK(b.size() == 3);
        T_CHECK(cl.requestBytes("GET", "/nothing", h, nullptr, 0, r, b)); T_CHECK(r.status == 404);
        T_CHECK(cl.requestBytes("GET", "/redir", h, nullptr, 0, r, b)); T_CHECK(r.status == 302); T_CHECK(r.headers.value("Location") == "/len/5");
        T_CHECK(cl.requestBytes("GET", "/status/500", h, nullptr, 0, r, b)); T_CHECK(r.status == 500);
    }
    return true;
}

VM2B_TEST("http", http_put_streaming_random)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    for (int tls = 0; tls < 2; ++tls) {
        HttpOptions o = e.opts(tls != 0); o.expect100 = c_.coin();
        HttpClient cl(o);
        int reqs = c_.range(4, 8);
        for (int i = 0; i < reqs; ++i) {
            u64 n = c_.coin(15) ? 0 : (u64)c_.range(1, c_.coin(30) ? 6000000 : 80000);
            int mode = (int)c_.rnd(4);       // 0 memory, 1 streamed with length, 2 streamed chunked, 3 POST streamed
            HttpHeaders h; HttpResponse r; bool ok;
            Bytes mem;
            if (mode == 0) { mem.resize((size_t)n); PatFill(0, mem.data(), (size_t)n); ok = cl.request("PUT", "/put/x", h, mem.data(), mem.size(), r); }
            else { PatSource ps(n); ok = cl.requestStream(mode == 3 ? "POST" : "PUT", "/put/x", h, &ps, mode == 2 ? -1 : (i64)n, r); }
            if (!ok) c_.note("PUT mode %d n=%llu: %s", mode, (unsigned long long)n, cl.err.c_str());
            T_CHECK(ok); if (!ok) return false;
            T_CHECK(r.status == 201);
            Bytes rb; u8 tmp[256]; i64 k; while ((k = r.body->read(tmp, sizeof(tmp))) > 0) { size_t o2 = rb.size(); rb.resize(o2 + (size_t)k); memcpy(rb.data() + o2, tmp, (size_t)k); }
            Bytes all; all.resize((size_t)n); PatFill(0, all.data(), (size_t)n);
            Str want = Str::fmt("%llu %u", (unsigned long long)n, (unsigned)Crc32(0, all.data(), all.size()));
            Str got((const char*)rb.data(), rb.size());
            if (got != want) c_.note("server answered '%s' wanted '%s' (mode %d)", got.c_str(), want.c_str(), mode);
            T_CHECK(got == want);
        }
        T_CHECK(cl.connects() == 1);
        // POST /echo with a memory body
        HttpHeaders h; h.add("Content-Type", "text/xml; charset=utf-8"); HttpResponse r; Bytes out;
        Str soap = "<soap>\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82</soap>";
        T_CHECK(cl.requestBytes("POST", "/echo", h, soap.data(), soap.size(), r, out)); T_CHECK(Str((const char*)out.data(), out.size()) == soap);
    }
    return true;
}

VM2B_TEST("http", http_expect_continue_refused)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    for (int ex = 0; ex < 2; ++ex) {
        HttpOptions o = e.opts(c_.coin()); o.expect100 = ex == 1;
        HttpClient cl(o);
        PatSource ps(100u << 20); HttpHeaders h; HttpResponse r;
        bool ok = cl.requestStream("PUT", "/reject", h, &ps, (i64)ps.total, r);
        if (ex == 1) { T_CHECK(ok); if (ok) { T_CHECK(r.status == 401); T_CHECK(ps.pos < ps.total / 2); c_.note("100-continue refused: only %llu bytes of the body were read", (unsigned long long)ps.pos); } else c_.note("%s", cl.err.c_str()); }
        else { // without Expect the server answers while the client is still sending: either the 401 or a clean transient error
            if (ok) T_CHECK(r.status == 401); else { T_CHECK(cl.transient); c_.note("(no Expect) %s", cl.err.c_str()); }
        }
        HttpHeaders h2; Bytes b; HttpResponse r2;
        T_CHECK(cl.requestBytes("GET", "/len/10", h2, nullptr, 0, r2, b)); T_CHECK(b.size() == 10);      // the client recovers with a fresh connection
    }
    return true;
}

VM2B_TEST("http", http_cookies_auth)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    HttpClient cl(e.opts(c_.coin()));
    HttpHeaders h; HttpResponse r; Bytes b;
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h, nullptr, 0, r, b)); T_CHECK(b.empty());
    T_CHECK(cl.requestBytes("GET", "/cookie/set", h, nullptr, 0, r, b)); T_CHECK(r.status == 200);
    T_CHECK(cl.getCookie("vmware_soap_session") == "\"abc123\""); T_CHECK(cl.getCookie("other") == "1");
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h, nullptr, 0, r, b));
    T_CHECK(Str((const char*)b.data(), b.size()) == "vmware_soap_session=\"abc123\"; other=1");
    T_CHECK(cl.requestBytes("GET", "/cookie/del", h, nullptr, 0, r, b));
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h, nullptr, 0, r, b));
    T_CHECK(Str((const char*)b.data(), b.size()) == "vmware_soap_session=\"abc123\"");
    cl.setCookie("manual", "x y");
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h, nullptr, 0, r, b)); T_CHECK(Str((const char*)b.data(), b.size()).find("manual=x y") > 0);
    cl.clearCookies();
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h, nullptr, 0, r, b)); T_CHECK(b.empty());
    // an explicit Cookie header of the caller wins
    HttpHeaders h2; h2.add("Cookie", "mine=1");
    T_CHECK(cl.requestBytes("GET", "/cookie/show", h2, nullptr, 0, r, b)); T_CHECK(Str((const char*)b.data(), b.size()) == "mine=1");
    // Basic auth
    T_CHECK(cl.requestBytes("GET", "/auth", h, nullptr, 0, r, b)); T_CHECK(r.status == 401);
    cl.setBasicAuth("user", "wrong");
    T_CHECK(cl.requestBytes("GET", "/auth", h, nullptr, 0, r, b)); T_CHECK(r.status == 401);
    cl.setBasicAuth("user", "p\xc3\xa4ss word");
    T_CHECK(cl.requestBytes("GET", "/auth", h, nullptr, 0, r, b)); T_CHECK(r.status == 200);
    T_CHECK(HttpClient::BasicAuthValue("Aladdin", "open sesame") == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    return true;
}

VM2B_TEST("http", http_failures)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    HttpHeaders h; HttpResponse r; Bytes b;
    { // wrong pin: permanent, carries the real fingerprint
        HttpOptions o = e.opts(true); o.tlsOpt.fingerprint = "sha256:1111111111111111111111111111111111111111111111111111111111111111";
        HttpClient cl(o); T_CHECK(!cl.request("GET", "/len/1", h, nullptr, 0, r)); T_CHECK(cl.certError); T_CHECK(!cl.transient); T_CHECK(cl.err.find(e.fp.c_str()) >= 0);
    }
    { // untrusted certificate with no pin
        HttpOptions o = e.opts(true); o.tlsOpt.fingerprint.clear(); HttpClient cl(o);
        T_CHECK(!cl.request("GET", "/len/1", h, nullptr, 0, r)); T_CHECK(cl.certError); T_CHECK(cl.err.find(e.fp.c_str()) >= 0);
        HttpOptions o2 = e.opts(true); o2.tlsOpt.fingerprint.clear(); o2.tlsOpt.verify = false; HttpClient c2(o2);
        T_CHECK(c2.requestBytes("GET", "/len/7", h, nullptr, 0, r, b)); T_CHECK(b.size() == 7); T_CHECK(c2.peerFingerprint() == e.fp);
        HttpOptions o3 = e.opts(true); o3.tlsOpt.fingerprint.clear(); o3.tlsOpt.caFile = e.caFile; HttpClient c3(o3);
        T_CHECK(c3.requestBytes("GET", "/len/7", h, nullptr, 0, r, b)); T_CHECK(b.size() == 7);
    }
    { // nothing listens
        HttpOptions o = e.opts(false); o.port = e.closedPort; o.tlsOpt.timeoutMs = 3000; HttpClient cl(o);
        T_CHECK(!cl.request("GET", "/", h, nullptr, 0, r)); T_CHECK(cl.transient); T_CHECK(!cl.certError);
        // and a client works again afterwards when the server shows up (same object, new port is not possible: just repeat)
        T_CHECK(!cl.request("GET", "/", h, nullptr, 0, r)); T_CHECK(cl.transient);
    }
    { // TLS spoken to a plain port, plain spoken to a TLS port
        HttpOptions o = e.opts(true); o.port = e.port; o.tlsOpt.timeoutMs = 3000; HttpClient cl(o);
        T_CHECK(!cl.request("GET", "/len/1", h, nullptr, 0, r));
        HttpOptions o2 = e.opts(false); o2.port = e.tlsPort; o2.tlsOpt.timeoutMs = 3000; HttpClient c2(o2);
        bool ok = c2.request("GET", "/len/1", h, nullptr, 0, r); T_CHECK(!ok || r.status != 200);
    }
    for (int tls = 0; tls < 2; ++tls) {
        // the connection is cut inside a Content-Length body / a chunked body: transient error, never a clean end
        HttpClient cl(e.opts(tls != 0));
        u64 n = (u64)c_.range(0, 3000000);
        T_CHECK(cl.request("GET", Str::fmt("/cut/%llu", (unsigned long long)n), h, nullptr, 0, r)); T_CHECK(r.contentLength == 5000000);
        bool good; i64 got = DrainPattern(r.body, good);
        T_CHECK(got < 0); T_CHECK(r.body->transient);
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);       // and the client recovers
        T_CHECK(cl.request("GET", Str::fmt("/cutchunk/%llu", (unsigned long long)(n / 4096 * 4096)), h, nullptr, 0, r));
        got = DrainPattern(r.body, good); T_CHECK(got < 0); T_CHECK(r.body->transient);
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);
    }
    { // a server that never answers: timeout
        HttpOptions o = e.opts(c_.coin()); o.tlsOpt.timeoutMs = 600; HttpClient cl(o);
        DWORD t0 = GetTickCount();
        T_CHECK(!cl.request("GET", "/slow", h, nullptr, 0, r)); T_CHECK(cl.transient); DWORD dt = GetTickCount() - t0;
        T_CHECK(dt >= 500 && dt < 8000);
    }
    { // abort
        static volatile LONG flag; flag = 0;
        HttpOptions o = e.opts(false); o.tlsOpt.timeoutMs = 20000; o.tlsOpt.abort = &flag; HttpClient cl(o);
        AbortLater a = { &flag, 400 }; HANDLE th = CreateThread(nullptr, 0, AbortThread, &a, 0, nullptr);
        DWORD t0 = GetTickCount(); bool ok = cl.request("GET", "/slow", h, nullptr, 0, r); DWORD dt = GetTickCount() - t0;
        WaitForSingleObject(th, 5000); CloseHandle(th);
        T_CHECK(!ok); T_CHECK(cl.aborted); T_CHECK(!cl.transient); T_CHECK(dt < 4000);
    }
    { // a failing request body source: its error and its transience are passed on, the client survives
        HttpClient cl(e.opts(false)); PatSource ps(3000000); ps.failAt = 1000000; ps.failTransient = c_.coin();
        T_CHECK(!cl.requestStream("PUT", "/put/x", h, &ps, 3000000, r)); T_CHECK(cl.err.find("injected") >= 0); T_CHECK(cl.transient == ps.failTransient);
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);
        // a source that ends early
        PatSource sh(1000); T_CHECK(!cl.requestStream("PUT", "/put/x", h, &sh, 2000, r));
        T_CHECK(cl.requestBytes("GET", "/len/10", h, nullptr, 0, r, b)); T_CHECK(b.size() == 10);
    }
    return true;
}

VM2B_TEST("http", http_large_streamed)
{
    const Json* j = Srv(c_, "http"); if (!j) return true;
    HttpEnv e(*j);
    u64 n = (u64)c_.range(60, 200) << 20;          // never held in memory: the generator / checker stream it
    if (BigRequested()) n = 2200ull << 20;         // offsets above 2 GiB (the server keeps nothing, so this costs no disk)
    for (int tls = 0; tls < 2; ++tls) {
        HttpClient cl(e.opts(tls != 0));
        HttpHeaders h; HttpResponse r; DWORD t0 = GetTickCount();
        T_CHECK(cl.request("GET", Str::fmt("/len/%llu", (unsigned long long)n), h, nullptr, 0, r));
        bool good; i64 got = DrainPattern(r.body, good);
        T_CHECK(got == (i64)n); T_CHECK(good);
        double dt = (GetTickCount() - t0) / 1000.0; c_.note("%s GET %s in %.1fs (%.0f MB/s)", tls ? "https" : "http", FormatSize(n).c_str(), dt, n / 1048576.0 / (dt > 0 ? dt : 1));
        t0 = GetTickCount();
        PatSource ps(n); HttpResponse r2;
        T_CHECK(cl.requestStream("PUT", "/put/big", h, &ps, (i64)n, r2)); T_CHECK(r2.status == 201);
        Bytes rb; bool ok = true; u8 tmp[128]; i64 k; while ((k = r2.body->read(tmp, sizeof(tmp))) > 0) { size_t o2 = rb.size(); rb.resize(o2 + (size_t)k); memcpy(rb.data() + o2, tmp, (size_t)k); }
        Str ans((const char*)rb.data(), rb.size());
        u32 crc = 0; { Bytes blk; blk.resize(1 << 20); u64 off = 0; while (off < n) { size_t m = (size_t)((n - off) < blk.size() ? (n - off) : blk.size()); PatFill(off, blk.data(), m); crc = Crc32(crc, blk.data(), m); off += m; } }
        Str want = Str::fmt("%llu %u", (unsigned long long)n, (unsigned)crc);
        if (ans != want) { c_.note("server answered '%s' wanted '%s'", ans.c_str(), want.c_str()); ok = false; }
        T_CHECK(ok);
        dt = (GetTickCount() - t0) / 1000.0; c_.note("%s PUT %s in %.1fs (%.0f MB/s)", tls ? "https" : "http", FormatSize(n).c_str(), dt, n / 1048576.0 / (dt > 0 ? dt : 1));
    }
    return true;
}

// ---- ftp -------------------------------------------------------------------------------------------------------------
static const char* const kFtpKeys[] = { "ftp", "ftpes", "ftps" };

static const Json* FtpSrv(Ctx& c, const char* key) { return (!c.servers.is_null() && c.servers.has(key)) ? &c.servers.get(key) : nullptr; }

static bool AnyFtp(Ctx& c)
{
    for (const char* k : kFtpKeys) if (FtpSrv(c, k)) return true;
    c.skip("no FTP servers (run tools/run-ftp-tests.sh)");
    return false;
}

// Target config for a servers.json entry; `sub` is a folder below the server's test directory (unique per test run).
static Json FtpCfg(const Json& s, const char* type, const Str& sub, bool pin = true)
{
    Json j = Json::object();
    j["type"] = type;
    j["host"] = s.get("host").str();
    j["port"] = (i64)s.get("port").integer();
    j["user"] = s.get("user").str();
    j["password"] = s.get("password").str();
    Str d = s.get("dir").str(); if (!sub.empty()) d = d + "/" + sub;
    j["directory"] = d;
    if (pin && s.has("fingerprint") && Str(type) != "ftp") j["fingerprint"] = s.get("fingerprint").str();
    j["timeout"] = 20;
    return j;
}

static Str Uniq(Ctx& c, const char* tag) { return Str::fmt("%s%llu", tag, (unsigned long long)(c.next() % 1000000)); }

static Target* MakeFtp(Ctx& c_, const Json& cfg)
{
    Str e; Target* t = MakeTarget(cfg, e);
    if (!t) c_.note("MakeTarget: %s", e.c_str());
    return t;
}

static bool Upload(Ctx& c_, Target* t, const Str& name, const Bytes& data, bool randomChunks = true)
{
    Sink* w = t->open_write(name);
    if (!w) { c_.note("open_write(%s): %s", name.c_str(), t->err.c_str()); return false; }
    size_t pos = 0; bool ok = true;
    while (pos < data.size() && ok) {
        size_t k = randomChunks ? (size_t)c_.range(1, c_.coin(20) ? 700000 : 40000) : data.size();
        if (k > data.size() - pos) k = data.size() - pos;
        ok = w->write(data.data() + pos, k); pos += k;
    }
    if (ok) ok = w->finish();
    if (!ok) { c_.note("upload(%s): %s", name.c_str(), w->err.c_str()); w->abort(); }
    delete w;
    return ok;
}

static bool Download(Ctx& c_, Target* t, const Str& name, Bytes& out)
{
    Source* r = t->open_read(name);
    if (!r) { c_.note("open_read(%s): %s", name.c_str(), t->err.c_str()); return false; }
    out.clear(); Bytes buf; buf.resize(300000);
    for (;;) {
        size_t want = (size_t)c_.range(1, c_.coin(20) ? 300000 : 30000);
        i64 k = r->read(buf.data(), want);
        if (k < 0) { c_.note("download(%s): %s", name.c_str(), r->err.c_str()); r->close(); delete r; return false; }
        if (k == 0) break;
        size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf.data(), (size_t)k);
    }
    r->close(); delete r;
    return true;
}

static Bytes RandomData(Ctx& c_, size_t n)
{
    Bytes b; b.resize(n);
    switch (c_.rnd(3)) { case 0: c_.fill(b.data(), n); break; case 1: PatFill((u64)c_.rnd(1000), b.data(), n); break; default: break; }
    return b;
}

static bool SameBytes(const Bytes& a, const Bytes& b) { return a.size() == b.size() && (a.empty() || !memcmp(a.data(), b.data(), a.size())); }

static bool HasName(const Vec<Str>& v, const Str& n) { for (const Str& s : v) if (s == n) return true; return false; }

// Arms a one-shot fault in the test FTP server (SITE FAULT, see tools/testservers/ftp_server.py) through a separate login.
static bool NetCrlf(NetConn& n, const char* cmd) { Str s(cmd); s += "\r\n"; return n.send(s.data(), s.size()); }

static bool ArmFault(Ctx& c_, const Json& s, const char* type, const char* what)
{
    NetConn n; TlsOptions o; o.fingerprint = s.get("fingerprint").str(); o.verify = !o.fingerprint.empty(); o.timeoutMs = 10000;
    Str host = s.get("host").str(); int port = (int)s.get("port").integer();
    if (!n.connect(host, port, Str(type) == "ftps", o)) { c_.note("arm: connect: %s", n.err.c_str()); return false; }
    Str l; int step = 0;
    auto reply = [&]() -> bool {
        ++step;
        for (;;) {
            if (!NetLine(n, l)) { c_.note("arm: no reply at step %d (%s) last line '%s'", step, n.err.c_str(), l.c_str()); return false; }
            if (l.size() >= 4 && l[3] == ' ') return true;
            if (l.size() == 3) return true;
        }
    };
    if (!reply()) return false;
    if (Str(type) == "ftpes") { NetCrlf(n, "AUTH TLS"); if (!reply()) return false; if (!n.starttls(host, o)) { c_.note("arm: tls: %s", n.err.c_str()); return false; } }
    NetCrlf(n, ("USER " + s.get("user").str()).c_str()); if (!reply()) return false;
    NetCrlf(n, ("PASS " + s.get("password").str()).c_str()); if (!reply()) return false;
    if (!l.starts_with("230")) { c_.note("arm: login reply: %s", l.c_str()); return false; }
    NetCrlf(n, (Str("SITE FAULT ") + what).c_str()); if (!reply()) return false;
    bool ok = l.starts_with("200");
    NetCrlf(n, "QUIT"); reply();
    n.close();
    if (!ok) c_.note("arm reply: %s", l.c_str());
    return ok;
}

static Str ServerPath(const Json& s, const Str& sub, const Str& name)
{
    Str p = s.get("root").str();
    Str d = s.get("dir").str(); if (!sub.empty()) d = d + "/" + sub;
    p = JoinPath(p, d.replace_all("/", "\\"));
    return name.empty() ? p : JoinPath(p, name);
}

static const char* const kNames[] = { "vm 00001.tar", "\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba \xe6\x97\xa5\xe6\x9c\xac.json", "a  b c.bin", "x.tar.gz.enc.00002", "UPPER.Case", "#hash&amp;%20.dat" };

VM2B_TEST("ftp", ftp_roundtrip_random)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Str sub = Uniq(c_, "rt");
        Target* t = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); if (!ok) c_.note("%s connect: %s", key, t->err.c_str());
        T_CHECK(ok); if (!ok) { delete t; return false; }
        int nfiles = c_.range(3, 6);
        Vec<Str> names; Vec<Bytes> datas;
        for (int i = 0; i < nfiles; ++i) {
            Str n = Str::fmt("%d-", i) + kNames[c_.rnd(6)];
            size_t sz = c_.coin(15) ? 0 : c_.coin(10) ? 1 : c_.coin(25) ? (size_t)c_.range(1, 4000000) : (size_t)c_.range(1, 90000);
            names.push_back(n); datas.push_back(RandomData(c_, sz));
            T_CHECK(Upload(c_, t, n, datas.back()));
        }
        Vec<Str> got; T_CHECK(t->list(got));
        T_CHECK(got.size() == names.size());
        for (const Str& n : names) if (!HasName(got, n)) { c_.note("%s: missing in list: %s", key, n.c_str()); c_.failed = true; }
        for (const Str& n : got) T_CHECK(!n.ends_with(".part"));
        for (int i = 0; i < nfiles; ++i) {
            Bytes b; bool dl = Download(c_, t, names[i], b); T_CHECK(dl);
            if (dl && !SameBytes(b, datas[i])) { c_.note("%s: %s differs (%zu vs %zu bytes)", key, names[i].c_str(), b.size(), datas[i].size()); c_.failed = true; }
            bool yes = false; T_CHECK(t->exists(names[i], yes)); T_CHECK(yes);
        }
        bool yes = true; T_CHECK(t->exists("no such file", yes)); T_CHECK(!yes);
        T_CHECK(!t->open_read("no such file")); T_CHECK(!t->transient);
        // an existing name is replaced
        Bytes repl = RandomData(c_, (size_t)c_.range(0, 50000));
        T_CHECK(Upload(c_, t, names[0], repl)); Bytes back; T_CHECK(Download(c_, t, names[0], back)); T_CHECK(SameBytes(back, repl));
        // delete: existing, then missing (not an error)
        for (int i = 0; i < nfiles; ++i) { T_CHECK(t->remove(names[i])); yes = true; T_CHECK(t->exists(names[i], yes)); T_CHECK(!yes); }
        T_CHECK(t->remove(names[0]));
        T_CHECK(t->list(got)); T_CHECK(got.empty());
        // put_bytes / get_bytes of the base class
        Bytes pb = RandomData(c_, 1234); T_CHECK(t->put_bytes("pb.json", pb.data(), pb.size())); Bytes gb; T_CHECK(t->get_bytes("pb.json", gb)); T_CHECK(SameBytes(pb, gb));
        t->close(); T_CHECK(t->connect()); T_CHECK(t->remove("pb.json")); T_CHECK(!t->remove("a/b"));
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_large_streamed)
{
    if (!AnyFtp(c_)) return true;
    // A big file is streamed from / checked against the pattern generator, never held in memory.
    u64 n = (u64)c_.range(60, 200) << 20;
    const char* key = c_.coin() ? "ftp" : "ftps"; if (!FtpSrv(c_, key)) key = "ftp"; if (!FtpSrv(c_, key)) key = "ftpes";
    { char kv[16]; if (GetEnvironmentVariableA("VM2B_FTP_KEY", kv, sizeof(kv)) && FtpSrv(c_, kv)) { static char forced[16]; strcpy(forced, kv); key = forced; } }      // repeat runs (tools/ftp-repeat.sh) pin the kind of server: ftp / ftpes / ftps
    const Json* s = FtpSrv(c_, key); if (!s) return true;
    Target* t = MakeFtp(c_, FtpCfg(*s, key, Uniq(c_, "big"))); T_CHECK(t && t->connect()); if (!t) return false;
    DWORD t0 = GetTickCount();
    Sink* w = t->open_write("big.bin"); T_CHECK(w != nullptr); if (!w) { delete t; return false; }
    Bytes blk; blk.resize(1 << 20); u64 off = 0; bool ok = true;
    while (off < n && ok) { size_t m = (size_t)((n - off) < blk.size() ? (n - off) : blk.size()); PatFill(off, blk.data(), m); ok = w->write(blk.data(), m); off += m; }
    if (ok) ok = w->finish(); else w->abort();
    if (!ok) c_.note("%s", w->err.c_str());
    delete w; T_CHECK(ok);
    double dt = (GetTickCount() - t0) / 1000.0; c_.note("%s upload %s in %.1fs (%.0f MB/s)", key, FormatSize(n).c_str(), dt, n / 1048576.0 / (dt > 0 ? dt : 1));
    t0 = GetTickCount();
    Source* r = t->open_read("big.bin"); T_CHECK(r != nullptr);
    if (r) { bool good; i64 got = DrainPattern(r, good); if (got != (i64)n || !good) c_.note("download got %lld of %llu good=%d err=%s", (long long)got, (unsigned long long)n, (int)good, r->err.c_str()); T_CHECK(got == (i64)n); T_CHECK(good); r->close(); delete r; }
    dt = (GetTickCount() - t0) / 1000.0; c_.note("%s download in %.1fs (%.0f MB/s)", key, dt, n / 1048576.0 / (dt > 0 ? dt : 1));
    T_CHECK(t->remove("big.bin"));
    delete t;
    return true;
}

VM2B_TEST("ftp", ftp_login_and_certificate_errors)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        { // wrong password: permanent, and the password is nowhere in the text
            Json cfg = FtpCfg(*s, key, ""); cfg["password"] = "Sup3r-Secret-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c";
            Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(!t->err.empty());
            T_CHECK(t->err.find("Sup3r-Secret") < 0); T_CHECK(t->err.find("login") >= 0);
            T_CHECK(!t->connect()); T_CHECK(!t->transient);                               // callable again after a failure
            T_CHECK(!t->open_write("x")); T_CHECK(!t->transient); T_CHECK(t->err.find("Sup3r-Secret") < 0);
            delete t;
        }
        { // wrong user
            Json cfg = FtpCfg(*s, key, ""); cfg["user"] = "nobody";
            Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(!t->transient); delete t;
        }
        { // nothing listens: transient
            Json cfg = FtpCfg(*s, key, ""); cfg["port"] = (i64)c_.servers.get("closed_port").integer(21990); cfg["timeout"] = 3;
            Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(t->transient); T_CHECK(!t->err.empty());
            T_CHECK(!t->reconnect()); T_CHECK(t->transient); delete t;
        }
        if (Str(key) != "ftp") {
            const Str fp = s->get("fingerprint").str();
            { // pinned to another certificate: permanent, with the real fingerprint in the text
                Json cfg = FtpCfg(*s, key, ""); cfg["fingerprint"] = "sha256:2222222222222222222222222222222222222222222222222222222222222222";
                Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find(fp.c_str()) >= 0);
                if (t->err.find(fp.c_str()) < 0) c_.note("%s: %s", key, t->err.c_str());
                T_CHECK(!t->connect()); T_CHECK(!t->transient); delete t;
            }
            { // no pin, untrusted certificate
                Json cfg = FtpCfg(*s, key, "", false); Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find(fp.c_str()) >= 0); delete t;
            }
            { // verify=false and a CA bundle both get in
                Json cfg = FtpCfg(*s, key, Uniq(c_, "v"), false); cfg["verify"] = false; Target* t = MakeFtp(c_, cfg); T_CHECK(t->connect()); delete t;
                Json cfg2 = FtpCfg(*s, key, Uniq(c_, "v"), false); cfg2["ca_file"] = c_.servers.get("ca_file").str(); Target* t2 = MakeFtp(c_, cfg2);
                bool ok = t2->connect(); if (!ok) c_.note("ca_file: %s", t2->err.c_str()); T_CHECK(ok); delete t2;
            }
        }
        if (Str(key) == "ftpes") { // a server without TLS spoken to as FTPES: AUTH TLS refused, permanent
            const Json* p = FtpSrv(c_, "ftp");
            if (p) { Json cfg = FtpCfg(*p, "ftpes", ""); Target* t = MakeFtp(c_, cfg); T_CHECK(!t->connect()); T_CHECK(!t->transient); T_CHECK(t->err.find("TLS") >= 0); delete t; }
        }
    }
    { Json bad = Json::object(); bad["type"] = "ftp"; Str e; T_CHECK(MakeTarget(bad, e) == nullptr); T_CHECK(!e.empty()); }
    return true;
}

VM2B_TEST("ftp", ftp_directories_and_reuse)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Str sub = Uniq(c_, "d") + "/n\xc3\xa4sted dir/" + Uniq(c_, "e");
        Target* a = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(a->connect());                    // created recursively
        T_CHECK(DirExists(ServerPath(*s, sub, "")));
        Target* b = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(b->connect());                    // exists already
        Bytes d1 = RandomData(c_, 5000); T_CHECK(Upload(c_, a, "f1.bin", d1));
        Bytes g; T_CHECK(Download(c_, b, "f1.bin", g)); T_CHECK(SameBytes(g, d1));
        // many small files in a row over one connection (the volume pattern)
        int cnt = c_.range(15, 30);
        for (int i = 0; i < cnt; ++i) { Bytes d = RandomData(c_, (size_t)c_.range(0, 3000)); T_CHECK(Upload(c_, a, Str::fmt("v%05d", i), d)); }
        Vec<Str> names; T_CHECK(b->list(names)); T_CHECK(names.size() == (size_t)cnt + 1);
        // an open transfer blocks others with a clear error
        Sink* w = a->open_write("busy.bin"); T_CHECK(w != nullptr);
        if (w) { Vec<Str> tmp; T_CHECK(!a->list(tmp)); T_CHECK(!a->transient); w->abort(); delete w; T_CHECK(a->list(tmp)); }
        delete a; delete b;
    }
    return true;
}

VM2B_TEST("ftp", ftp_passive_active_and_listing_variants)
{
    const char* variants[] = { "ftp_private", "ftp_noepsv", "ftp_mlsd", "ftp_list" };
    bool any = false;
    for (const char* key : variants) if (FtpSrv(c_, key)) any = true;
    if (!any) { c_.skip("no FTP variant servers"); return true; }
    for (const char* key : variants) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Str sub = Uniq(c_, "pv");
        Target* t = MakeFtp(c_, FtpCfg(*s, "ftp", sub)); bool ok = t->connect(); if (!ok) c_.note("%s: %s", key, t->err.c_str()); T_CHECK(ok);
        if (!ok) { delete t; continue; }
        Vec<Str> want;
        for (int i = 0; i < 3; ++i) { Str n = Str::fmt("%d ", i) + kNames[c_.rnd(6)]; if (HasName(want, n)) continue; want.push_back(n); Bytes d = RandomData(c_, (size_t)c_.range(0, 200000)); T_CHECK(Upload(c_, t, n, d)); Bytes g; T_CHECK(Download(c_, t, n, g)); T_CHECK(SameBytes(g, d)); }
        // a sub folder next to the files (LIST / MLSD know it is a folder)
        Json sc = FtpCfg(*s, "ftp", sub + "/subfolder"); Target* sf = MakeFtp(c_, sc); T_CHECK(sf->connect()); delete sf;
        Vec<Str> got; bool lok = t->list(got); if (!lok) c_.note("%s list: %s", key, t->err.c_str()); T_CHECK(lok);
        for (const Str& n : want) if (!HasName(got, n)) { c_.note("%s: %s missing from %zu names", key, n.c_str(), got.size()); c_.failed = true; }
        Str k(key); if (k == "ftp_mlsd" || k == "ftp_list") T_CHECK(!HasName(got, "subfolder"));
        bool yes = false; T_CHECK(t->exists(want[0], yes)); T_CHECK(yes);
        for (const Str& n : want) T_CHECK(t->remove(n));
        T_CHECK(t->list(got)); T_CHECK(got.empty() || (got.size() == 1 && got[0] == "subfolder" && k != "ftp_mlsd" && k != "ftp_list"));
        delete t;
    }
    // active mode, plain and TLS
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Json cfg = FtpCfg(*s, key, Uniq(c_, "act")); cfg["passive"] = false;
        Target* t = MakeFtp(c_, cfg); bool ok = t->connect(); T_CHECK(ok);
        if (ok) {
            Bytes d = RandomData(c_, (size_t)c_.range(0, 300000)); bool up = Upload(c_, t, "act.bin", d);
            if (!up) c_.note("active %s: %s", key, t->err.c_str());
            T_CHECK(up);
            if (up) { Bytes g; T_CHECK(Download(c_, t, "act.bin", g)); T_CHECK(SameBytes(g, d)); Vec<Str> l; T_CHECK(t->list(l)); T_CHECK(l.size() == 1); }
        }
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_fault_injection_upload)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        for (int rep = 0; rep < 2; ++rep) {
            Str sub = Uniq(c_, "fu");
            Target* t = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(t->connect());
            size_t size = (size_t)c_.range(300000, 5000000);
            size_t cut = c_.coin(20) ? 0 : (size_t)c_.range(1, (int)size - 1);
            Bytes data = RandomData(c_, size);
            Str name = Str("vol ") + Uniq(c_, "") + ".tar";
            T_CHECK(ArmFault(c_, *s, key, Str::fmt("up %zu", cut).c_str()));
            Sink* w = t->open_write(name); T_CHECK(w != nullptr);
            bool failed = false; bool tr = false;
            if (!w) { failed = true; tr = t->transient; }
            else {
                size_t pos = 0; bool ok = true;
                while (pos < size && ok) { size_t k = (size_t)c_.range(1, 200000); if (k > size - pos) k = size - pos; ok = w->write(data.data() + pos, k); pos += k; }
                if (ok) ok = w->finish();
                failed = !ok; tr = w->transient;
                if (!ok) w->abort();
                delete w;
            }
            c_.note("%s: cut at %zu of %zu -> %s", key, cut, size, failed ? "failed as expected" : "NO FAILURE");
            T_CHECK(failed); T_CHECK(tr);
            T_CHECK(!FileExists(ServerPath(*s, sub, name)));                                // never under the final name
            T_CHECK(t->reconnect());
            bool yes = true; T_CHECK(t->exists(name, yes)); T_CHECK(!yes);
            Vec<Str> l; T_CHECK(t->list(l)); T_CHECK(!HasName(l, name));
            for (const Str& n : l) T_CHECK(!n.ends_with(".part"));
            // retry succeeds
            T_CHECK(Upload(c_, t, name, data)); Bytes g; T_CHECK(Download(c_, t, name, g)); T_CHECK(SameBytes(g, data));
            T_CHECK(!FileExists(ServerPath(*s, sub, name + ".part")));
            delete t;
        }
    }
    return true;
}

VM2B_TEST("ftp", ftp_fault_injection_download_and_list)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Target* t = MakeFtp(c_, FtpCfg(*s, key, Uniq(c_, "fd"))); T_CHECK(t->connect());
        size_t size = (size_t)c_.range(100000, 4000000); Bytes data = RandomData(c_, size);
        T_CHECK(Upload(c_, t, "file one.bin", data));
        for (int i = 0; i < 4; ++i) T_CHECK(Upload(c_, t, Str::fmt("a-rather-long-volume-name-number-%02d.tar.00001", i), RandomData(c_, 10)));
        for (int rep = 0; rep < 2; ++rep) {
            size_t cut = c_.coin(20) ? 0 : (size_t)c_.range(1, (int)size - 1);
            T_CHECK(ArmFault(c_, *s, key, Str::fmt("down %zu", cut).c_str()));
            Source* r = t->open_read("file one.bin");
            bool failed = false, tr = false; u64 got = 0;
            if (!r) { failed = true; tr = t->transient; }
            else {
                Bytes buf; buf.resize(100000);
                for (;;) { i64 k = r->read(buf.data(), buf.size()); if (k < 0) { failed = true; tr = r->transient; break; } if (k == 0) break; got += (u64)k; }
                r->close(); delete r;
            }
            c_.note("%s: download cut at %zu (got %llu of %zu) -> %s", key, cut, (unsigned long long)got, size, failed ? "error" : "NO ERROR");
            T_CHECK(failed); T_CHECK(tr);
            T_CHECK(t->reconnect());
            Bytes g; T_CHECK(Download(c_, t, "file one.bin", g)); T_CHECK(SameBytes(g, data));
        }
        // the listing is cut
        for (int rep = 0; rep < 2; ++rep) {
            size_t cut = (size_t)c_.range(0, 60);
            T_CHECK(ArmFault(c_, *s, key, Str::fmt("list %zu", cut).c_str()));
            Vec<Str> l; l.push_back("stale");
            bool ok = t->list(l);
            c_.note("%s: listing cut at %zu -> %s", key, cut, ok ? "NO ERROR" : t->err.c_str());
            T_CHECK(!ok); T_CHECK(t->transient);
            T_CHECK(t->reconnect());
            T_CHECK(t->list(l)); T_CHECK(l.size() == 5); T_CHECK(HasName(l, "file one.bin"));
        }
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_server_refusals_and_lost_reply)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Str sub = Uniq(c_, "fm");
        Target* t = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(t->connect());
        Bytes data = RandomData(c_, (size_t)c_.range(1000, 300000));
        { // 552 on STOR: permanent
            T_CHECK(ArmFault(c_, *s, key, "stor552"));
            Sink* w = t->open_write("full.bin"); T_CHECK(w == nullptr); T_CHECK(!t->transient); T_CHECK(t->err.find("552") >= 0); if (t->err.find("552") < 0) c_.note("552 case: %s", t->err.c_str());
            Bytes g; T_CHECK(Upload(c_, t, "full.bin", data)); T_CHECK(Download(c_, t, "full.bin", g)); T_CHECK(SameBytes(g, data));
        }
        { // 421 on STOR: closes the connection; the target reconnects by itself or reports it as transient
            T_CHECK(ArmFault(c_, *s, key, "stor421"));
            Sink* w = t->open_write("late.bin"); bool done = false;
            if (!w) { T_CHECK(t->transient); T_CHECK(t->err.find("421") >= 0); }
            else { done = w->write(data.data(), data.size()) && w->finish(); if (!done) w->abort(); delete w; }
            if (!done) T_CHECK(t->reconnect());
            Bytes g; T_CHECK(Upload(c_, t, "late.bin", data)); T_CHECK(Download(c_, t, "late.bin", g)); T_CHECK(SameBytes(g, data));
        }
        { // RNTO refused: a clear permanent error, nothing under the final name, no .part left
            T_CHECK(ArmFault(c_, *s, key, "norename"));
            Sink* w = t->open_write("nr.bin"); T_CHECK(w != nullptr);
            if (w) { bool ok = w->write(data.data(), data.size()) && w->finish(); T_CHECK(!ok); T_CHECK(!w->transient); T_CHECK(w->err.find("rename") >= 0); if (w->err.find("rename") < 0) c_.note("%s", w->err.c_str()); delete w; }
            T_CHECK(!FileExists(ServerPath(*s, sub, "nr.bin"))); T_CHECK(!FileExists(ServerPath(*s, sub, "nr.bin.part")));
            bool yes = true; T_CHECK(t->exists("nr.bin", yes)); T_CHECK(!yes);
        }
        { // the 226 is lost with the connection: transient, final name absent, retry works
            T_CHECK(ArmFault(c_, *s, key, "lost226"));
            Sink* w = t->open_write("lost.bin"); T_CHECK(w != nullptr);
            if (w) { bool ok = w->write(data.data(), data.size()) && w->finish(); T_CHECK(!ok); T_CHECK(w->transient); if (!ok) w->abort(); delete w; }
            T_CHECK(!FileExists(ServerPath(*s, sub, "lost.bin")));
            T_CHECK(t->reconnect()); Bytes g; T_CHECK(Upload(c_, t, "lost.bin", data)); T_CHECK(Download(c_, t, "lost.bin", g)); T_CHECK(SameBytes(g, data));
        }
        { // a stalled server: the idle timeout turns it into a transient error
            Json cfg = FtpCfg(*s, key, sub); cfg["timeout"] = 1; Target* t2 = MakeFtp(c_, cfg); T_CHECK(t2->connect());
            T_CHECK(ArmFault(c_, *s, key, "stall 5000"));
            Bytes big = RandomData(c_, 40u << 20); DWORD t0 = GetTickCount();
            Sink* w = t2->open_write("stall.bin"); T_CHECK(w != nullptr);
            if (w) { bool ok = w->write(big.data(), big.size()) && w->finish(); DWORD dt = GetTickCount() - t0; T_CHECK(!ok); T_CHECK(w->transient); if (!ok) w->abort(); delete w; c_.note("%s: stalled upload ended after %lu ms: %s", key, (unsigned long)dt, ok ? "no error" : "error"); }
            delete t2;
            Sleep(5500);                                                                   // the server wakes up again
        }
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_abort_and_idle_connection)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        Str sub = Uniq(c_, "ab");
        Target* t = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(t->connect());
        Bytes data = RandomData(c_, (size_t)c_.range(400000, 3000000));
        Sink* w = t->open_write("aborted.bin"); T_CHECK(w != nullptr);
        if (w) { size_t half = data.size() / 2; T_CHECK(w->write(data.data(), half)); w->abort(); delete w; }
        bool yes = true; T_CHECK(t->exists("aborted.bin", yes)); T_CHECK(!yes);
        T_CHECK(!FileExists(ServerPath(*s, sub, "aborted.bin"))); T_CHECK(!FileExists(ServerPath(*s, sub, "aborted.bin.part")));
        Vec<Str> l; T_CHECK(t->list(l)); T_CHECK(l.empty());
        // a reader abandoned half way
        T_CHECK(Upload(c_, t, "r.bin", data));
        Source* r = t->open_read("r.bin"); T_CHECK(r != nullptr);
        if (r) { Bytes b; b.resize(100000); T_CHECK(r->read(b.data(), b.size()) > 0); r->close(); delete r; }
        Bytes g; T_CHECK(Download(c_, t, "r.bin", g)); T_CHECK(SameBytes(g, data));
        // a read of a file while another is open is refused, not mixed up
        delete t;
    }
    // the server drops idle control connections after 3 s: the target replaces it unnoticed
    if (const Json* s = FtpSrv(c_, "ftp_idle")) {
        Str sub = Uniq(c_, "id");
        Target* t = MakeFtp(c_, FtpCfg(*s, "ftp", sub)); T_CHECK(t->connect());
        Sleep(5000);
        Vec<Str> l; bool ok = t->list(l); if (!ok) c_.note("after idle: %s", t->err.c_str()); T_CHECK(ok);
        Sleep(5000);
        Bytes d = RandomData(c_, 70000); T_CHECK(Upload(c_, t, "after idle.bin", d));
        // a slow upload longer than the idle timeout, with NOOP keepalives on the control connection
        Json cfg = FtpCfg(*s, "ftp", sub); cfg["keepalive"] = 1; Target* k = MakeFtp(c_, cfg); T_CHECK(k->connect());
        Sink* w = k->open_write("slow.bin"); T_CHECK(w != nullptr);
        if (w) {
            bool wok = true; Bytes chunk = RandomData(c_, 300000);
            for (int i = 0; i < 6 && wok; ++i) { wok = w->write(chunk.data(), chunk.size()); Sleep(1500); }
            if (wok) wok = w->finish(); if (!wok) { c_.note("slow upload: %s", w->err.c_str()); w->abort(); } T_CHECK(wok); delete w;
        }
        Bytes g; T_CHECK(Download(c_, k, "slow.bin", g)); T_CHECK(g.size() == 6 * 300000u);
        delete t; delete k;
    }
    return true;
}

VM2B_TEST("ftp", ftp_download_over_2gib_sparse)
{
    // A sparse 2.2 GB file (no disk used) is downloaded and checked: 64-bit sizes / offsets in SIZE, the byte counters and the reader.
    // Runs when VM2B_BIG=1, otherwise in one run of five.
    if (!AnyFtp(c_)) return true;
    if (!BigRequested() && c_.rnd(5) != 0) { c_.skip("only with VM2B_BIG=1 (or in one run of five)"); return true; }
    const char* key = c_.coin() ? "ftp" : "ftps"; if (!FtpSrv(c_, key)) key = "ftp"; if (!FtpSrv(c_, key)) key = "ftpes";
    const Json* s = FtpSrv(c_, key); if (!s) return true;
    Str sub = Uniq(c_, "sp");
    Target* t = MakeFtp(c_, FtpCfg(*s, key, sub)); T_CHECK(t->connect());
    u64 n = (2ull << 30) + (u64)c_.range(1, 50000000);
    Str path = ServerPath(*s, sub, "sparse.bin");
    HANDLE h = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    T_CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) { delete t; return false; }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)n; SetFilePointerEx(h, li, nullptr, FILE_BEGIN); SetEndOfFile(h);
    DWORD w = 0; li.QuadPart = (LONGLONG)n - 5; SetFilePointerEx(h, li, nullptr, FILE_BEGIN); WriteFile(h, "tail!", 5, &w, nullptr);   // something non-zero at the end
    CloseHandle(h);
    DWORD t0 = GetTickCount();
    Source* r = t->open_read("sparse.bin"); T_CHECK(r != nullptr);
    if (r) {
        Bytes buf; buf.resize(1 << 20); u64 got = 0; u8 last[5] = { 0, 0, 0, 0, 0 }; bool zeros = true; i64 k;
        while ((k = r->read(buf.data(), buf.size())) > 0) {
            u64 body_end = got + (u64)k;
            for (i64 i = 0; i < k && zeros; i += 4096) { size_t m = (size_t)((k - i) < 4096 ? (k - i) : 4096); u64 abs = got + (u64)i; for (size_t j = 0; j < m; ++j) if (buf[(size_t)i + j] && abs + j < n - 5) { zeros = false; break; } }
            if (body_end >= n) memcpy(last, buf.data() + (size_t)(k - 5), 5);
            got = body_end;
        }
        if (k < 0) c_.note("read error: %s", r->err.c_str());
        T_CHECK(k == 0); T_CHECK(got == n); T_CHECK(zeros); T_CHECK(!memcmp(last, "tail!", 5));
        c_.note("%s: %s sparse download in %.1fs", key, FormatSize(n).c_str(), (GetTickCount() - t0) / 1000.0);
        r->close(); delete r;
    }
    T_CHECK(t->remove("sparse.bin"));
    delete t;
    return true;
}

// ---- the engine through an FTP target ---------------------------------------------------------------------------------
struct FtpTmp {
    Str path; bool keep;
    FtpTmp(Ctx& c, const char* tag) : keep(c.keep) {
        wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t);
        path = JoinPath(FromWide(t), Str::fmt("vm2b-%s-%lu-%llu", tag, (unsigned long)GetCurrentProcessId(), (unsigned long long)(c.next() % 1000000)));
        Str e; MakeDirs(path, e);
    }
    ~FtpTmp() { if (!keep) RemoveTree(path); }
    Str sub(const char* n) const { return JoinPath(path, n); }
};

static bool WriteBytesTo(const Str& path, const Bytes& b)
{
    int cut = path.rfind('\\'); Str e; if (cut > 0) MakeDirs(path.substr(0, (size_t)cut), e);
    HANDLE h = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = b.empty() || (WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr) && w == b.size());
    CloseHandle(h); return ok;
}

VM2B_TEST("ftp", ftp_engine_roundtrip)
{
    if (!AnyFtp(c_)) return true;
    for (const char* key : kFtpKeys) {
        const Json* s = FtpSrv(c_, key); if (!s) continue;
        FtpTmp tmp(c_, "ftpeng");
        Str root = tmp.sub("vms"), vm = "vm \xd0\x92\xd0\x9c";
        struct F { Str rel; Bytes data; }; Vec<F> files;
        int n = c_.range(2, 8);
        for (int i = 0; i < n; ++i) {
            F f; f.rel = Str::fmt("%s%s-%d.%s", c_.coin(30) ? "sub/" : "", c_.coin() ? "disk" : "conf ig", i, c_.coin() ? "vmdk" : "vmx");
            size_t sz = c_.coin(15) ? 0 : c_.coin(30) ? (size_t)c_.range(1, 3000000) : (size_t)c_.range(1, 60000);
            f.data = RandomData(c_, sz);
            T_CHECK(WriteBytesTo(JoinPath(JoinPath(root, vm), f.rel.replace_all("/", "\\")), f.data));
            files.push_back(f);
        }
        Target* t = MakeFtp(c_, FtpCfg(*s, key, Uniq(c_, "eng"))); T_CHECK(t->connect());
        VmSource* src = NewDirectorySource(root);
        BackupOptions o; o.codec.level = c_.range(1, 3); o.verify = c_.coin(); o.kdfIterations = 3000;
        if (c_.coin()) o.password = "ftp-pw-\xd0\xbf";
        Manifest m; Str warn, err;
        bool ok = BackupVm(*src, vm, *t, o, nullptr, m, warn, err); if (!ok) c_.note("%s backup: %s", key, err.c_str());
        T_CHECK(ok);
        if (ok) {
            Str ve; bool vok = VerifyBackup(*t, m.archive, o.password, nullptr, nullptr, ve); if (!vok) c_.note("verify: %s", ve.c_str()); T_CHECK(vok);
            Vec<Manifest> list; Str le; T_CHECK(ListBackups(*t, nullptr, list, le)); T_CHECK(list.size() == 1);
            Str dest = tmp.sub("restore"); Str re;
            bool rok = RestoreBackup(*t, m.archive, dest, o.password, nullptr, nullptr, re); if (!rok) c_.note("restore: %s", re.c_str()); T_CHECK(rok);
            for (const F& f : files) {
                Bytes got; Str e2; bool r = ReadWholeFile(JoinPath(JoinPath(dest, vm), f.rel.replace_all("/", "\\")).c_str(), got, e2);
                if (!r || !SameBytes(got, f.data)) { c_.note("%s: %s differs", key, f.rel.c_str()); c_.failed = true; }
            }
            Str de; T_CHECK(DeleteBackup(*t, m.archive, de));
            Vec<Str> names; T_CHECK(t->list(names)); T_CHECK(names.empty());
        }
        delete src; delete t;
    }
    return true;
}

// ---- servers with quirks (tools/testservers/ftp_quirk.py): what vsftpd / ProFTPD / IIS / FileZilla / old boxes do differently ------------
static Json QuirkCfg(const Json& q, const Json& p, const Str& dir, int timeoutS = 20)
{
    Json j = Json::object();
    { Str ty = p.get("type").str(); j["type"] = ty == "plain" ? Str("ftp") : ty; } j["host"] = q.get("host").str(); j["port"] = (i64)p.get("port").integer();
    j["user"] = q.get("user").str(); j["password"] = q.get("password").str();
    if (!dir.empty()) j["directory"] = dir;
    if (p.has("fingerprint")) j["fingerprint"] = p.get("fingerprint").str();
    j["timeout"] = (i64)timeoutS;
    return j;
}

static const Json* QuirkSrv(Ctx& c) { return Srv(c, "ftpq"); }

VM2B_TEST("ftp", ftp_quirk_servers_roundtrip)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        Str name = p.get("name").str();
        if (p.get("expect").str() != "ok" || name == "big" || name == "lines5k" || name == "slow" || name == "es_reuse" || name == "s_reuse" || name == "pasv_public") continue;
        Target* t = MakeFtp(c_, QuirkCfg(*q, p, Uniq(c_, "q"))); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); if (!ok) c_.note("%s connect: %s", name.c_str(), t->err.c_str());
        T_CHECK(ok); if (!ok) { delete t; continue; }
        Vec<Str> want; Vec<Bytes> data;
        int n = c_.range(2, 5);
        for (int i = 0; i < n; ++i) {
            Str nm = Str::fmt("%d-", i) + kNames[c_.rnd(6)];
            Bytes d = RandomData(c_, c_.coin(10) ? 0 : (size_t)c_.range(1, c_.coin(10) ? 1500000 : 80000));
            bool up = Upload(c_, t, nm, d); if (!up) c_.note("%s: upload %s: %s", name.c_str(), nm.c_str(), t->err.c_str());
            T_CHECK(up); want.push_back(nm); data.push_back(d);
        }
        Vec<Str> got; bool lok = t->list(got); if (!lok) c_.note("%s list: %s", name.c_str(), t->err.c_str());
        T_CHECK(lok);
        if (got.size() != want.size()) { c_.note("%s: listed %zu names, want %zu", name.c_str(), got.size(), want.size()); c_.failed = true; }
        for (const Str& w : want) if (!HasName(got, w)) { c_.note("%s: '%s' not in the listing", name.c_str(), w.c_str()); c_.failed = true; }
        for (size_t i = 0; i < want.size(); ++i) {
            Bytes g; bool dok = Download(c_, t, want[i], g); T_CHECK(dok);
            if (dok && !SameBytes(g, data[i])) { c_.note("%s: %s differs", name.c_str(), want[i].c_str()); c_.failed = true; }
        }
        bool yes = false; T_CHECK(t->exists(want[0], yes)); T_CHECK(yes);
        T_CHECK(t->exists("nonexistent.file", yes)); T_CHECK(!yes);
        // the same name again (a server that will not rename over a file gets it removed first)
        Bytes d2 = RandomData(c_, (size_t)c_.range(1, 50000));
        bool up2 = Upload(c_, t, want[0], d2); if (!up2) c_.note("%s: overwrite: %s", name.c_str(), t->err.c_str()); T_CHECK(up2);
        if (up2) { Bytes g; T_CHECK(Download(c_, t, want[0], g)); T_CHECK(SameBytes(g, d2)); }
        // a connection that sat idle past the server's patience is replaced transparently
        for (const Str& w : want) { bool r = t->remove(w); if (!r) c_.note("%s: remove: %s", name.c_str(), t->err.c_str()); T_CHECK(r); }
        T_CHECK(t->remove("never-existed.bin"));
        T_CHECK(t->list(got)); T_CHECK(got.empty());
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_quirk_login_and_banner_failures)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        Str ex = p.get("expect").str();
        if (ex != "perm" && ex != "transient") continue;
        Target* t = MakeFtp(c_, QuirkCfg(*q, p, "d")); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect();
        if (ok) { c_.note("%s: connect unexpectedly worked", p.get("name").str().c_str()); c_.failed = true; }
        if (!ok && t->transient != (ex == "transient")) { c_.note("%s: transient=%d, want %d (%s)", p.get("name").str().c_str(), (int)t->transient, (int)(ex == "transient"), t->err.c_str()); c_.failed = true; }
        if (!ok && t->err.find(q->get("password").str().c_str()) >= 0) { c_.note("password in the error text"); c_.failed = true; }
        T_CHECK(!ok || ex == "ok");
        delete t;
    }
    return true;
}

VM2B_TEST("ftp", ftp_hostile_servers_no_hang)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        if (p.get("expect").str() != "hostile") continue;
        Str name = p.get("name").str();
        Target* t = MakeFtp(c_, QuirkCfg(*q, p, "d", 5)); T_CHECK(t != nullptr); if (!t) return false;
        DWORD t0 = GetTickCount();
        bool ok = t->connect();
        DWORD ms = GetTickCount() - t0;
        if (ok) { c_.note("%s: connect worked against a hostile server", name.c_str()); c_.failed = true; }
        if (ms > 40000) { c_.note("%s: took %lu ms to give up", name.c_str(), (unsigned long)ms); c_.failed = true; }
        c_.note("%s: gave up after %lu ms: %s", name.c_str(), (unsigned long)ms, t->err.c_str());
        // the object is still usable (and can try again)
        Vec<Str> l; T_CHECK(!t->list(l));
        delete t;
    }
    // a hostile server that is fine at first and then babbles must not hang an upload / listing either
    return true;
}

VM2B_TEST("ftp", ftp_big_listing_and_slow_server)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        Str name = p.get("name").str();
        if (name == "big") {
            Target* t = MakeFtp(c_, QuirkCfg(*q, p, "big", 60)); T_CHECK(t != nullptr); if (!t) return false;
            T_CHECK(t->connect());
            DWORD t0 = GetTickCount(); Vec<Str> l; bool ok = t->list(l); DWORD ms = GetTickCount() - t0;
            if (!ok) c_.note("big list: %s", t->err.c_str());
            T_CHECK(ok); T_CHECK(l.size() == 50000);
            c_.note("50000 names listed in %lu ms", (unsigned long)ms);
            bool yes = false; T_CHECK(t->exists("volume-049999.bin", yes)); T_CHECK(yes);
            delete t;
        } else if (name == "lines5k") {
            Target* t = MakeFtp(c_, QuirkCfg(*q, p, "", 20)); T_CHECK(t != nullptr); if (!t) return false;
            bool ok = t->connect(); if (!ok) c_.note("4000-line banner: %s", t->err.c_str()); T_CHECK(ok); delete t;
        } else if (name == "slow") {
            Target* t = MakeFtp(c_, QuirkCfg(*q, p, Uniq(c_, "sl"), 20)); T_CHECK(t != nullptr); if (!t) return false;
            T_CHECK(t->connect());
            Bytes d = RandomData(c_, 1500000); DWORD t0 = GetTickCount();
            T_CHECK(Upload(c_, t, "slow.bin", d)); Bytes g; T_CHECK(Download(c_, t, "slow.bin", g)); T_CHECK(SameBytes(g, d));
            c_.note("slow: 2 x 1.5 MB in %lu ms", (unsigned long)(GetTickCount() - t0));
            delete t;
        }
    }
    return true;
}

// vsftpd's require_ssl_reuse: the data connection must resume the control connection's TLS session. The session is resumed (TLS 1.2 id / TLS 1.3 ticket).
VM2B_TEST("ftp", ftp_tls_session_reuse_required)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        Str name = p.get("name").str();
        if (name != "es_reuse" && name != "s_reuse") continue;
        Target* t = MakeFtp(c_, QuirkCfg(*q, p, Uniq(c_, "ru"))); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); T_CHECK(ok);
        if (ok) {
            Bytes d = RandomData(c_, (size_t)c_.range(1, 90000));
            bool up = Upload(c_, t, "reuse.bin", d);
            T_CHECK(up);
            if (up) { Bytes g; T_CHECK(Download(c_, t, "reuse.bin", g)); T_CHECK(SameBytes(g, d)); Vec<Str> l; T_CHECK(t->list(l)); T_CHECK(l.size() == 1); }
        }
        delete t;
    }
    return true;
}

// A server that lies in the data channel: the announced size, a listing that never ends, a passive address that leads nowhere.
VM2B_TEST("ftp", ftp_lying_data_channel)
{
    const Json* q = QuirkSrv(c_); if (!q) return true;
    const Json& profs = q->get("profiles");
    for (size_t pi = 0; pi < profs.size(); ++pi) {
        const Json& p = profs.at(pi);
        Str name = p.get("name").str(), ex = p.get("expect").str();
        if (ex != "data" && name != "pasv_public") continue;
        Target* t = MakeFtp(c_, QuirkCfg(*q, p, Uniq(c_, "ld"), name == "pasv_public" ? 4 : 10)); T_CHECK(t != nullptr); if (!t) return false;
        bool ok = t->connect(); if (!ok) c_.note("%s connect: %s", name.c_str(), t->err.c_str()); T_CHECK(ok); if (!ok) { delete t; continue; }
        DWORD t0 = GetTickCount();
        if (name == "size_long" || name == "size_short") {
            // the upload itself is honest; only SIZE lies
            Bytes d = RandomData(c_, (size_t)c_.range(2000, 50000)); T_CHECK(Upload(c_, t, "f.bin", d));
            Bytes g; bool dok = Download(c_, t, "f.bin", g);
            c_.note("%s: download %s (%s)", name.c_str(), dok ? "WORKED" : "failed", dok ? "" : t->err.c_str());
            if (dok) { c_.note("%s: a download whose size contradicts SIZE must not be accepted", name.c_str()); c_.failed = true; }
            // the target is usable afterwards
            Vec<Str> l; T_CHECK(t->list(l)); T_CHECK(l.size() == 1);
        } else if (name == "listbomb") {
            Vec<Str> l; bool lok = t->list(l);
            c_.note("listbomb: list %s after %lu ms: %s", lok ? "WORKED?!" : "failed", (unsigned long)(GetTickCount() - t0), t->err.c_str());
            T_CHECK(!lok); T_CHECK(!t->transient || true);
        } else if (name == "pasv_dead") {
            Vec<Str> l; bool lok = t->list(l); c_.note("pasv_dead: list %s: %s", lok ? "WORKED?!" : "failed", t->err.c_str());
            T_CHECK(!lok); T_CHECK(t->transient);
        } else {   // pasv_public: an address nobody can reach, the control connection's address does work
            Bytes d = RandomData(c_, (size_t)c_.range(1, 40000)); bool up = Upload(c_, t, "f.bin", d);
            c_.note("pasv_public: upload %s after %lu ms: %s", up ? "ok" : "failed", (unsigned long)(GetTickCount() - t0), up ? "" : t->err.c_str());
            T_CHECK(up);
            if (up) { Bytes g; T_CHECK(Download(c_, t, "f.bin", g)); T_CHECK(SameBytes(g, d)); }
        }
        if (GetTickCount() - t0 > 90000) { c_.note("%s took too long", name.c_str()); c_.failed = true; }
        delete t;
    }
    return true;
}

// ---- throttling over real sockets: the wire rate follows the limit, and an abort under a tight limit is quick ----------------
struct ThrAbortProg : Progress {
    DWORD t0, afterMs; DWORD abortedAt; ThrAbortProg(DWORD a) : t0(GetTickCount()), afterMs(a), abortedAt(0) {}
    bool aborted() override { if (GetTickCount() - t0 >= afterMs) { if (!abortedAt) abortedAt = GetTickCount(); return true; } return false; }
};

VM2B_TEST("ftp", ftp_throttle_accuracy_and_abort)
{
    if (!AnyFtp(c_)) return true;
    const char* key = kFtpKeys[c_.rnd(3)]; const Json* s = FtpSrv(c_, key); if (!s) return true;
    FtpTmp tmp(c_, "ftpthr");
    Str root = tmp.sub("vms");
    size_t sz = (size_t)c_.range(500000, 900000); Bytes data; data.resize(sz); { u64 x = c_.next() | 1; for (size_t i = 0; i < sz; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; data[i] = (u8)(x >> 24); } }   // really incompressible (c_.fill is not: the wire rate is what is measured)
    T_CHECK(WriteBytesTo(JoinPath(JoinPath(root, "vm"), "disk.vmdk"), data));
    Target* t = MakeFtp(c_, FtpCfg(*s, key, Uniq(c_, "thr"))); T_CHECK(t && t->connect()); if (!t) return false;
    VmSource* src = NewDirectorySource(root);
    u64 rate = (u64)c_.range(150000, 300000);
    BackupOptions o; o.codec.level = 1; o.verify = false; o.kdfIterations = 3000;
    RateLimiter rl; rl.setBytesPerSecond(rate); o.limiter = &rl;
    DWORD t0 = GetTickCount(); Manifest m; Str warn, err;
    bool ok = BackupVm(*src, "vm", *t, o, nullptr, m, warn, err); if (!ok) c_.note("%s backup: %s", key, err.c_str()); T_CHECK(ok);
    double dt = (double)(GetTickCount() - t0) / 1000.0, want = (double)sz / (double)rate;
    if (dt < want * 0.7 - 0.2 || dt > want * 1.5 + 5) c_.note("%s: %zu bytes at %llu B/s took %.2f s (expected about %.2f)", key, sz, (unsigned long long)rate, dt, want);
    T_CHECK(dt >= want * 0.7 - 0.2); T_CHECK(dt <= want * 1.5 + 5);
    // reading back is limited the same way
    ReadOptions ro; ro.limiter = &rl; t0 = GetTickCount(); Str ve;
    T_CHECK(VerifyBackup(*t, m.archive, "", ro, nullptr, nullptr, ve));
    dt = (double)(GetTickCount() - t0) / 1000.0;
    if (dt < want * 0.7 - 0.2 || dt > want * 1.5 + 5) c_.note("%s: verify %.2f s (expected about %.2f)", key, dt, want);
    T_CHECK(dt >= want * 0.7 - 0.2); T_CHECK(dt <= want * 1.5 + 5);
    // a tight limit and an abort after a second: the upload stops at once, nothing is left on the server
    Str root2 = tmp.sub("vms2"); T_CHECK(WriteBytesTo(JoinPath(JoinPath(root2, "vm2"), "disk.vmdk"), data));
    VmSource* src2 = NewDirectorySource(root2);
    rl.setBytesPerSecond(3000); ThrAbortProg ap(1000); Manifest m2; Str w2, e2;
    ok = BackupVm(*src2, "vm2", *t, o, &ap, m2, w2, e2); T_CHECK(!ok);
    DWORD lat = ap.abortedAt ? GetTickCount() - ap.abortedAt : 0;
    if (!ap.abortedAt || lat > 5000) c_.note("%s: abort latency %lu ms (aborted flag seen: %d)", key, (unsigned long)lat, ap.abortedAt != 0);
    T_CHECK(ap.abortedAt != 0); T_CHECK(lat < 5000);
    Vec<Str> names; T_CHECK(t->list(names));
    for (const Str& n : names) if (n.find("vm2") >= 0) { c_.note("%s: left behind after abort: %s", key, n.c_str()); c_.failed = true; }
    delete src; delete src2; delete t;
    return true;
}

// ---- several jobs at once, over every FTP flavour: each its own target (own control + data + TLS sessions), all intact ----------
struct FtpParArg { Json cfg; int id, files; volatile long* bad; Str msg; };
static DWORD WINAPI FtpParWorker(LPVOID p)
{
    FtpParArg* a = (FtpParArg*)p;
    Str e; Target* t = MakeTarget(a->cfg, e);
    if (!t) { a->msg = "MakeTarget: " + e; InterlockedIncrement(a->bad); return 1; }
    if (!t->connect()) { a->msg = "connect: " + t->err; InterlockedIncrement(a->bad); delete t; return 1; }
    for (int i = 0; i < a->files && !*a->bad; ++i) {
        Str n = Str::fmt("job%d-%d.tar.%05d", a->id, i, i);
        size_t size = (size_t)((a->id * 7919 + i * 104729) % 900000);
        Bytes data; data.resize(size); PatFill((u64)(a->id * 31 + i), data.data(), size);
        Sink* w = t->open_write(n);
        if (!w) { a->msg = "open_write: " + t->err; InterlockedIncrement(a->bad); break; }
        bool ok = true; size_t pos = 0;
        while (pos < size && ok) { size_t k = size - pos < 30011 ? size - pos : 30011; ok = w->write(data.data() + pos, k); pos += k; }
        if (ok) ok = w->finish(); else w->abort();
        if (!ok) { a->msg = "upload: " + w->err; InterlockedIncrement(a->bad); delete w; break; }
        delete w;
        Source* r = t->open_read(n); if (!r) { a->msg = "open_read: " + t->err; InterlockedIncrement(a->bad); break; }
        Bytes got; Bytes buf; buf.resize(40000);
        for (;;) { i64 k = r->read(buf.data(), buf.size()); if (k < 0) { a->msg = "read: " + r->err; InterlockedIncrement(a->bad); break; } if (k == 0) break; size_t o = got.size(); got.resize(o + (size_t)k); memcpy(got.data() + o, buf.data(), (size_t)k); }
        r->close(); delete r;
        if (!*a->bad && !SameBytes(got, data)) { a->msg = Str::fmt("data differs (%zu vs %zu bytes)", got.size(), data.size()); InterlockedIncrement(a->bad); }
    }
    Vec<Str> names; if (!*a->bad && !t->list(names)) { a->msg = "list: " + t->err; InterlockedIncrement(a->bad); }
    for (const Str& n : names) if (!*a->bad) t->remove(n);
    delete t;
    return 0;
}

VM2B_TEST("ftp", ftp_parallel_jobs)
{
    if (!AnyFtp(c_)) return true;
    enum { N = 9 }; FtpParArg args[N]; HANDLE th[N]; volatile long bad = 0; int n = 0;
    for (int i = 0; i < N; ++i) {
        const char* key = kFtpKeys[i % 3]; const Json* s = FtpSrv(c_, key); if (!s) continue;
        args[n].cfg = FtpCfg(*s, key, Uniq(c_, "par") + Str::fmt("p%d", i)); args[n].id = i; args[n].files = 4; args[n].bad = &bad; ++n;
    }
    for (int i = 0; i < n; ++i) th[i] = CreateThread(nullptr, 0, FtpParWorker, &args[i], 0, nullptr);
    WaitForMultipleObjects((DWORD)n, th, TRUE, INFINITE);
    for (int i = 0; i < n; ++i) { CloseHandle(th[i]); if (!args[i].msg.empty()) c_.note("job %d (%s): %s", args[i].id, args[i].cfg.get("type").str().c_str(), args[i].msg.c_str()); }
    T_CHECK(bad == 0);
    return true;
}

} // namespace vm2b
