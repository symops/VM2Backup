// TCP + TLS (OpenSSL over our own socket loop) connection, see tls.h.
#include "tls.h"
#include "winutil.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

namespace vm2b {

// ---- options ---------------------------------------------------------------------------------------------------------
TlsOptions TlsOptions::FromJson(const Json& c)
{
    TlsOptions o;
    o.verify = c.get("verify").boolean(true);
    o.fingerprint = c.get("fingerprint").str();
    Str m = c.get("tls_min").str("1.2"); if (!m.empty()) o.tlsMin = m;
    o.caFile = c.get("ca_file").str();
    double s = c.get("timeout").num(30);
    if (s > 0) o.timeoutMs = (int)(s * 1000);
    if (c.has("timeout_ms")) o.timeoutMs = (int)c.get("timeout_ms").integer(o.timeoutMs);
    if (o.timeoutMs < 100) o.timeoutMs = 100;
    return o;
}

Str TlsNormalizeFingerprint(const Str& s)
{
    Str t = s.lower(), algo, hex;
    int c = t.find(':');
    size_t start = 0;
    if (c >= 0 && (t.starts_with("sha256:") || t.starts_with("sha-256:") || t.starts_with("sha1:") || t.starts_with("sha-1:"))) {
        algo = t.substr(0, (size_t)c); start = (size_t)c + 1;
        algo = algo.replace_all("-", "");
    }
    for (size_t i = start; i < t.size(); ++i) {
        char ch = t[i];
        if (ch == ':' || ch == ' ' || ch == '\t') continue;
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return Str();
        hex += ch;
    }
    if (algo.empty()) algo = hex.size() == 64 ? "sha256" : hex.size() == 40 ? "sha1" : "";
    if (algo == "sha256" && hex.size() == 64) return algo + ":" + hex;
    if (algo == "sha1" && hex.size() == 40) return algo + ":" + hex;
    return Str();
}

// ---- Winsock ---------------------------------------------------------------------------------------------------------
namespace {

struct WsGuard { WsGuard() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };
void WsInit() { static WsGuard g; (void)g; }

Str WsErr(const char* what, int code)
{
    return Str::fmt("%s: %s", what, WinErrorText((unsigned long)code).c_str());
}

Str AddrText(const sockaddr* sa, int* port)
{
    char buf[64] = "";
    if (sa->sa_family == AF_INET) { const sockaddr_in* a = (const sockaddr_in*)sa; inet_ntop(AF_INET, (void*)&a->sin_addr, buf, sizeof(buf)); if (port) *port = ntohs(a->sin_port); }
    else if (sa->sa_family == AF_INET6) { const sockaddr_in6* a = (const sockaddr_in6*)sa; inet_ntop(AF_INET6, (void*)&a->sin6_addr, buf, sizeof(buf)); if (port) *port = ntohs(a->sin6_port); }
    return Str(buf);
}

} // namespace

// ---- credentials: one SSL_CTX per tls_min, shared between connections so that sessions can be resumed ------------------
namespace {

struct TlsCred {
    long refs;
    SSL_CTX* ctx;
    Str tlsMin;
    CRITICAL_SECTION lock;
    // Sessions of this server (control connection -> data connections). TLS 1.3 tickets are single use for many servers (a ticket
    // that was used once is refused), so each one is handed out once from `pool`; `last` (any version) is the fallback.
    SSL_SESSION* pool[8]; int npool;
    SSL_SESSION* last;
    TlsCred() : refs(1), ctx(nullptr), npool(0), last(nullptr) { InitializeCriticalSection(&lock); memset(pool, 0, sizeof(pool)); }
    ~TlsCred() { for (int i = 0; i < npool; ++i) SSL_SESSION_free(pool[i]); if (last) SSL_SESSION_free(last); if (ctx) SSL_CTX_free(ctx); DeleteCriticalSection(&lock); }
    void addSession(SSL_SESSION* s)             // we own the reference
    {
        EnterCriticalSection(&lock);
        SSL_SESSION_up_ref(s); if (last) SSL_SESSION_free(last); last = s;
        if (SSL_SESSION_get_protocol_version(s) >= TLS1_3_VERSION) {
            if (npool == 8) { SSL_SESSION_free(pool[0]); memmove(pool, pool + 1, 7 * sizeof(pool[0])); --npool; }
            pool[npool++] = s;
        } else SSL_SESSION_free(s);
        LeaveCriticalSection(&lock);
    }
    SSL_SESSION* takeSession()                  // a reference for the caller, or null
    {
        EnterCriticalSection(&lock);
        SSL_SESSION* s = nullptr;
        if (npool > 0) s = pool[--npool];
        else if (last) { s = last; SSL_SESSION_up_ref(s); }
        LeaveCriticalSection(&lock);
        return s;
    }
};

int NewSessionCb(SSL* ssl, SSL_SESSION* s)
{
    TlsCred* c = (TlsCred*)SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl));
    if (!c) return 0;
    c->addSession(s);            // we keep the reference we were handed
    return 1;
}

TlsCred* NewCred(const Str& tlsMin, Str& err)
{
    TlsCred* c = new TlsCred();
    c->tlsMin = tlsMin;
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) { err = "cannot set up TLS (SSL_CTX_new)"; delete c; return nullptr; }
    int minv = tlsMin == "1.0" ? TLS1_VERSION : tlsMin == "1.1" ? TLS1_1_VERSION : TLS1_2_VERSION;
    SSL_CTX_set_min_proto_version(c->ctx, minv);
    SSL_CTX_set_max_proto_version(c->ctx, TLS1_3_VERSION);
    // Old servers (ESXi 6.0: TLS 1.0, SHA-1 certificates, small keys) need security level 0; only when the user lowered the floor.
    if (minv < TLS1_2_VERSION) { SSL_CTX_set_security_level(c->ctx, 0); SSL_CTX_set_cipher_list(c->ctx, "DEFAULT:@SECLEVEL=0"); }
    SSL_CTX_set_mode(c->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, nullptr);          // the certificate is judged by validateCert() after the handshake
    SSL_CTX_set_session_cache_mode(c->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_set_app_data(c->ctx, c);
    SSL_CTX_sess_set_new_cb(c->ctx, NewSessionCb);
    return c;
}

void CredRelease(TlsCred* c) { if (c && InterlockedDecrement(&c->refs) == 0) delete c; }

// The OpenSSL error queue as one line ("what: reason").
Str SslErr(const char* what, unsigned long* reason = nullptr)
{
    unsigned long e = ERR_get_error(), last = e;
    while (last) { unsigned long n = ERR_get_error(); if (!n) break; last = n; e = n; }     // the newest entry is the most specific
    ERR_clear_error();
    if (reason) *reason = e;
    const char* t = e ? ERR_reason_error_string(e) : nullptr;
    if (e && !t) return Str::fmt("%s: TLS error 0x%lx", what, e);
    return Str::fmt("%s%s%s", what, t ? ": " : "", t ? t : "");
}

// Windows ROOT + CA stores as OpenSSL trust anchors (public CAs work without a bundle); loaded once.
X509_STORE* g_sysStore; INIT_ONCE g_sysOnce = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK LoadSysStore(PINIT_ONCE, PVOID, PVOID*)
{
    g_sysStore = X509_STORE_new();
    static const wchar_t* names[] = { L"ROOT", L"CA" };
    for (const wchar_t* nm : names) {
        HCERTSTORE h = CertOpenSystemStoreW(0, nm);
        if (!h) continue;
        PCCERT_CONTEXT cc = nullptr;
        while ((cc = CertEnumCertificatesInStore(h, cc)) != nullptr) {
            const unsigned char* p = cc->pbCertEncoded;
            X509* x = d2i_X509(nullptr, &p, (long)cc->cbCertEncoded);
            if (x) { X509_STORE_add_cert(g_sysStore, x); X509_free(x); }
        }
        CertCloseStore(h, 0);
    }
    ERR_clear_error();
    return TRUE;
}
X509_STORE* SystemStore() { InitOnceExecuteOnce(&g_sysOnce, LoadSysStore, nullptr, nullptr); return g_sysStore; }

} // namespace

TlsCredRef::TlsCredRef(const TlsCredRef& o) : m_c(o.m_c) { if (m_c) InterlockedIncrement(&((TlsCred*)m_c)->refs); }
TlsCredRef& TlsCredRef::operator=(const TlsCredRef& o)
{
    if (this != &o) { if (o.m_c) InterlockedIncrement(&((TlsCred*)o.m_c)->refs); CredRelease((TlsCred*)m_c); m_c = o.m_c; }
    return *this;
}
TlsCredRef::~TlsCredRef() { CredRelease((TlsCred*)m_c); }
void TlsCredRef::reset() { CredRelease((TlsCred*)m_c); m_c = nullptr; }

// ---- NetConn: plain TCP ----------------------------------------------------------------------------------------------
NetConn::NetConn()
    : timedOut(false), aborted(false), certError(false), tlsError(false), cleanEof(false), timeoutMs(30000), abortFlag(nullptr),
      m_sock((size_t)INVALID_SOCKET), m_tls(false), m_closeNotify(false), m_v6(false), m_broken(false), m_cred(nullptr), m_ssl(nullptr),
      m_peerPort(0), m_sent(0), m_recvd(0), m_resumed(false)
{
    WsInit();
}

NetConn::~NetConn() { close(); }

void NetConn::freeTls()
{
    if (m_ssl) { SSL_free((SSL*)m_ssl); m_ssl = nullptr; }
    if (m_cred) { CredRelease((TlsCred*)m_cred); m_cred = nullptr; }
    m_tls = false; m_closeNotify = false;
}

void NetConn::close()
{
    // The peer already said close_notify (a finished download): answer it, so that its own shutdown completes and a server that
    // drops the session of an unclean close (OpenSSL: fatal alert -> session removed) keeps it for the next data connection.
    if (m_ssl && m_tls && m_closeNotify && !m_broken && m_sock != (size_t)INVALID_SOCKET) { SSL_shutdown((SSL*)m_ssl); ERR_clear_error(); }
    freeTls();
    if (m_sock != (size_t)INVALID_SOCKET) {
        // an immediate close: the data that was not delivered is dropped (abort of a transfer must not linger)
        closesocket((SOCKET)m_sock);
        m_sock = (size_t)INVALID_SOCKET;
    }
}

int NetConn::waitSock(bool forWrite, int ms)
{
    if (ms < 0) ms = timeoutMs;
    DWORD start = GetTickCount();
    for (;;) {
        if (abortFlag && *abortFlag) { aborted = true; err = "aborted"; return -1; }
        DWORD el = GetTickCount() - start;
        int left = ms - (int)el;
        if (left < 0) left = 0;
        int slice = left < 100 ? left : 100;
        fd_set fs, es; FD_ZERO(&fs); FD_ZERO(&es);
        FD_SET((SOCKET)m_sock, &fs); FD_SET((SOCKET)m_sock, &es);
        timeval tv; tv.tv_sec = slice / 1000; tv.tv_usec = (slice % 1000) * 1000;
        int r = select(0, forWrite ? nullptr : &fs, forWrite ? &fs : nullptr, &es, &tv);
        if (r == SOCKET_ERROR) { err = WsErr("select failed", WSAGetLastError()); return -1; }
        if (r > 0) return 1;
        if (left <= slice) { timedOut = true; err = "timed out"; return 0; }
    }
}

bool NetConn::rawSendAll(const void* p, size_t n)
{
    const char* d = (const char*)p;
    while (n) {
        int k = ::send((SOCKET)m_sock, d, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (k == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) { int w = waitSock(true, -1); if (w <= 0) { if (w == 0) err = "send timed out"; return false; } continue; }
            err = WsErr("send failed", e); return false;
        }
        d += k; n -= (size_t)k; m_sent += (u64)k;
    }
    return true;
}

i64 NetConn::rawRecv(void* p, size_t n)
{
    for (;;) {
        int k = ::recv((SOCKET)m_sock, (char*)p, n > (1u << 20) ? (1 << 20) : (int)n, 0);
        if (k >= 0) { m_recvd += (u64)k; return k; }
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) { int w = waitSock(false, -1); if (w <= 0) { if (w == 0) err = "receive timed out"; return -1; } continue; }
        err = WsErr("receive failed", e);
        return -1;
    }
}

void NetConn::sockInfo()
{
    sockaddr_storage ss; int l = sizeof(ss);
    if (getpeername((SOCKET)m_sock, (sockaddr*)&ss, &l) == 0) { m_peerIp = AddrText((sockaddr*)&ss, &m_peerPort); m_v6 = ss.ss_family == AF_INET6; }
    l = sizeof(ss);
    if (getsockname((SOCKET)m_sock, (sockaddr*)&ss, &l) == 0) m_localIp = AddrText((sockaddr*)&ss, nullptr);
}

static void TuneSocket(SOCKET s)
{
    BOOL one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));     // records must not wait for Nagle + delayed ACK
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
}

bool NetConn::connect(const Str& host, int port, bool implicitTls, const TlsOptions& o, TlsCredRef* cred)
{
    close();
    err.clear(); timedOut = aborted = certError = tlsError = false; cleanEof = false;
    timeoutMs = o.timeoutMs; abortFlag = o.abort;
    m_fp256.clear(); m_fp1.clear(); m_ver.clear(); m_sid.clear(); m_sent = m_recvd = 0; m_resumed = false;
    if (host.empty()) return fail("no host name");
    addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
    int gr = getaddrinfo(host.c_str(), ps, &hints, &res);
    if (gr != 0 || !res) return fail(Str::fmt("cannot resolve %s: %s", host.c_str(), WinErrorText((unsigned long)WSAGetLastError()).c_str()));
    Str lastErr;
    bool ok = false;
    // IPv4 first (what the test servers and most NAS boxes listen on), then the others.
    for (int pass = 0; pass < 2 && !ok; ++pass) {
        for (addrinfo* a = res; a && !ok; a = a->ai_next) {
            if ((a->ai_family == AF_INET) != (pass == 0)) continue;
            SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) { lastErr = WsErr("socket failed", WSAGetLastError()); continue; }
            TuneSocket(s);
            m_sock = (size_t)s;
            int r = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
            bool good = r == 0;
            if (!good) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) {
                    int w = waitSock(true, -1);
                    if (w == 1) {
                        int soerr = 0; int sl = sizeof(soerr);
                        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
                        if (soerr == 0) good = true; else lastErr = WsErr(Str::fmt("cannot connect to %s:%d", host.c_str(), port).c_str(), soerr);
                    } else if (w == 0) lastErr = Str::fmt("cannot connect to %s:%d: timed out", host.c_str(), port);
                    else { lastErr = err; if (aborted) { freeaddrinfo(res); close(); return false; } }
                } else lastErr = WsErr(Str::fmt("cannot connect to %s:%d", host.c_str(), port).c_str(), e);
            }
            if (good) ok = true;
            else { closesocket(s); m_sock = (size_t)INVALID_SOCKET; }
        }
    }
    freeaddrinfo(res);
    if (!ok) { err = lastErr.empty() ? Str("cannot connect") : lastErr; if (!timedOut && err.find("timed out") >= 0) timedOut = true; return false; }
    sockInfo();
    if (implicitTls && !starttls(host, o, cred)) return false;
    return true;
}

bool NetConn::accept(TcpListener& l, const TlsOptions& o)
{
    close();
    err.clear(); timedOut = aborted = certError = tlsError = false; cleanEof = false;
    timeoutMs = o.timeoutMs; abortFlag = o.abort;
    m_sent = m_recvd = 0;
    m_sock = l.m_sock;
    int w = waitSock(false, -1);
    m_sock = (size_t)INVALID_SOCKET;
    if (w != 1) { if (w == 0) err = "the server did not connect back (active mode) in time"; return false; }
    SOCKET s = ::accept((SOCKET)l.m_sock, nullptr, nullptr);
    if (s == INVALID_SOCKET) return fail(WsErr("accept failed", WSAGetLastError()));
    TuneSocket(s);
    m_sock = (size_t)s;
    sockInfo();
    return true;
}

bool NetConn::send(const void* p, size_t n)
{
    if (!isOpen()) return fail("connection is closed");
    if (!n) return true;
    timedOut = aborted = false;
    return m_tls ? tlsSend(p, n) : rawSendAll(p, n);
}

i64 NetConn::recv(void* p, size_t n)
{
    if (!isOpen()) { err = "connection is closed"; return -1; }
    if (!n) return 0;
    timedOut = aborted = false;
    if (m_tls) return tlsRecv(p, n);
    i64 k = rawRecv(p, n);
    if (k == 0) cleanEof = true;
    return k;
}

bool NetConn::waitReadable(int ms)
{
    if (!isOpen()) return true;
    if (m_ssl && SSL_pending((SSL*)m_ssl) > 0) return true;     // decrypted bytes already buffered
    fd_set fs; FD_ZERO(&fs); FD_SET((SOCKET)m_sock, &fs);
    timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    int r = select(0, &fs, nullptr, nullptr, &tv);
    return r != 0;
}

void NetConn::shutdown()
{
    if (!isOpen()) return;
    if (m_tls && m_ssl && !m_broken) {
        if (m_ver == "TLS 1.3") { u8 junk[512]; size_t got = 0; SSL_read_ex((SSL*)m_ssl, junk, sizeof(junk), &got); }   // takes in session tickets that came after the handshake (an upload never reads)
        // Sends close_notify; the peer's is not awaited.  The socket is non-blocking: when the peer reads slowly and the kernel send buffer is full, SSL_shutdown
        // answers WANT_WRITE and the alert stays inside OpenSSL, while the ::shutdown(SD_SEND) below puts the FIN after the already queued data - the server then saw
        // the end of the upload without a close_notify ("Unexpected EOF"; pyftpdlib spun forever on it and never answered 226, so the client waited out 60 s: round 9,
        // ftp_fault_injection_upload on a Windows VM, about 1 run in 20).  So the alert is pushed out for real: wait for writability and call again.
        DWORD tshut = GetTickCount(); int sbudget = timeoutMs > 0 && timeoutMs < 10000 ? timeoutMs : 10000;
        for (;;) {
            ERR_clear_error(); int r = SSL_shutdown((SSL*)m_ssl); if (r >= 0) break;
            int e = SSL_get_error((SSL*)m_ssl, r);
            if (e != SSL_ERROR_WANT_WRITE) break;                                          // WANT_READ: our close_notify is out, the peer's is not awaited
            if (abortFlag && *abortFlag) break;
            int left = sbudget - (int)(GetTickCount() - tshut); if (left <= 0) break;
            fd_set ws; FD_ZERO(&ws); FD_SET((SOCKET)m_sock, &ws); timeval tv; tv.tv_sec = left / 1000; tv.tv_usec = (left % 1000) * 1000;
            if (select(0, nullptr, &ws, nullptr, &tv) <= 0) break;
        }
        ERR_clear_error();
    }
    if (m_sock != (size_t)INVALID_SOCKET) {
        ::shutdown((SOCKET)m_sock, SD_SEND);
        // Wait for the peer's own end (its close_notify and FIN) before closing. closesocket() with bytes still unread in the receive
        // buffer (TLS 1.3 session tickets, the peer's close_notify) sends a RST instead of a FIN, and some servers (pyftpdlib) take the
        // RST for the end of an upload that is complete, drop what they have not read yet and answer 226: a silently truncated file.
        DWORD began = GetTickCount(); int budget = timeoutMs > 0 && timeoutMs < 10000 ? timeoutMs : 10000;
        for (;;) {
            if (abortFlag && *abortFlag) break;
            int left = budget - (int)(GetTickCount() - began); if (left <= 0) break;
            fd_set fs; FD_ZERO(&fs); FD_SET((SOCKET)m_sock, &fs);
            timeval tv; int slice = left < 100 ? left : 100; tv.tv_sec = 0; tv.tv_usec = slice * 1000;
            int r = select(0, &fs, nullptr, nullptr, &tv);
            if (r == SOCKET_ERROR) break;
            if (r > 0) { char junk[4096]; int k = ::recv((SOCKET)m_sock, junk, sizeof(junk), 0); if (k <= 0) break; }
        }
    }
    close();
}

// ---- TLS -------------------------------------------------------------------------------------------------------------
bool NetConn::starttls(const Str& serverName, const TlsOptions& o, TlsCredRef* cred)
{
    if (!isOpen()) return fail("connection is closed");
    if (m_tls) return true;
    timeoutMs = o.timeoutMs; abortFlag = o.abort;
    bool ok = tlsHandshake(serverName, o, cred);
    if (!ok) { Str e = err; bool t = timedOut, a = aborted, c = certError, te = tlsError; close(); err = e; timedOut = t; aborted = a; certError = c; tlsError = te; }
    return ok;
}

static const char* VersionName(int v)
{
    switch (v) { case TLS1_3_VERSION: return "TLS 1.3"; case TLS1_2_VERSION: return "TLS 1.2"; case TLS1_1_VERSION: return "TLS 1.1"; case TLS1_VERSION: return "TLS 1.0"; default: return "unknown TLS version"; }
}

static bool IsIpLiteral(const Str& s)
{
    in_addr a4; in6_addr a6;
    return inet_pton(AF_INET, s.c_str(), &a4) == 1 || inet_pton(AF_INET6, s.c_str(), &a6) == 1;
}

// Runs one SSL call to completion on the non-blocking socket: waits for the direction OpenSSL asks for (timeout / abort as in
// every other wait). Returns 1 when the call succeeded, 0 on failure (err / timedOut / aborted set; `sslErr` = SSL_get_error).
int NetConn::sslWait(int sslErr)
{
    if (sslErr == SSL_ERROR_WANT_READ || sslErr == SSL_ERROR_WANT_WRITE) {
        int w = waitSock(sslErr == SSL_ERROR_WANT_WRITE, -1);
        if (w == 1) return 1;
        if (w == 0) err = sslErr == SSL_ERROR_WANT_WRITE ? "send timed out" : "receive timed out";
        return 0;
    }
    return -1;
}

bool NetConn::tlsHandshake(const Str& serverName, const TlsOptions& o, TlsCredRef* cred)
{
    m_server = serverName;
    TlsCred* c = nullptr;
    if (cred && cred->m_c && ((TlsCred*)cred->m_c)->tlsMin == o.tlsMin) { c = (TlsCred*)cred->m_c; InterlockedIncrement(&c->refs); }
    else {
        Str e; c = NewCred(o.tlsMin, e);
        if (!c) { tlsError = true; return fail(e); }
        if (cred) { cred->reset(); cred->m_c = c; InterlockedIncrement(&c->refs); }
    }
    m_cred = c;
    SSL* ssl = SSL_new(c->ctx);
    if (!ssl) { tlsError = true; return fail(SslErr("cannot set up TLS (SSL_new)")); }
    m_ssl = ssl; m_broken = false;
    SSL_set_fd(ssl, (int)m_sock);
    if (!IsIpLiteral(serverName)) SSL_set_tlsext_host_name(ssl, serverName.c_str());
    if (SSL_SESSION* s = c->takeSession()) { SSL_set_session(ssl, s); SSL_SESSION_free(s); }    // resume: data connection <- control connection
    for (;;) {
        int r = SSL_connect(ssl);
        if (r == 1) break;
        int wsa = WSAGetLastError();
        int e = SSL_get_error(ssl, r);
        int w = sslWait(e);
        if (w == 1) continue;
        m_broken = true;
        if (w == 0) return false;
        unsigned long reason = 0; Str t = SslErr("TLS handshake failed", &reason);
        if (ERR_SYSTEM_ERROR(reason)) return fail(WsErr("receive failed", (int)ERR_GET_REASON(reason)));
        if (e == SSL_ERROR_SYSCALL && wsa && wsa != WSAEWOULDBLOCK) return fail(WsErr("receive failed", wsa));      // reset: not a protocol failure
        tlsError = true;
        if (ERR_GET_REASON(reason) == SSL_R_UNEXPECTED_EOF_WHILE_READING || e == SSL_ERROR_SYSCALL) return fail("the server closed the connection during the TLS handshake");
        if (ERR_GET_REASON(reason) == SSL_R_UNSUPPORTED_PROTOCOL || ERR_GET_REASON(reason) == SSL_R_WRONG_VERSION_NUMBER || ERR_GET_REASON(reason) == SSL_R_NO_PROTOCOLS_AVAILABLE ||
            ERR_GET_REASON(reason) == SSL_R_TLSV1_ALERT_PROTOCOL_VERSION || ERR_GET_REASON(reason) == SSL_R_NO_SUITABLE_DIGEST_ALGORITHM)
            return fail(Str::fmt("%s (no TLS protocol at or above the configured minimum TLS %s (tls_min) is offered by the server)", t.c_str(), o.tlsMin.c_str()));
        return fail(t);
    }
    m_tls = true;
    m_ver = VersionName(SSL_version(ssl));
    m_resumed = SSL_session_reused(ssl) != 0;
    if (SSL_version(ssl) < TLS1_3_VERSION) { unsigned int l = 0; const unsigned char* id = SSL_SESSION_get_id(SSL_get_session(ssl), &l); if (id && l) m_sid = HexEncode(id, l); }
    syncCounts();
    if (!validateCert(serverName, o)) { m_tls = false; return false; }
    return true;
}

void NetConn::syncCounts()
{
    if (!m_ssl) return;
    BIO* r = SSL_get_rbio((SSL*)m_ssl); BIO* w = SSL_get_wbio((SSL*)m_ssl);
    if (r) m_recvd = (u64)BIO_number_read(r);
    if (w) m_sent = (u64)BIO_number_written(w);
}

static Str FpHex(const char* algo, const u8* d, size_t n) { return Str(algo) + ":" + HexEncode(d, n); }

static const char* VerifyErrText(int e)
{
    switch (e) {
    case X509_V_ERR_CERT_HAS_EXPIRED: case X509_V_ERR_CERT_NOT_YET_VALID: return "the certificate is expired or not yet valid";
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return "the certificate is issued by an untrusted authority (self-signed or unknown CA)";
    case X509_V_ERR_HOSTNAME_MISMATCH: case X509_V_ERR_IP_ADDRESS_MISMATCH: case X509_V_ERR_EMAIL_MISMATCH: return "the certificate does not match the host name";
    case X509_V_ERR_CERT_REVOKED: return "the certificate was revoked";
    default: return nullptr;
    }
}

bool NetConn::validateCert(const Str& serverName, const TlsOptions& o)
{
    SSL* ssl = (SSL*)m_ssl;
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) { certError = true; return fail("the server sent no certificate"); }
    u8 h256[32], h1[20]; unsigned l = 0;
    X509_digest(cert, EVP_sha256(), h256, &l); X509_digest(cert, EVP_sha1(), h1, &l);
    m_fp256 = FpHex("sha256", h256, 32); m_fp1 = FpHex("sha1", h1, 20);
    bool ok = false; Str why;
    if (!o.fingerprint.empty()) {
        Str want = TlsNormalizeFingerprint(o.fingerprint);
        if (want.empty()) why = "the pinned fingerprint is malformed (expected sha256:<64 hex digits>)";
        else if (want == m_fp256 || want == m_fp1) ok = true;
        else why = "the server certificate does not match the pinned fingerprint";
    } else if (!o.verify) {
        ok = true;
    } else {
        X509_STORE* st = SystemStore(); bool own = false;
        if (!o.caFile.empty()) {
            Bytes pem; Str e;
            if (!ReadWholeFile(o.caFile.c_str(), pem, e, 16 << 20)) why = "cannot read the CA bundle: " + e;
            else {
                st = X509_STORE_new(); own = true; int count = 0;
                BIO* b = BIO_new_mem_buf(pem.data(), (int)pem.size());
                while (X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr)) { if (X509_STORE_add_cert(st, x)) ++count; X509_free(x); }
                BIO_free(b); ERR_clear_error();
                if (!count) why = "the CA bundle contains no certificates";
                else {                                      // ... in addition to the system store
                    STACK_OF(X509_OBJECT)* objs = X509_STORE_get0_objects(SystemStore());
                    for (int i = 0; i < sk_X509_OBJECT_num(objs); ++i) if (X509* x = X509_OBJECT_get0_X509(sk_X509_OBJECT_value(objs, i))) X509_STORE_add_cert(st, x);
                    ERR_clear_error();
                }
            }
        }
        if (why.empty()) {
            X509_STORE_CTX* sc = X509_STORE_CTX_new();
            int verr = 0;
            if (!sc || !X509_STORE_CTX_init(sc, st, cert, SSL_get_peer_cert_chain(ssl))) why = "cannot verify the certificate (internal error)";
            else {
                X509_STORE_CTX_set_default(sc, "ssl_server");
                X509_VERIFY_PARAM* vp = X509_STORE_CTX_get0_param(sc);
                X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                if (IsIpLiteral(serverName)) X509_VERIFY_PARAM_set1_ip_asc(vp, serverName.c_str()); else X509_VERIFY_PARAM_set1_host(vp, serverName.c_str(), 0);
                if (own) X509_VERIFY_PARAM_set_flags(vp, X509_V_FLAG_PARTIAL_CHAIN);      // a bundle certificate is an anchor even when it is not a root
                // no revocation checking (ESXi / NAS boxes have no CRL)
                if (X509_verify_cert(sc) == 1) ok = true;
                else {
                    verr = X509_STORE_CTX_get_error(sc);
                    const char* t = VerifyErrText(verr);
                    why = Str::fmt("%s (%s)", t ? t : "the certificate is not valid", X509_verify_cert_error_string(verr));
                }
            }
            if (sc) X509_STORE_CTX_free(sc);
        }
        if (own) X509_STORE_free(st);
        ERR_clear_error();
    }
    X509_free(cert);
    if (!ok) {
        certError = true;
        return fail(Str::fmt("certificate of %s rejected: %s; server certificate %s", serverName.c_str(), why.c_str(), m_fp256.c_str()));
    }
    return true;
}

bool NetConn::tlsSend(const void* p, size_t n)
{
    SSL* ssl = (SSL*)m_ssl;
    const u8* d = (const u8*)p;
    while (n) {
        size_t chunk = n > (1u << 20) ? (1u << 20) : n, wr = 0;
        if (SSL_write_ex(ssl, d, chunk, &wr) == 1) { d += wr; n -= wr; continue; }
        int wsa = WSAGetLastError();
        int e = SSL_get_error(ssl, 0);
        int w = sslWait(e);
        if (w == 1) continue;
        syncCounts();
        if (w == 0) return false;
        m_broken = true;
        if (e == SSL_ERROR_SYSCALL || ERR_SYSTEM_ERROR(ERR_peek_last_error())) { int code = ERR_SYSTEM_ERROR(ERR_peek_last_error()) ? (int)ERR_GET_REASON(ERR_peek_last_error()) : wsa; ERR_clear_error(); return fail(WsErr("send failed", code ? code : WSAECONNRESET)); }     // the connection, not the TLS protocol
        tlsError = true;
        return fail(SslErr("TLS send failed"));
    }
    syncCounts();
    return true;
}

i64 NetConn::tlsRecv(void* p, size_t n)
{
    SSL* ssl = (SSL*)m_ssl;
    if (m_closeNotify) { cleanEof = true; return 0; }
    for (;;) {
        size_t got = 0;
        if (SSL_read_ex(ssl, p, n > (1u << 20) ? (1u << 20) : n, &got) == 1) { syncCounts(); return (i64)got; }
        int wsa = WSAGetLastError();
        int e = SSL_get_error(ssl, 0);
        if (e == SSL_ERROR_ZERO_RETURN) { m_closeNotify = true; cleanEof = true; syncCounts(); return 0; }
        int w = sslWait(e);
        if (w == 1) continue;
        syncCounts();
        if (w == 0) return -1;
        m_broken = true;
        unsigned long reason = 0; Str t = SslErr("TLS receive failed", &reason);
        if (ERR_SYSTEM_ERROR(reason)) { err = WsErr("receive failed", (int)ERR_GET_REASON(reason)); return -1; }       // the socket failed (reset ...), not the protocol
        if (e == SSL_ERROR_SYSCALL && wsa && wsa != WSAEWOULDBLOCK) { err = WsErr("receive failed", wsa); return -1; }                       // reset etc.: the connection, not the protocol
        if (ERR_GET_REASON(reason) == SSL_R_UNEXPECTED_EOF_WHILE_READING || (e == SSL_ERROR_SYSCALL && !reason)) { cleanEof = false; return 0; }   // FIN without close_notify: the caller decides
        tlsError = true; err = t; return -1;
    }
}

// ---- TcpListener -----------------------------------------------------------------------------------------------------
TcpListener::TcpListener() : m_sock((size_t)INVALID_SOCKET), m_port(0) { WsInit(); }
TcpListener::~TcpListener() { close(); }
void TcpListener::close() { if (m_sock != (size_t)INVALID_SOCKET) { closesocket((SOCKET)m_sock); m_sock = (size_t)INVALID_SOCKET; } }

bool TcpListener::listen(const Str& localIp, Str& err)
{
    close();
    addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(localIp.c_str(), "0", &hints, &res) != 0 || !res) { err = "bad local address " + localIp; return false; }
    SOCKET s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    bool ok = false;
    if (s != INVALID_SOCKET) {
        if (bind(s, res->ai_addr, (int)res->ai_addrlen) == 0 && ::listen(s, 1) == 0) {
            sockaddr_storage ss; int l = sizeof(ss);
            if (getsockname(s, (sockaddr*)&ss, &l) == 0) { int port = 0; AddrText((sockaddr*)&ss, &port); m_port = port; ok = true; }
        }
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    }
    freeaddrinfo(res);
    if (!ok) { err = WsErr("cannot listen", WSAGetLastError()); if (s != INVALID_SOCKET) closesocket(s); return false; }
    m_sock = (size_t)s;
    return true;
}

} // namespace vm2b
