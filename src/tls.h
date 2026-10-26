// VM2Backup — a TCP connection that is optionally wrapped in TLS (OpenSSL, static; the socket loop is ours). Plain TCP and TLS share one
// interface so the FTP and HTTP code does not care which one it talks to:
//   connect (with timeout), send / recv (with idle timeouts and an abort flag), starttls() on an existing socket (FTP AUTH TLS),
//   shutdown (close_notify), the peer certificate fingerprint, session resumption through a shared credential (SSL_CTX + newest session).
//
// Certificate checking (TlsOptions):
//   * default: the Windows system store, chain + validity + host name (no revocation check: ESXi / NAS boxes have no CRL);
//   * fingerprint "sha256:<hex>" / "sha1:<hex>": exactly that certificate is accepted, whatever the chain, name or date say
//     (a self-signed ESXi / FTP server is used this way); any other certificate is refused;
//   * verify=false: everything is accepted (the fingerprint is still recorded);
//   * caFile: a PEM bundle of extra trusted CAs, used in addition to the system store.
// Every certificate failure puts the server certificate's SHA-256 ("sha256:<64 hex>") into the error text, so a UI can offer to
// pin it, and sets certError.  Protocol floor tlsMin "1.0" | "1.1" | "1.2" (default 1.2); TLS 1.3 is used when the server has it.
// tls_min below 1.2 also lowers the OpenSSL security level to 0 (old ESXi: TLS 1.0, SHA-1 certificates).
#pragma once
#include "base.h"

namespace vm2b {

struct TlsOptions {
    bool verify;                 // check the certificate chain and host name (ignored when a fingerprint is pinned)
    Str fingerprint;             // "sha256:<hex>" | "sha1:<hex>" | bare 64 / 40 hex digits; ':' and blanks are ignored
    Str tlsMin;                  // "1.0" | "1.1" | "1.2"
    Str caFile;                  // PEM file with extra trusted CA certificates
    int timeoutMs;               // idle timeout of every connect / send / recv step
    const volatile long* abort;  // polled in every wait loop; non-zero stops the call (error "aborted")
    TlsOptions() : verify(true), tlsMin("1.2"), timeoutMs(30000), abort(nullptr) {}
    // Reads the keys of the target configs: verify, fingerprint, tls_min, ca_file, timeout (seconds) / timeout_ms.
    static TlsOptions FromJson(const Json& cfg);
};

// A TLS client context, shared between connections so that the TLS session can be resumed (FTP data connections
// reuse the control connection's session; an HTTP client reconnecting reuses its last one).
class TlsCredRef {
public:
    TlsCredRef() : m_c(nullptr) {}
    TlsCredRef(const TlsCredRef& o);
    TlsCredRef& operator=(const TlsCredRef& o);
    ~TlsCredRef();
    bool valid() const { return m_c != nullptr; }
    void reset();
private:
    friend class NetConn;
    void* m_c;
};

class TcpListener;

class NetConn {
public:
    NetConn();
    ~NetConn();
    // Opens the TCP connection (all addresses of the name are tried); implicitTls = TLS handshake at once (ftps, https).
    // `cred`: when given and valid, its credential is used (session resumption); when valid() is false it receives the new one.
    bool connect(const Str& host, int port, bool implicitTls, const TlsOptions& o, TlsCredRef* cred = nullptr);
    // TLS handshake on the open socket (after AUTH TLS, or on a data connection once the server said 150).
    bool starttls(const Str& serverName, const TlsOptions& o, TlsCredRef* cred = nullptr);
    // Active-mode FTP: waits for the server to connect to the listener, then takes that socket.
    bool accept(TcpListener& l, const TlsOptions& o);

    bool send(const void* p, size_t n);                  // all or error
    i64 recv(void* p, size_t n);                         // >0 bytes, 0 = end of stream, -1 error (see err / timedOut)
    // Waits up to ms for data (or EOF / error): true when recv() will not block. Decrypted bytes already buffered count.
    bool waitReadable(int ms);
    bool readableNow() { return waitReadable(0); }
    void shutdown();                                     // TLS close_notify + TCP FIN, then close
    void close();                                        // drops the socket at once (no close_notify)
    bool isOpen() const { return m_sock != (size_t)-1; }
    bool isTls() const { return m_tls; }
    bool sessionReused() const { return m_resumed; }     // the TLS handshake resumed an earlier session

    Str err;                     // last error, one line
    bool timedOut;               // the last failure was an idle timeout
    bool aborted;                // the last failure was the abort flag
    bool certError;              // the last failure was a certificate / pinning rejection (permanent)
    bool tlsError;               // the last failure was a TLS protocol failure (handshake refused, bad record): permanent-ish
    bool cleanEof;               // after recv()==0: the peer ended the stream properly (close_notify, or any FIN on plain TCP)
    int timeoutMs;               // may be changed between calls
    const volatile long* abortFlag;

    Str peerFingerprint() const { return m_fp256; }      // "sha256:<hex>" of the server certificate (empty without TLS)
    Str peerFingerprintSha1() const { return m_fp1; }
    Str peerAddress() const { return m_peerIp; }         // numeric IP of the connected peer
    Str localAddress() const { return m_localIp; }
    int peerPort() const { return m_peerPort; }
    bool peerIsV6() const { return m_v6; }
    Str tlsVersion() const { return m_ver; }             // "TLS 1.2", "TLS 1.3", ... (empty for plain)
    Str sessionId() const { return m_sid; }              // hex of the TLS session id (TLS <= 1.2), empty when not known
    u64 bytesSent() const { return m_sent; }
    u64 bytesReceived() const { return m_recvd; }

private:
    friend class TcpListener;
    NetConn(const NetConn&);
    NetConn& operator=(const NetConn&);
    bool fail(const Str& e) { err = e; return false; }
    int waitSock(bool forWrite, int ms);                 // 1 ready, 0 timeout, -1 error / abort
    bool rawSendAll(const void* p, size_t n);
    i64 rawRecv(void* p, size_t n);
    bool tlsHandshake(const Str& serverName, const TlsOptions& o, TlsCredRef* cred);
    int sslWait(int sslErr);                             // 1 retry, 0 failed (err set), -1 a real TLS error
    void syncCounts();
    bool validateCert(const Str& serverName, const TlsOptions& o);
    bool tlsSend(const void* p, size_t n);
    i64 tlsRecv(void* p, size_t n);
    void freeTls();
    void sockInfo();

    size_t m_sock;               // SOCKET
    bool m_tls, m_closeNotify, m_v6, m_broken;
    void* m_cred;                // TlsCred*
    void* m_ssl;                 // SSL*
    Str m_server;
    Str m_fp256, m_fp1, m_peerIp, m_localIp, m_ver, m_sid;
    int m_peerPort;
    u64 m_sent, m_recvd;
    bool m_resumed;
};

// A listening socket for active-mode FTP.
class TcpListener {
public:
    TcpListener();
    ~TcpListener();
    // Binds an ephemeral port on the given local address (the control connection's local address).
    bool listen(const Str& localIp, Str& err);
    int port() const { return m_port; }
    void close();
private:
    friend class NetConn;
    size_t m_sock; int m_port;
};

Str TlsNormalizeFingerprint(const Str& s);               // "SHA256:AA:bb.." -> "sha256:aabb.." ("" when malformed)

} // namespace vm2b
