// VM2Backup — HTTP/1.1 client over NetConn (plain or TLS), made for the ESXi access (vSphere SOAP + datastore file access):
//   keep-alive with automatic reconnect on a stale connection, request bodies from memory or streamed from a Source (Content-Length,
//   or chunked when the length is unknown), `Expect: 100-continue`, response bodies as a streaming Source (Content-Length, chunked or
//   close-delimited; never buffered as a whole), a cookie jar (name=value pairs, no path / domain / expiry handling), Basic auth.
//   Redirects are NOT followed (the 3xx is returned), gzip is not requested or decoded.
#pragma once
#include "stream.h"
#include "tls.h"

namespace vm2b {

struct HttpHeaders {
    Vec<Str> names, values;
    void add(const Str& n, const Str& v) { names.push_back(n); values.push_back(v); }
    void set(const Str& n, const Str& v);                    // replaces all headers of that name
    const Str* get(const char* n) const;                     // first one, case-insensitive; nullptr when absent
    Str value(const char* n, const char* def = "") const { const Str* s = get(n); return s ? *s : Str(def); }
    size_t size() const { return names.size(); }
    void clear() { names.clear(); values.clear(); }
};

struct HttpOptions {
    Str host; int port; bool tls;
    TlsOptions tlsOpt;             // verify / fingerprint / tls_min / ca_file / timeoutMs (idle timeout) / abort
    bool expect100;                // send "Expect: 100-continue" with streamed bodies and wait for the go-ahead
    Str userAgent;
    HttpOptions() : port(0), tls(false), expect100(false), userAgent("VM2Backup") {}
    // Keys of the target configs: host, port, tls (bool) or scheme "https", verify, fingerprint, tls_min, ca_file, timeout (s) / timeout_ms.
    static HttpOptions FromJson(const Json& cfg);
};

struct HttpResponse {
    int status;
    Str reason;
    HttpHeaders headers;
    Source* body;                  // owned by the client; valid until the next request or close(); never nullptr after a successful request
    i64 contentLength;             // -1 when unknown (chunked / close-delimited)
    HttpResponse() : status(0), body(nullptr), contentLength(-1) {}
};

class HttpClient {
public:
    explicit HttpClient(const HttpOptions& o);
    ~HttpClient();

    // Request with a memory body (n may be 0 with p == nullptr).  false = no response could be had (see err / transient / certError).
    bool request(const Str& method, const Str& path, const HttpHeaders& h, const void* body, size_t n, HttpResponse& rsp);
    // Request body streamed from `body`; bodyLen >= 0 sends Content-Length, < 0 sends chunked.
    bool requestStream(const Str& method, const Str& path, const HttpHeaders& h, Source* body, i64 bodyLen, HttpResponse& rsp);
    // Convenience: sends a memory body and reads the whole response body (at most maxSize bytes) into `out`.
    bool requestBytes(const Str& method, const Str& path, const HttpHeaders& h, const void* body, size_t n, HttpResponse& rsp, Bytes& out, size_t maxSize = (size_t)64 << 20);

    void setBasicAuth(const Str& user, const Str& password);  // sent as Authorization unless the request has its own
    static Str BasicAuthValue(const Str& user, const Str& password);   // "Basic base64(user:password)"
    void setCookie(const Str& name, const Str& value);
    Str getCookie(const Str& name) const;                    // "" when absent
    void clearCookies() { m_cn.clear(); m_cv.clear(); }

    void close();                                            // drops the connection (cookies stay)
    bool connected() const { return m_conn.isOpen(); }
    int reconnects() const { return m_reconnects; }          // how often a stale connection was replaced (tests)
    int connects() const { return m_connects; }              // TCP connections opened so far
    Str peerFingerprint() const { return m_fp; }
    Str tlsVersion() const { return m_conn.tlsVersion(); }

    Str err;
    bool transient;                // a retry may help (connection lost / reset / timeout)
    bool certError;                // certificate rejected or pin mismatch (permanent); err carries the server's sha256
    bool aborted;

private:
    friend class HttpBody;
    HttpClient(const HttpClient&);
    HttpClient& operator=(const HttpClient&);
    bool doRequest(const Str& method, const Str& path, const HttpHeaders& h, const void* mem, size_t memLen, Source* src, i64 srcLen, HttpResponse& rsp);
    bool ensureConnected();
    bool readHead(bool& gotBytes, HttpResponse& rsp, bool ignoreInterim);
    bool fillBuf(bool& gotBytes);
    bool bufLine(Str& line, bool& gotBytes);
    bool failNet(const char* what);
    bool failMsg(const Str& e, bool t) { err = e; transient = t; return false; }
    void finishBody();
    void applyCookies(const HttpHeaders& h);

    HttpOptions m_o;
    NetConn m_conn;
    TlsCredRef m_cred;
    Bytes m_buf; size_t m_rpos, m_rend;       // received bytes not consumed yet
    Source* m_bodySrc;                        // the live response body (HttpBody), or nullptr
    Str m_authValue;
    Vec<Str> m_cn, m_cv;
    Str m_fp;
    int m_reconnects, m_connects;
};

} // namespace vm2b
