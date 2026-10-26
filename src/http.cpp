// HTTP/1.1 client, see http.h.
#include "http.h"
#include "winutil.h"
#include <windows.h>

namespace vm2b {

// ---- headers / options -----------------------------------------------------------------------------------------------
static bool IEq(const Str& a, const char* b)
{
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) { char x = a[i], y = b[i]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return false; }
    return true;
}

void HttpHeaders::set(const Str& n, const Str& v)
{
    for (size_t i = names.size(); i-- > 0;) if (IEq(names[i], n.c_str())) { names.erase(i); values.erase(i); }
    add(n, v);
}

const Str* HttpHeaders::get(const char* n) const
{
    for (size_t i = 0; i < names.size(); ++i) if (IEq(names[i], n)) return &values[i];
    return nullptr;
}

HttpOptions HttpOptions::FromJson(const Json& c)
{
    HttpOptions o;
    o.host = c.get("host").str();
    o.tls = c.get("tls").boolean(c.get("scheme").str() == "https");
    o.port = (int)c.get("port").integer(o.tls ? 443 : 80);
    o.tlsOpt = TlsOptions::FromJson(c);
    o.expect100 = c.get("expect100").boolean(false);
    return o;
}

Str HttpClient::BasicAuthValue(const Str& user, const Str& password)
{
    Str s = user + ":" + password;
    return "Basic " + Base64Encode(s.data(), s.size());
}

// ---- the response body -----------------------------------------------------------------------------------------------
class HttpBody : public Source {
public:
    enum Mode { None, Length, Chunked, UntilClose };
    HttpBody(HttpClient* c, Mode m, u64 len, bool keepAlive) : m_c(c), m_mode(m), m_left(len), m_total(len), m_got(0), m_keep(keepAlive), m_done(m == None), m_cs(0), m_chunkLeft(0) {
        if (m_done) finishConn();
    }
    ~HttpBody() { if (m_c && m_c->m_bodySrc == this) { m_c->m_bodySrc = nullptr; if (!m_done) m_c->close(); } }
    bool done() const { return m_done; }
    void detach() { m_c = nullptr; }
    i64 read(void* p, size_t n) override {
        if (!m_c) return fail("the HTTP client is gone", false);
        if (m_failed) return -1;
        if (m_done || n == 0) return 0;
        switch (m_mode) {
        case Length: {
            if (m_left == 0) { finish(); return 0; }
            size_t want = n < m_left ? n : (size_t)m_left;
            i64 k = raw(p, want);
            if (k < 0) return -1;
            if (k == 0) return broken(Str::fmt("connection closed after %llu of %llu body bytes", (unsigned long long)m_got, (unsigned long long)m_total));
            m_left -= (u64)k; m_got += (u64)k;
            if (m_left == 0) finish();
            return k;
        }
        case Chunked: {
            for (;;) {
                if (m_cs == 0) {                                  // chunk size line
                    Str line; if (!rawLine(line)) return -1;
                    int sc = line.find(';'); if (sc >= 0) line = line.substr(0, (size_t)sc);
                    line = line.trim();
                    if (line.empty() || line.size() > 16) return broken("bad chunk size line");
                    u64 v = 0;
                    for (size_t i = 0; i < line.size(); ++i) {
                        char ch = line[i]; int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                        if (d < 0) return broken("bad chunk size");
                        v = v * 16 + (u64)d;
                    }
                    if (v == 0) {                                  // trailers up to the empty line
                        for (;;) { Str t; if (!rawLine(t)) return -1; if (t.empty()) break; }
                        finish(); return 0;
                    }
                    m_chunkLeft = v; m_cs = 1;
                }
                if (m_cs == 1) {
                    size_t want = n < m_chunkLeft ? n : (size_t)m_chunkLeft;
                    i64 k = raw(p, want);
                    if (k < 0) return -1;
                    if (k == 0) return broken("connection closed inside a chunk");
                    m_chunkLeft -= (u64)k; m_got += (u64)k;
                    if (m_chunkLeft == 0) m_cs = 2;
                    return k;
                }
                Str crlf; if (!rawLine(crlf)) return -1;           // the CRLF after the chunk data
                if (!crlf.empty()) return broken("missing CRLF after a chunk");
                m_cs = 0;
            }
        }
        case UntilClose: {
            i64 k = raw(p, n);
            if (k < 0) return -1;
            if (k == 0) { m_done = true; m_keep = false; finishConn(); return 0; }
            m_got += (u64)k;
            return k;
        }
        default: return 0;
        }
    }
    void close() override { if (m_c && !m_done) { m_done = true; m_keep = false; m_c->close(); } }
private:
    void finishConn() { if (!m_keep && m_c) m_c->close(); }
    void finish() { m_done = true; finishConn(); }
    i64 broken(const Str& e) { m_failed = true; if (m_c) m_c->close(); return fail(e, true); }
    // bytes from the client's buffer first, then straight from the socket
    i64 raw(void* p, size_t n) {
        HttpClient* c = m_c;
        if (c->m_rpos == c->m_rend && n < 8192) {                // small reads (chunk framing): one big receive instead of many tiny ones
            c->m_rpos = c->m_rend = 0;
            i64 k = c->m_conn.recv(c->m_buf.data(), c->m_buf.size());
            if (k < 0) { m_failed = true; bool ab = c->m_conn.aborted; Str e = c->m_conn.err; c->close(); return fail(ab ? Str("aborted") : e, !ab); }
            if (k == 0) return 0;
            c->m_rend = (size_t)k;
        }
        if (c->m_rpos < c->m_rend) { size_t k = c->m_rend - c->m_rpos; if (k > n) k = n; memcpy(p, c->m_buf.data() + c->m_rpos, k); c->m_rpos += k; return (i64)k; }
        i64 k = c->m_conn.recv(p, n);
        if (k < 0) { m_failed = true; bool ab = c->m_conn.aborted; Str e = c->m_conn.err; c->close(); return fail(ab ? Str("aborted") : e, !ab); }
        if (k == 0 && m_mode == UntilClose && c->m_conn.isTls() && !c->m_conn.cleanEof) { /* no close_notify: accepted, the caller checks sizes / hashes */ }
        return k;
    }
    bool rawLine(Str& line) {
        line.clear();
        for (;;) {
            char ch; i64 k = raw(&ch, 1);
            if (k < 0) return false;
            if (k == 0) { broken("connection closed inside the chunked framing"); return false; }
            if (ch == '\n') { if (line.size() && line[line.size() - 1] == '\r') line.resize(line.size() - 1); return true; }
            line += ch;
            if (line.size() > 4096) { broken("chunk framing line too long"); return false; }
        }
    }
    HttpClient* m_c; Mode m_mode; u64 m_left, m_total, m_got; bool m_keep, m_done, m_failed = false; int m_cs; u64 m_chunkLeft;
};

// ---- client ----------------------------------------------------------------------------------------------------------
HttpClient::HttpClient(const HttpOptions& o)
    : transient(false), certError(false), aborted(false), m_o(o), m_rpos(0), m_rend(0), m_bodySrc(nullptr), m_reconnects(0), m_connects(0)
{
    if (m_o.port <= 0) m_o.port = m_o.tls ? 443 : 80;
    m_buf.resize(1 << 16);
}

HttpClient::~HttpClient()
{
    if (m_bodySrc) ((HttpBody*)m_bodySrc)->detach();
    delete m_bodySrc; m_bodySrc = nullptr;
    close();
}

void HttpClient::close()
{
    m_conn.close();
    m_rpos = m_rend = 0;
}

void HttpClient::setBasicAuth(const Str& user, const Str& password) { m_authValue = BasicAuthValue(user, password); }

void HttpClient::setCookie(const Str& name, const Str& value)
{
    for (size_t i = 0; i < m_cn.size(); ++i) if (m_cn[i] == name) { m_cv[i] = value; return; }
    m_cn.push_back(name); m_cv.push_back(value);
}

Str HttpClient::getCookie(const Str& name) const
{
    for (size_t i = 0; i < m_cn.size(); ++i) if (m_cn[i] == name) return m_cv[i];
    return Str();
}

bool HttpClient::failNet(const char* what)
{
    err = Str(what) + ": " + m_conn.err;
    aborted = m_conn.aborted;
    certError = m_conn.certError;
    transient = !(m_conn.aborted || m_conn.certError || m_conn.tlsError);
    return false;
}

bool HttpClient::ensureConnected()
{
    if (m_conn.isOpen()) return true;
    m_rpos = m_rend = 0;
    ++m_connects;
    TlsOptions to = m_o.tlsOpt;
    if (!m_conn.connect(m_o.host, m_o.port, m_o.tls, to, m_o.tls ? &m_cred : nullptr)) return failNet("cannot connect");
    if (m_o.tls) m_fp = m_conn.peerFingerprint();
    return true;
}

// Receives more bytes into the buffer. gotBytes is set when anything arrived (a retry on a stale connection is only safe before that).
bool HttpClient::fillBuf(bool& gotBytes)
{
    if (m_rpos == m_rend) m_rpos = m_rend = 0;
    else if (m_rpos > 0) { memmove(m_buf.data(), m_buf.data() + m_rpos, m_rend - m_rpos); m_rend -= m_rpos; m_rpos = 0; }
    if (m_rend == m_buf.size()) m_buf.resize(m_buf.size() * 2);
    i64 k = m_conn.recv(m_buf.data() + m_rend, m_buf.size() - m_rend);
    if (k < 0) return failNet("receive failed");
    if (k == 0) return failMsg(gotBytes ? "the server closed the connection in the middle of the response" : "the server closed the connection without a response", true);
    m_rend += (size_t)k; gotBytes = true;
    return true;
}

bool HttpClient::bufLine(Str& line, bool& gotBytes)
{
    for (;;) {
        for (size_t i = m_rpos; i < m_rend; ++i) {
            if (m_buf[i] == '\n') {
                size_t e = i; if (e > m_rpos && m_buf[e - 1] == '\r') --e;
                line = Str((const char*)m_buf.data() + m_rpos, e - m_rpos);
                m_rpos = i + 1;
                return true;
            }
        }
        if (m_rend - m_rpos > (1 << 17)) return failMsg("response header line too long", false);
        if (!fillBuf(gotBytes)) return false;
    }
}

// Reads a status line + headers. Interim 1xx responses are skipped unless ignoreInterim is false (then they are returned as they are).
bool HttpClient::readHead(bool& gotBytes, HttpResponse& rsp, bool ignoreInterim)
{
    for (int guard = 0; guard < 20; ++guard) {
        Str line;
        // tolerate stray empty lines before the status line
        do { if (!bufLine(line, gotBytes)) return false; } while (line.empty());
        if (!line.starts_with("HTTP/")) return failMsg("the server did not answer with HTTP", false);
        int sp = line.find(' ');
        if (sp < 0) return failMsg("bad HTTP status line", false);
        Str ver = line.substr(0, (size_t)sp);
        Str rest = line.substr((size_t)sp + 1);
        int st = atoi(rest.c_str());
        if (st < 100 || st > 599) return failMsg("bad HTTP status code", false);
        rsp.status = st;
        int sp2 = rest.find(' ');
        rsp.reason = sp2 >= 0 ? rest.substr((size_t)sp2 + 1) : Str();
        rsp.headers.clear();
        size_t total = 0;
        for (;;) {
            if (!bufLine(line, gotBytes)) return false;
            if (line.empty()) break;
            total += line.size();
            if (total > (1 << 18) || rsp.headers.size() > 500) return failMsg("response headers too large", false);
            int c = line.find(':');
            if (c <= 0) continue;
            rsp.headers.add(line.substr(0, (size_t)c).trim(), line.substr((size_t)c + 1).trim());
        }
        rsp.headers.add("X-Http-Version", ver);        // internal, read back by doRequest
        if (st >= 100 && st < 200 && st != 101 && ignoreInterim) continue;
        return true;
    }
    return failMsg("too many interim responses", false);
}

static bool HeaderOk(const Str& s) { for (size_t i = 0; i < s.size(); ++i) if (s[i] == '\r' || s[i] == '\n' || s[i] == 0) return false; return true; }

void HttpClient::applyCookies(const HttpHeaders& h)
{
    for (size_t i = 0; i < h.size(); ++i) {
        if (!IEq(h.names[i], "Set-Cookie")) continue;
        Str v = h.values[i];
        int sc = v.find(';'); Str nv = sc >= 0 ? v.substr(0, (size_t)sc) : v;
        int eq = nv.find('='); if (eq <= 0) continue;
        Str name = nv.substr(0, (size_t)eq).trim(), val = nv.substr((size_t)eq + 1).trim();
        bool remove = val.empty();
        Str low = v.lower();
        if (low.find("max-age=0") >= 0 && low.find("max-age=0") + 9 <= (int)low.size() && !(low.size() > (size_t)low.find("max-age=0") + 9 && low[(size_t)low.find("max-age=0") + 9] >= '1' && low[(size_t)low.find("max-age=0") + 9] <= '9')) remove = true;
        if (remove) { for (size_t k = 0; k < m_cn.size(); ++k) if (m_cn[k] == name) { m_cn.erase(k); m_cv.erase(k); break; } }
        else setCookie(name, val);
    }
}

void HttpClient::finishBody()
{
    // The previous response must be consumed before the connection is used again: a small rest is drained, a big one drops the connection.
    if (!m_bodySrc) return;
    HttpBody* b = (HttpBody*)m_bodySrc;
    if (!b->done()) {
        u8 tmp[16384]; u64 drained = 0;
        for (;;) {
            i64 k = b->read(tmp, sizeof(tmp));
            if (k <= 0) break;
            drained += (u64)k;
            if (drained > (4u << 20)) { b->close(); break; }
        }
    }
    delete b; m_bodySrc = nullptr;
}

bool HttpClient::request(const Str& method, const Str& path, const HttpHeaders& h, const void* body, size_t n, HttpResponse& rsp)
{
    return doRequest(method, path, h, body, n, nullptr, 0, rsp);
}

bool HttpClient::requestStream(const Str& method, const Str& path, const HttpHeaders& h, Source* body, i64 bodyLen, HttpResponse& rsp)
{
    return doRequest(method, path, h, nullptr, 0, body, bodyLen, rsp);
}

bool HttpClient::requestBytes(const Str& method, const Str& path, const HttpHeaders& h, const void* body, size_t n, HttpResponse& rsp, Bytes& out, size_t maxSize)
{
    if (!request(method, path, h, body, n, rsp)) return false;
    out.clear();
    u8 tmp[65536];
    for (;;) {
        i64 k = rsp.body->read(tmp, sizeof(tmp));
        if (k < 0) { err = rsp.body->err; transient = rsp.body->transient; return false; }
        if (k == 0) break;
        if (out.size() + (size_t)k > maxSize) { rsp.body->close(); return failMsg("the response is bigger than allowed", false); }
        size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, tmp, (size_t)k);
    }
    return true;
}

bool HttpClient::doRequest(const Str& method, const Str& path, const HttpHeaders& hin, const void* mem, size_t memLen, Source* src, i64 srcLen, HttpResponse& rsp)
{
    err.clear(); transient = false; certError = false; aborted = false;
    finishBody();
    rsp = HttpResponse();
    if (method.empty() || !HeaderOk(method) || !HeaderOk(path) || path.find(' ') >= 0) return failMsg("bad HTTP method or path", false);
    for (size_t i = 0; i < hin.size(); ++i) if (!HeaderOk(hin.names[i]) || !HeaderOk(hin.values[i]) || hin.names[i].empty()) return failMsg("bad HTTP header", false);
    bool replayable = src == nullptr;
    bool srcTouched = false;
    bool expect = src && m_o.expect100 && (srcLen != 0);
    bool isHead = IEq(method, "HEAD");

    Str host = m_o.host;
    if (host.find(':') >= 0 && host[0] != '[') host = "[" + host + "]";
    bool defPort = m_o.port == (m_o.tls ? 443 : 80);
    Str head = method + " " + (path.empty() ? Str("/") : path) + " HTTP/1.1\r\n";
    head += "Host: " + host + (defPort ? Str() : Str::fmt(":%d", m_o.port)) + "\r\n";
    if (!hin.get("User-Agent")) head += "User-Agent: " + m_o.userAgent + "\r\n";
    if (!hin.get("Accept")) head += "Accept: */*\r\n";
    if (!hin.get("Authorization") && !m_authValue.empty()) head += "Authorization: " + m_authValue + "\r\n";
    if (!hin.get("Cookie") && !m_cn.empty()) {
        Str ck; for (size_t i = 0; i < m_cn.size(); ++i) { if (i) ck += "; "; ck += m_cn[i] + "=" + m_cv[i]; }
        head += "Cookie: " + ck + "\r\n";
    }
    for (size_t i = 0; i < hin.size(); ++i) {
        if (IEq(hin.names[i], "Host") || IEq(hin.names[i], "Content-Length") || IEq(hin.names[i], "Transfer-Encoding")) continue;
        head += hin.names[i] + ": " + hin.values[i] + "\r\n";
    }
    if (src) {
        if (srcLen >= 0) head += Str::fmt("Content-Length: %llu\r\n", (unsigned long long)srcLen);
        else head += "Transfer-Encoding: chunked\r\n";
        if (expect) head += "Expect: 100-continue\r\n";
    } else if (memLen > 0 || IEq(method, "POST") || IEq(method, "PUT") || IEq(method, "PATCH")) head += Str::fmt("Content-Length: %llu\r\n", (unsigned long long)memLen);
    head += "\r\n";

    bool earlyResponse = false;      // the server answered before the body was sent (Expect: 100-continue refused, or an early error)
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool reused = m_conn.isOpen();
        if (reused && m_conn.readableNow()) { close(); reused = false; ++m_reconnects; }   // idle but readable = the peer closed it (or spoke out of turn)
        if (!ensureConnected()) return false;
        bool retryable = reused && replayable;
        // head (+ a small memory body in the same segment)
        Str out = head;
        if (!src && memLen > 0 && memLen <= 65536) out.append((const char*)mem, memLen);
        if (!m_conn.send(out.data(), out.size())) {
            if (retryable || (reused && !srcTouched)) { close(); ++m_reconnects; if (attempt == 0) continue; }
            return failNet("send failed");
        }
        bool gotBytes = false;
        HttpResponse tmpRsp;
        bool haveHead = false;
        if (src) {
            bool goAhead = true;
            if (expect) {
                // wait for "100 Continue" or a final answer; a server that ignores Expect gets the body after a second
                int waited = 0;
                while (waited < 1500 && !m_conn.waitReadable(100)) { waited += 100; if (m_o.tlsOpt.abort && *m_o.tlsOpt.abort) { aborted = true; return failMsg("aborted", false); } }
                if (m_conn.waitReadable(0)) {
                    if (!readHead(gotBytes, tmpRsp, false)) {
                        if (reused && !gotBytes && attempt == 0) { close(); ++m_reconnects; continue; }
                        return false;
                    }
                    if (tmpRsp.status == 100) { /* go ahead */ }
                    else { haveHead = true; goAhead = false; earlyResponse = true; }
                }
            }
            if (goAhead) {
                u64 sent = 0;
                Bytes buf; buf.resize(262144 + 32);
                for (;;) {
                    if (m_o.tlsOpt.abort && *m_o.tlsOpt.abort) { close(); aborted = true; return failMsg("aborted", false); }
                    size_t want = 262144;
                    if (srcLen >= 0 && (u64)srcLen - sent < want) want = (size_t)((u64)srcLen - sent);
                    i64 k = 0;
                    if (want) { k = src->read(buf.data() + 16, want); srcTouched = true; replayable = false; }
                    if (k < 0) { Str e = src->err; bool t = src->transient; close(); return failMsg("reading the request body failed: " + e, t); }
                    if (k == 0) {
                        if (srcLen >= 0 && sent != (u64)srcLen) { close(); return failMsg(Str::fmt("the request body ended after %llu of %lld bytes", (unsigned long long)sent, (long long)srcLen), false); }
                        if (srcLen < 0) { if (!m_conn.send("0\r\n\r\n", 5)) { if (!m_conn.timedOut) {} return failNet("send failed"); } }
                        break;
                    }
                    bool ok;
                    if (srcLen < 0) {
                        char hdr[24]; int hl = snprintf(hdr, sizeof(hdr), "%llx\r\n", (unsigned long long)k);
                        u8* start = buf.data() + 16 - hl; memcpy(start, hdr, (size_t)hl);
                        buf[16 + (size_t)k] = '\r'; buf[16 + (size_t)k + 1] = '\n';
                        ok = m_conn.send(start, (size_t)hl + (size_t)k + 2);
                    } else ok = m_conn.send(buf.data() + 16, (size_t)k);
                    if (!ok) {
                        // the server may have answered and closed (413, 401 ...): look for that answer before giving up
                        if (m_conn.aborted || m_conn.certError) return failNet("send failed");
                        Str sendErr = m_conn.err;
                        HttpResponse r2; bool g2 = false;
                        m_conn.timeoutMs = 1000;
                        if (!m_conn.isOpen()) return failNet("send failed");
                        if (m_conn.waitReadable(200) && readHead(g2, r2, true) && r2.status >= 300) { tmpRsp = r2; haveHead = true; earlyResponse = true; break; }
                        close();
                        return failMsg(sendErr, true);
                    }
                    sent += (u64)k;
                    if (srcLen >= 0 && sent == (u64)srcLen) break;
                }
            }
        } else if (memLen > 65536) {
            if (!m_conn.send(mem, memLen)) {
                if (reused && attempt == 0) { close(); ++m_reconnects; continue; }
                return failNet("send failed");
            }
        }
        if (!haveHead) {
            if (!readHead(gotBytes, tmpRsp, true)) {
                // a stale keep-alive connection fails before the first response byte: reconnect and try once more
                bool staleLike = reused && !gotBytes && attempt == 0 && transient && !aborted && (replayable || !srcTouched);
                if (staleLike) { close(); ++m_reconnects; err.clear(); continue; }
                close();
                return false;
            }
        }
        rsp.status = tmpRsp.status; rsp.reason = tmpRsp.reason;
        Str ver = tmpRsp.headers.value("X-Http-Version");
        for (size_t i = 0; i < tmpRsp.headers.size(); ++i) if (!IEq(tmpRsp.headers.names[i], "X-Http-Version")) rsp.headers.add(tmpRsp.headers.names[i], tmpRsp.headers.values[i]);
        applyCookies(rsp.headers);
        // framing
        bool keep = ver != "HTTP/1.0";
        Str conn = rsp.headers.value("Connection").lower();
        if (conn.find("close") >= 0) keep = false;
        else if (ver == "HTTP/1.0" && conn.find("keep-alive") >= 0) keep = true;
        if (earlyResponse) keep = false;                  // the request body was not (completely) sent: the stream is out of step
        HttpBody::Mode mode = HttpBody::UntilClose; u64 len = 0;
        const Str* te = rsp.headers.get("Transfer-Encoding");
        const Str* cl = rsp.headers.get("Content-Length");
        if (isHead || rsp.status == 204 || rsp.status == 304 || (rsp.status >= 100 && rsp.status < 200)) mode = HttpBody::None;
        else if (te && te->lower().find("chunked") >= 0) mode = HttpBody::Chunked;
        else if (cl) {
            const char* s = cl->c_str(); char* e = nullptr; unsigned long long v = strtoull(s, &e, 10);
            if (e == s) { close(); return failMsg("bad Content-Length in the response", false); }
            mode = HttpBody::Length; len = v; rsp.contentLength = (i64)v;
        } else keep = false;
        if (mode == HttpBody::UntilClose) keep = false;
        if (mode == HttpBody::None && cl && isHead) { rsp.contentLength = (i64)strtoull(cl->c_str(), nullptr, 10); }
        if (mode == HttpBody::Length && len == 0 && !earlyResponse) { /* empty body: the connection stays */ }
        HttpBody* b = new HttpBody(this, mode, len, keep);
        m_bodySrc = b;
        rsp.body = b;
        return true;
    }
    return failMsg("the request could not be sent", true);
}

} // namespace vm2b
