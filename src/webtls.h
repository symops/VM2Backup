// VM2Backup — server-side TLS for the administration web site (OpenSSL), and the server certificate:
// a self-signed one made here (ECDSA P-256, SHA-256, 825 days, SAN = machine name + localhost + chosen addresses) or one imported from a
// PFX (RSA or EC).  The identity that is kept on disk is {certificate DER, private key (DER) protected with DPAPI}: no key file or
// container is left behind.
//
// TLS floor: 1.2 (and 1.3), ECDHE + AEAD suites only, no client certificates, renegotiation is refused.
#pragma once
#include "base.h"

namespace vm2b {

struct WebTlsIdentity {
    Bytes certDer;                 // the leaf certificate
    Str keyProtected;              // "dpapi:..." of "pkey:" + base64(private key DER)
};

// Names: DNS names and IP literals for the subjectAltName, added to the machine name + localhost + 127.0.0.1 + ::1 (always there).
bool WebTlsGenerate(const Vec<Str>& names, WebTlsIdentity& id, Str& err);
bool WebTlsImportPfx(const Str& pfxPath, const Str& password, WebTlsIdentity& id, Str& err);
Str  WebTlsFingerprint(const Bytes& certDer);                       // "sha256:<64 hex>"
// {subject, not_before, not_after (unix), names:[...]} of a certificate (for the admin UI)
Json WebTlsDescribe(const Bytes& certDer);

// Server TLS context built from an identity; shared by all connections.
class WebTlsServer {
public:
    WebTlsServer();
    ~WebTlsServer();
    bool init(const WebTlsIdentity& id, Str& err);
    void* handle() { return m_ctx; }                   // SSL_CTX*
    Str fingerprint() const { return m_fp; }
private:
    WebTlsServer(const WebTlsServer&);
    WebTlsServer& operator=(const WebTlsServer&);
    void* m_ctx; Str m_fp;
};

enum { kTlsTimeout = -2 };

// One accepted connection (a socket that is already accepted).  Blocking socket + select timeouts.
class WebTlsConn {
public:
    WebTlsConn();
    ~WebTlsConn();
    // The handshake; `stop` is polled (non-zero = give up).  deadlineMs: the whole handshake must finish in that time.
    bool accept(size_t sock, WebTlsServer& srv, int deadlineMs, const volatile long* stop, Str& err);
    // >0 bytes of plain text, 0 = clean end, -1 error, kTlsTimeout idle timeout
    i64 recv(void* p, size_t n, int timeoutMs);
    bool send(const void* p, size_t n, int timeoutMs);
    void shutdown();                                   // close_notify (the socket stays the caller's)
    Str version() const { return m_ver; }              // "TLS 1.2" / "TLS 1.3"
    Str cipher() const { return m_cipher; }
private:
    int waitSock(bool write, int ms);
    Str m_ver, m_cipher;
    size_t m_sock; void* m_ssl;
    bool m_open, m_eof;
};

} // namespace vm2b
