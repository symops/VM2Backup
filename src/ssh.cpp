// VM2Backup — the SSH client of the SCP target on libssh2 (see ssh.h). The session runs non-blocking: every libssh2 call that answers
// EAGAIN is driven by Drive(), which waits on the socket in slices of 100 ms and so can poll the abort flag, enforce the idle timeout
// and the deadline of the connect phase, and send keepalives.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <errno.h>
#include <libssh2.h>
#include "ssh.h"
#include "winutil.h"

namespace vm2b {

namespace {

const int kAborted = -2000, kTimedOut = -2001;       // results of Drive() that are not libssh2 codes

BOOL CALLBACK WsInitCb(PINIT_ONCE, PVOID, PVOID*) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); return TRUE; }
void WsInit() { static INIT_ONCE once = INIT_ONCE_STATIC_INIT; InitOnceExecuteOnce(&once, WsInitCb, nullptr, nullptr); }
u64 NowMs() { return (u64)GetTickCount64(); }

BOOL CALLBACK Ssh2InitCb(PINIT_ONCE, PVOID, PVOID*) { libssh2_init(0); return TRUE; }
void Ssh2Init() { static INIT_ONCE once = INIT_ONCE_STATIC_INIT; InitOnceExecuteOnce(&once, Ssh2InitCb, nullptr, nullptr); }

// The defaults, in the order of preference. libssh2 drops the names it does not support. No CBC ciphers, no SHA-1 kex / MAC
// except at the end (servers of the ESXi 6 generation need group14-sha1, ssh-rsa, hmac-sha1).
const char kDefKex[] = "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,ecdh-sha2-nistp521,"
                       "diffie-hellman-group-exchange-sha256,diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,diffie-hellman-group14-sha256,"
                       "diffie-hellman-group14-sha1,diffie-hellman-group-exchange-sha1";
const char kDefHostKey[] = "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256,ssh-rsa";
// chacha20-poly1305 comes last, used only when a server offers nothing else: libssh2 1.11.1 runs away (CPU and memory without bound) on a
// packet whose length field was damaged (found by the corrupted_stream test, which therefore never asks for it).
const char kDefCipher[] = "aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr,chacha20-poly1305@openssh.com";
const char kDefMac[] = "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512,hmac-sha1-etm@openssh.com,hmac-sha1";

} // namespace

struct SshSession::Impl {
    SOCKET sock; LIBSSH2_SESSION* ses; SshConfig cfg; volatile long* abortFlag;
    u64 lastIo;                 // the last time bytes arrived or were accepted by the socket
    u64 deadline;               // absolute end of the connect phase (0 = none)
    int lastSockErr;            // the Winsock error of the last failed recv / send
    Vec<SshChannel*> chans;
    Impl() : sock(INVALID_SOCKET), ses(nullptr), abortFlag(nullptr), lastIo(0), deadline(0), lastSockErr(0) {}
};

namespace {

bool StopNow(const SshSession::Impl& I) { return (I.abortFlag && *I.abortFlag) || (I.deadline && NowMs() >= I.deadline); }

// libssh2 asks these for every byte: they report the real Winsock error to us and keep the "bytes are moving" clock.
ssize_t RecvCb(libssh2_socket_t s, void* buf, size_t len, int flags, void** abs)
{
    SshSession::Impl* I = (SshSession::Impl*)*abs;
    if (StopNow(*I)) return -EAGAIN;                 // Drive() sees the abort / the deadline even when the data never stops coming
    int n = recv(s, (char*)buf, (int)(len > 0x40000000 ? 0x40000000 : len), flags);
    if (n > 0) I->lastIo = NowMs();
    if (n >= 0) return n;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return -EAGAIN;
    I->lastSockErr = e;
    return -ECONNRESET;
}

ssize_t SendCb(libssh2_socket_t s, const void* buf, size_t len, int flags, void** abs)
{
    SshSession::Impl* I = (SshSession::Impl*)*abs;
    if (StopNow(*I)) return -EAGAIN;
    int n = send(s, (const char*)buf, (int)(len > 0x40000000 ? 0x40000000 : len), flags);
    if (n > 0) I->lastIo = NowMs();
    if (n >= 0) return n;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) return -EAGAIN;
    I->lastSockErr = e;
    return -ECONNRESET;
}

void KbdintCb(const char*, int, const char*, int, int numPrompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT*, LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abs)
{
    SshSession::Impl* I = (SshSession::Impl*)*abs;
    for (int i = 0; i < numPrompts; ++i) {                       // every prompt gets the password (ESXi asks "Password:" once)
        size_t n = I->cfg.password.size();
        char* t = (char*)malloc(n + 1);
        if (t) { memcpy(t, I->cfg.password.data(), n); t[n] = 0; }
        responses[i].text = t; responses[i].length = t ? (unsigned int)n : 0;
    }
}

// libssh2_keepalive_send() may answer EAGAIN with a half-sent packet; it must then be called again, unchanged, before anything else.
void KeepaliveNow(SshSession::Impl& I)
{
    u64 end = NowMs() + 2000;
    for (;;) {
        int next = 0;
        int rc = libssh2_keepalive_send(I.ses, &next);
        if (rc != LIBSSH2_ERROR_EAGAIN || NowMs() >= end) return;
        fd_set wf; FD_ZERO(&wf); FD_SET(I.sock, &wf);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 20000;
        select(0, nullptr, &wf, nullptr, &tv);
    }
}

// Runs f() (a libssh2 call) until it stops answering EAGAIN. Returns its result, kAborted or kTimedOut.
template <class F> int Drive(SshSession::Impl& I, int idleMs, F f)
{
    I.lastIo = NowMs();
    u64 lastKeep = I.lastIo;
    for (;;) {
        int rc = f();
        if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
        if (I.abortFlag && *I.abortFlag) return kAborted;
        u64 now = NowMs();
        if (I.deadline && now >= I.deadline) return kTimedOut;
        if (idleMs > 0 && (int)(now - I.lastIo) >= idleMs) return kTimedOut;
        int dir = libssh2_session_block_directions(I.ses);
        fd_set rf, wf; FD_ZERO(&rf); FD_ZERO(&wf);
        if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(I.sock, &rf);
        if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(I.sock, &wf);
        timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;
        if (!(dir & 3)) Sleep(5); else select(0, &rf, &wf, nullptr, &tv);
        // Keepalive (keeps NAT / firewall state; the reply counts as traffic). libssh2 wants a pending call repeated unchanged, so a packet of
        // ours must never be slipped in while an outgoing one is half sent: only when the line has been quiet and we merely wait to receive.
        if (I.cfg.keepaliveMs > 0 && dir == LIBSSH2_SESSION_BLOCK_INBOUND && (int)(now - I.lastIo) >= 1000 && (int)(now - lastKeep) >= 1000) {
            u64 keep = I.lastIo; KeepaliveNow(I); I.lastIo = keep; lastKeep = now;
        }
    }
}

Str B64NoPad(const u8* p, size_t n) { Str s = Base64Encode(p, n); while (s.ends_with("=")) s.resize(s.size() - 1); return s; }

} // namespace

SshSession::SshSession() : transient(false), hostKeyUntrusted(false), rekeys(0), m_i(new Impl()), m_state(St_Closed), m_abort(nullptr) { Ssh2Init(); }
SshSession::~SshSession() { disconnect(); delete m_i; }
void SshSession::set_abort_flag(volatile long* f) { m_abort = f; m_i->abortFlag = f; }

void SshSession::disconnect()
{
    Impl& I = *m_i;
    
    for (size_t i = 0; i < I.chans.size(); ++i) { I.chans[i]->m_ch = nullptr; }      // libssh2 frees them with the session
    I.chans.clear();
    if (I.ses) {
        I.abortFlag = nullptr; I.deadline = 0;
        if (m_state == St_Ready) { libssh2_session_set_blocking(I.ses, 1); libssh2_session_set_timeout(I.ses, 1000); libssh2_session_disconnect_ex(I.ses, SSH_DISCONNECT_BY_APPLICATION, "bye", ""); }
        libssh2_session_free(I.ses); I.ses = nullptr; 
    }
    if (I.sock != INVALID_SOCKET) { closesocket(I.sock); I.sock = INVALID_SOCKET; }
    m_state = St_Closed;
}

// Maps a libssh2 result to an English message and the transient flag (a retry may help). No secret can be in libssh2's texts.
bool SshSession::fail_rc(int rc, const char* what)
{
    Impl& I = *m_i;
    // after an abort, a timeout or a transport error libssh2 may sit in the middle of an operation: the session cannot be used again
    bool chanLevel = rc == LIBSSH2_ERROR_CHANNEL_FAILURE || rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED || rc == LIBSSH2_ERROR_REQUEST_DENIED ||
                     rc == LIBSSH2_ERROR_CHANNEL_CLOSED || rc == LIBSSH2_ERROR_CHANNEL_EOF_SENT;
    if (!chanLevel && m_state == St_Ready) m_state = St_Dead;
    Str detail;
    if (I.ses && rc != kAborted && rc != kTimedOut) { char* m = nullptr; int ml = 0; libssh2_session_last_error(I.ses, &m, &ml, 0); if (m && ml > 0) detail = Str(m, (size_t)ml); }
    switch (rc) {
    case kAborted: return fail("aborted", false);
    case kTimedOut: return fail("timed out waiting for the server", true);
    case LIBSSH2_ERROR_SOCKET_DISCONNECT: case LIBSSH2_ERROR_SOCKET_RECV: case LIBSSH2_ERROR_SOCKET_SEND: case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT: case LIBSSH2_ERROR_TIMEOUT: case LIBSSH2_ERROR_BANNER_RECV: case LIBSSH2_ERROR_BANNER_SEND:
        return fail(I.lastSockErr ? Str::fmt("connection lost (error %d)", I.lastSockErr) : Str("connection lost"), true);
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED: case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED: case LIBSSH2_ERROR_KEYFILE_AUTH_FAILED:
        return fail("authentication failed (wrong user name, password or key)", false);
    case LIBSSH2_ERROR_PASSWORD_EXPIRED: return fail("authentication failed: the password has expired", false);
    case LIBSSH2_ERROR_METHOD_NONE: case LIBSSH2_ERROR_KEX_FAILURE: case LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE: case LIBSSH2_ERROR_ALGO_UNSUPPORTED:
    case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED: case LIBSSH2_ERROR_HOSTKEY_INIT: case LIBSSH2_ERROR_HOSTKEY_SIGN:
        return fail(Str("key exchange failed (no common algorithm, or the server's key was refused)") + (detail.empty() ? Str() : ": " + detail), false);
    case LIBSSH2_ERROR_INVALID_MAC: case LIBSSH2_ERROR_MAC_FAILURE: case LIBSSH2_ERROR_DECRYPT: case LIBSSH2_ERROR_ENCRYPT: case LIBSSH2_ERROR_PROTO:
        return fail("the SSH stream is damaged" + (detail.empty() ? Str() : ": " + detail), true);
    case LIBSSH2_ERROR_FILE: return fail("cannot read the key file" + (detail.empty() ? Str() : ": " + detail), false);
    case LIBSSH2_ERROR_ALLOC: return fail("out of memory", false);
    default: break;
    }
    return fail(Str(what) + (detail.empty() ? Str::fmt(" (SSH error %d)", rc) : ": " + detail), false);
}

bool SshSession::connect_socket(const SshConfig& cfg)
{
    Impl& I = *m_i;
    WsInit();
    addrinfo hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    Str portS = Str::fmt("%d", cfg.port);
    int gr = getaddrinfo(cfg.host.c_str(), portS.c_str(), &hints, &res);
    if (gr != 0 || !res) return fail("cannot resolve the host name " + cfg.host, false);
    Str lastErr = "cannot connect";
    for (addrinfo* a = res; a; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        int rc = ::connect(s, a->ai_addr, (int)a->ai_addrlen);
        bool ok = rc == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            u64 t0 = NowMs();
            for (;;) {
                if (m_abort && *m_abort) { closesocket(s); freeaddrinfo(res); return fail("aborted", false); }
                fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef);
                timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;
                int r = select(0, nullptr, &wf, &ef, &tv);
                if (r > 0) {
                    int so = 0, sl = sizeof(so); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &sl);
                    if (FD_ISSET(s, &wf) && so == 0) ok = true; else lastErr = Str::fmt("cannot connect to %s:%d (error %d)", cfg.host.c_str(), cfg.port, so);
                    break;
                }
                if ((int)(NowMs() - t0) >= cfg.connectTimeoutMs) { lastErr = Str::fmt("timed out connecting to %s:%d", cfg.host.c_str(), cfg.port); break; }
            }
        } else if (!ok) lastErr = Str::fmt("cannot connect to %s:%d (error %d)", cfg.host.c_str(), cfg.port, WSAGetLastError());
        if (ok) {
            int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
            setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
            int buf = 1 << 20; setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf)); setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
            I.sock = s; freeaddrinfo(res); I.lastIo = NowMs();
            return true;
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return fail(lastErr, true);
}

bool SshSession::authenticate(const SshConfig& cfg)
{
    Impl& I = *m_i;
    const char* u = cfg.user.c_str(); unsigned ul = (unsigned)cfg.user.size();
    // "none" first: it tells which methods the server wants (and some servers let a user in without one)
    char* list = nullptr;
    int rc = Drive(I, cfg.ioTimeoutMs, [&]() -> int {
        list = libssh2_userauth_list(I.ses, u, ul);
        if (list) return 0;
        return libssh2_userauth_authenticated(I.ses) ? 0 : libssh2_session_last_errno(I.ses);
    });
    if (rc != 0) return fail_rc(rc, "authentication failed");
    if (libssh2_userauth_authenticated(I.ses)) return true;
    Str methods = list ? Str(list) : Str();
    auto offers = [&](const char* m) { Str h = "," + methods + ","; Str n = Str(",") + m + ","; return h.find(n.c_str()) >= 0; };
    int lastRc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    bool tried = false;
    if (!cfg.keyFile.empty() || !cfg.keyText.empty()) {
        if (offers("publickey")) {
            tried = true;
            if (!cfg.keyText.empty()) rc = Drive(I, cfg.ioTimeoutMs, [&]() { return libssh2_userauth_publickey_frommemory(I.ses, u, ul, nullptr, 0, cfg.keyText.data(), cfg.keyText.size(), cfg.keyPassphrase.empty() ? nullptr : cfg.keyPassphrase.c_str()); });
            else rc = Drive(I, cfg.ioTimeoutMs, [&]() { return libssh2_userauth_publickey_fromfile_ex(I.ses, u, ul, nullptr, cfg.keyFile.c_str(), cfg.keyPassphrase.empty() ? nullptr : cfg.keyPassphrase.c_str()); });
            if (rc == 0) return true;
            if (rc == kAborted || rc == kTimedOut || rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_SOCKET_RECV || rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_FILE) return fail_rc(rc, "public key authentication failed");
            lastRc = rc;
        } else if (cfg.password.empty()) return fail("the server does not accept public keys", false);
    }
    if (!cfg.password.empty()) {
        if (offers("password")) {
            tried = true;
            rc = Drive(I, cfg.ioTimeoutMs, [&]() { return libssh2_userauth_password_ex(I.ses, u, ul, cfg.password.c_str(), (unsigned)cfg.password.size(), nullptr); });
            if (rc == 0) return true;
            if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED) return fail_rc(rc, "password authentication failed");
            lastRc = rc;
        }
        if (offers("keyboard-interactive")) {
            tried = true;
            rc = Drive(I, cfg.ioTimeoutMs, [&]() { return libssh2_userauth_keyboard_interactive_ex(I.ses, u, ul, KbdintCb); });
            if (rc == 0) return true;
            if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED) return fail_rc(rc, "keyboard-interactive authentication failed");
            lastRc = rc;
        }
    }
    if (!tried) return fail("the server accepts none of the available login methods (it offers: " + (methods.empty() ? Str("nothing") : methods) + ")", false);
    return fail_rc(lastRc, "authentication failed");
}

bool SshSession::connect(const SshConfig& cfg)
{
    
    disconnect();
    err.clear(); transient = false; hostKeyUntrusted = false; fingerprint.clear(); serverVersion.clear(); kexName.clear(); cipherName.clear(); macName.clear(); hostKeyName.clear(); rekeys = 0;
    Impl& I = *m_i;
    I.cfg = cfg; I.abortFlag = m_abort; I.lastSockErr = 0;
    I.deadline = 0;
    if (!connect_socket(cfg)) { disconnect(); return false; }
    I.ses = libssh2_session_init_ex(nullptr, nullptr, nullptr, &I);
    if (!I.ses) { disconnect(); return fail("out of memory", false); }
    libssh2_session_callback_set(I.ses, LIBSSH2_CALLBACK_RECV, (void*)RecvCb);
    libssh2_session_callback_set(I.ses, LIBSSH2_CALLBACK_SEND, (void*)SendCb);
    libssh2_session_set_blocking(I.ses, 0);
    libssh2_session_banner_set(I.ses, "SSH-2.0-VM2Backup");
    struct Pref { int type; const Str* list; const char* def; } prefs[] = {
        { LIBSSH2_METHOD_KEX, &cfg.kexList, kDefKex }, { LIBSSH2_METHOD_HOSTKEY, &cfg.hostKeyList, kDefHostKey },
        { LIBSSH2_METHOD_CRYPT_CS, &cfg.cipherList, kDefCipher }, { LIBSSH2_METHOD_CRYPT_SC, &cfg.cipherList, kDefCipher },
        { LIBSSH2_METHOD_MAC_CS, &cfg.macList, kDefMac }, { LIBSSH2_METHOD_MAC_SC, &cfg.macList, kDefMac } };
    for (const Pref& p : prefs) {
        Str l = p.list->empty() ? Str(p.def) : *p.list;
        if (libssh2_session_method_pref(I.ses, p.type, l.c_str()) != 0) { disconnect(); return fail("none of the configured SSH algorithms is supported: " + l, false); }
    }
    libssh2_session_method_pref(I.ses, LIBSSH2_METHOD_COMP_CS, "none"); libssh2_session_method_pref(I.ses, LIBSSH2_METHOD_COMP_SC, "none");
    // the connect phase (handshake + login) as a whole must not last forever, whatever dribbles in
    int phase = cfg.connectTimeoutMs > cfg.ioTimeoutMs ? cfg.connectTimeoutMs : cfg.ioTimeoutMs;
    I.deadline = NowMs() + (u64)phase * 3;
    int rc = Drive(I, cfg.ioTimeoutMs, [&]() { return libssh2_session_handshake(I.ses, (libssh2_socket_t)I.sock); });
    if (rc != 0) { bool ok = fail_rc(rc, "SSH handshake failed"); disconnect(); return ok; }
    // the host key
    const char* h = libssh2_hostkey_hash(I.ses, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!h) { disconnect(); return fail("cannot read the host key of the server", false); }
    Str fp = "SHA256:" + B64NoPad((const u8*)h, 32);
    fingerprint = fp;
    bool trusted = cfg.insecureAcceptHostKey;
    Str want = cfg.hostKey.trim(); while (want.ends_with("=")) want.resize(want.size() - 1);
    if (!trusted && !want.empty() && want == fp) trusted = true;
    for (const Str& k : cfg.knownFingerprints) { Str kk = k.trim(); while (kk.ends_with("=")) kk.resize(kk.size() - 1); if (!kk.empty() && kk == fp) trusted = true; }
    if (!trusted) {
        hostKeyUntrusted = true;
        fail(want.empty() ? "the host key is not trusted yet: " + fp : "the host key does not match the configured one: server presents " + fp, false);
        disconnect(); return false;
    }
    const char* banner = libssh2_session_banner_get(I.ses); if (banner) serverVersion = banner;
    const char* m;
    if ((m = libssh2_session_methods(I.ses, LIBSSH2_METHOD_KEX))) kexName = m;
    if ((m = libssh2_session_methods(I.ses, LIBSSH2_METHOD_HOSTKEY))) hostKeyName = m;
    if ((m = libssh2_session_methods(I.ses, LIBSSH2_METHOD_CRYPT_CS))) cipherName = m;
    if ((m = libssh2_session_methods(I.ses, LIBSSH2_METHOD_MAC_CS))) macName = m;
    if (!authenticate(cfg)) { disconnect(); return false; }
    I.deadline = 0;
    if (cfg.keepaliveMs > 0) libssh2_keepalive_config(I.ses, 1, (unsigned)(cfg.keepaliveMs < 2000 ? 2 : (cfg.keepaliveMs + 999) / 1000));   // libssh2: 2 s at least
    err.clear(); transient = false;
    m_state = St_Ready;
    return true;
}

bool SshSession::idle_tick()
{
    Impl& I = *m_i;
    if (m_state != St_Ready || !I.ses || I.cfg.keepaliveMs <= 0) return m_state == St_Ready;
    KeepaliveNow(I);
    return true;
}

SshChannel* SshSession::open_session()
{
    Impl& I = *m_i;
    if (m_state != St_Ready) { err = "not connected"; transient = true; return nullptr; }
    LIBSSH2_CHANNEL* ch = nullptr;
    int rc = 0;
    // A server with MaxSessions 1 frees the previous channel a moment after our CLOSE: a refusal is tried again for a few seconds.
    for (int attempt = 0; attempt < 30; ++attempt) {
        rc = Drive(I, I.cfg.ioTimeoutMs, [&]() -> int {
            ch = libssh2_channel_open_ex(I.ses, "session", 7, 2 * 1024 * 1024, 32768, nullptr, 0);
            if (ch) return 0;
            int e = libssh2_session_last_errno(I.ses);
            return e == 0 ? LIBSSH2_ERROR_CHANNEL_FAILURE : e;
        });
        if (rc != LIBSSH2_ERROR_CHANNEL_FAILURE) break;
        Sleep(100);
        if (m_abort && *m_abort) { rc = kAborted; break; }
    }
    if (rc != 0) {
        if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE) { char* m = nullptr; int ml = 0; libssh2_session_last_error(I.ses, &m, &ml, 0); fail("the server refused to open a session channel" + (m && ml ? Str(": ") + Str(m, (size_t)ml) : Str()), false); }
        else fail_rc(rc, "cannot open a channel");
        return nullptr;
    }
    SshChannel* c = new SshChannel(this, ch);
    I.chans.push_back(c);
    return c;
}

bool SshSession::exec(const Str& command, Bytes& out, Bytes& errOut, int& status, size_t maxOut)
{
    out.clear(); errOut.clear(); status = -1;
    SshChannel* ch = open_session();
    if (!ch) return false;
    struct Guard { SshChannel* c; ~Guard() { delete c; } } guard{ ch };
    if (!ch->exec(command)) return fail(ch->err, ch->transient);
    u8 buf[32768];
    for (;;) {
        i64 k = ch->read(buf, sizeof buf);
        if (k < 0) return fail(ch->err, ch->transient);
        if (k == 0) break;
        if (out.size() + (size_t)k > maxOut) return fail("the command printed too much output", false);
        size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf, (size_t)k);
    }
    if (!ch->wait_close(2000)) return fail(ch->err, ch->transient);
    errOut = ch->stderr_data();
    status = ch->exit_status();
    return true;
}

// ============================== channels ====================================================================================
SshChannel::SshChannel(SshSession* s, void* ch) : transient(false), m_s(s), m_ch(ch), m_exitStatus(-1), m_closeSent(false) {}

SshChannel::~SshChannel()
{
    
    SshSession::Impl& I = *m_s->m_i;
    for (size_t i = 0; i < I.chans.size(); ++i) if (I.chans[i] == this) { I.chans.erase(i); break; }
    if (m_ch && I.ses && m_s->m_state == SshSession::St_Ready) {
        LIBSSH2_CHANNEL* ch = (LIBSSH2_CHANNEL*)m_ch;
        I.deadline = 0;
        Drive(I, 2000, [&]() { return libssh2_channel_free(ch); });
    }
    m_ch = nullptr;
}

bool SshChannel::fail_rc(int rc, const char* what)
{
    if (m_s->fail_rc(rc, what)) return true;
    return fail(m_s->err, m_s->transient);
}

bool SshChannel::exec(const Str& command)
{
    if (!m_ch) return fail("the connection is closed", true);
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    int rc = Drive(I, I.cfg.ioTimeoutMs, [&]() { return libssh2_channel_exec((LIBSSH2_CHANNEL*)m_ch, command.c_str()); });
    if (rc != 0) {
        if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE || rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED || rc == LIBSSH2_ERROR_REQUEST_DENIED) return fail("the server refused to run the command", false);
        return fail_rc(rc, "cannot start the command");
    }
    return true;
}

// libssh2_channel_eof() says "not yet" while any data packet of the channel, stderr included, is still unread: so at the end of stdout the
// stderr text is taken (it is kept for stderr_data()) and the EOF flag asked again. Done only when stdout has nothing, never in between reads.
bool SshChannel::at_eof()
{
    LIBSSH2_CHANNEL* ch = (LIBSSH2_CHANNEL*)m_ch;
    if (libssh2_channel_eof(ch)) return true;
    drain_stderr();
    return libssh2_channel_eof(ch) != 0;
}

void SshChannel::drain_stderr()
{
    if (!m_ch) return;
    u8 buf[4096];
    for (int i = 0; i < 16; ++i) {
        ssize_t n = libssh2_channel_read_ex((LIBSSH2_CHANNEL*)m_ch, SSH_EXTENDED_DATA_STDERR, (char*)buf, sizeof buf);
        if (n <= 0) break;
        if (m_errBuf.size() < ((size_t)64 << 10)) { size_t o = m_errBuf.size(); m_errBuf.resize(o + (size_t)n); memcpy(m_errBuf.data() + o, buf, (size_t)n); }
    }
}

i64 SshChannel::read(void* p, size_t n)
{
    if (!m_ch) return fail("the connection is closed", true) ? 0 : -1;
    if (n == 0) return 0;
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    i64 got = 0;
    int rc = Drive(I, I.cfg.ioTimeoutMs, [&]() -> int {
        ssize_t k = libssh2_channel_read_ex((LIBSSH2_CHANNEL*)m_ch, 0, (char*)p, n);
        if (k > 0) { got = (i64)k; return 0; }
        if (k == 0) {                                   // libssh2 also answers 0 for "nothing yet": only the EOF flag says it is the end
            if (at_eof()) { got = 0; return 0; }
            return LIBSSH2_ERROR_EAGAIN;
        }
        return (int)k;
    });
    if (rc == 0) return got;
    if (rc == LIBSSH2_ERROR_CHANNEL_CLOSED) return 0;
    fail_rc(rc, "cannot read from the server");
    return -1;
}

bool SshChannel::read_exact(void* p, size_t n)
{
    u8* d = (u8*)p; size_t got = 0;
    while (got < n) {
        i64 k = read(d + got, n - got);
        if (k < 0) return false;
        if (k == 0) { err.clear(); transient = true; return false; }
        got += (size_t)k;
    }
    return true;
}

bool SshChannel::write(const void* p, size_t n)
{
    
    if (!m_ch) return fail("the connection is closed", true);
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    const char* d = (const char*)p; size_t left = n;
    while (left) {
        ssize_t w = 0;
        int rc = Drive(I, I.cfg.ioTimeoutMs, [&]() -> int {
            ssize_t k = libssh2_channel_write_ex((LIBSSH2_CHANNEL*)m_ch, 0, d, left);
            if (k > 0) { w = k; return 0; }
            if (k == 0) return LIBSSH2_ERROR_EAGAIN;     // nothing accepted (window closed): wait like for EAGAIN, never spin
            return (int)k;
        });
        if (rc != 0) return fail_rc(rc, "cannot write to the server");
        if ((size_t)w > left) return fail("internal error: libssh2 reported too many bytes", false);
        d += w; left -= (size_t)w;
    }
    return true;
}

bool SshChannel::send_eof()
{
    if (!m_ch) return fail("the connection is closed", true);
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    int rc = Drive(I, I.cfg.ioTimeoutMs, [&]() { return libssh2_channel_send_eof((LIBSSH2_CHANNEL*)m_ch); });
    return rc == 0 ? true : fail_rc(rc, "cannot finish the transfer");
}

void SshChannel::close()
{
    if (!m_ch || m_closeSent) return;
    m_closeSent = true;
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    if (m_s->m_state != SshSession::St_Ready) return;
    LIBSSH2_CHANNEL* ch = (LIBSSH2_CHANNEL*)m_ch;
    Drive(I, 2000, [&]() { return libssh2_channel_close(ch); });
}

bool SshChannel::wait_close(int graceMs)
{
    if (!m_ch) return fail("the connection is closed", true);
    SshSession::Impl& I = *m_s->m_i; I.deadline = 0;
    LIBSSH2_CHANNEL* ch = (LIBSSH2_CHANNEL*)m_ch;
    int ms = graceMs < 0 ? I.cfg.ioTimeoutMs : graceMs;
    // wait_closed() wants the remote EOF to be known, and that is learned only by reading: take what is left of stdout first
    int rc = Drive(I, ms, [&]() -> int {
        char junk[512];
        for (;;) { ssize_t k = libssh2_channel_read_ex(ch, 0, junk, sizeof junk); if (k > 0) continue; if (k < 0 && k != LIBSSH2_ERROR_CHANNEL_CLOSED) return (int)k; break; }
        return at_eof() ? 0 : LIBSSH2_ERROR_EAGAIN;
    });
    // Most servers (OpenSSH) close a session only after the client's EOF / CLOSE, so we send ours: libssh2_channel_close() sends it and waits
    // for the server's. The exit status arrives before the server's EOF, so it is known by now.
    if (rc == 0) rc = Drive(I, ms, [&]() { return libssh2_channel_close(ch); });
    m_closeSent = true;
    if (rc == kTimedOut && graceMs >= 0) { m_exitStatus = libssh2_channel_get_exit_status(ch); return true; }
    if (rc != 0) return fail_rc(rc, "the server did not close the channel");
    m_exitStatus = libssh2_channel_get_exit_status(ch);
    return true;
}

const Bytes& SshChannel::stderr_data()
{
    drain_stderr();
    return m_errBuf;
}

} // namespace vm2b
