// The FTP / FTPES / FTPS target (RFC 959, 2428, 4217). A backup volume is uploaded to "<name>.part" and renamed when complete.
//
//   * control connection: kept open between files; before a command after a pause it is tested (readable = the server spoke or
//     closed -> reconnect; idle > 5 s -> NOOP) and replaced transparently; NOOP is sent while a long transfer runs ("keepalive")
//   * data connections: EPSV, falling back to PASV; a private address in the PASV reply is replaced by the control address;
//     active mode (passive=false) with PORT / EPRT; TLS on the data channel resumes the control channel's session (shared credential)
//   * errors: lost connection / reset / timeout / 421 425 426 450 451 452 and every other 4xx are transient; auth failures, 550 and
//     friends, certificate problems are permanent
#include "target.h"
#include "tls.h"
#include "winutil.h"
#include <windows.h>

namespace vm2b {

namespace {

struct Reply { int code; Str text; Reply() : code(0) {} Str first() const { int n = text.find('\n'); return n >= 0 ? text.substr(0, (size_t)n) : text; } };

class FtpTarget;

// A running transfer (writer / reader): the target must be able to cut it loose when it goes away.
struct FtpXfer {
    virtual ~FtpXfer() {}
    virtual void orphan() = 0;
};

static bool IsPrivateV4(unsigned a, unsigned b)
{
    return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a >= 224);
}

class FtpTarget : public Target {
public:
    enum Mode { Plain, Explicit, Implicit };
    FtpTarget() : m_port(21), m_mode(Plain), m_passive(true), m_keepalive(30), m_rpos(0), m_lastCtl(0), m_noops(0),
                  m_hasSize(false), m_hasMlsd(false), m_hasUtf8(false), m_epsvBroken(false), m_featKnown(false) {}
    ~FtpTarget() override { if (m_active) m_active->orphan(); m_active = nullptr; m_ctl.close(); }
    const char* kind() const override { return m_mode == Plain ? "ftp" : m_mode == Explicit ? "ftpes" : "ftps"; }

    bool connect() override;
    void close() override { if (m_active) m_active->orphan(); m_active = nullptr; m_ctl.close(); m_rbuf.clear(); m_rpos = 0; m_noops = 0; }
    Sink* open_write(const Str& name) override;
    Source* open_read(const Str& name) override;
    bool list(Vec<Str>& names) override;
    bool remove(const Str& name) override;
    bool exists(const Str& name, bool& yes) override;

    // configuration
    Str m_host, m_user, m_pass, m_dir;
    int m_port; Mode m_mode; bool m_passive; int m_keepalive;
    TlsOptions m_tls;

    // ---- used by the transfer objects ----
    struct Data { NetConn conn; TcpListener lis; bool activeMode; Data() : activeMode(false) {} };
    bool beginTransfer(const Str& cmdLine, Data& d, Reply& r);                 // data connection + command + 1xx + TLS
    bool readFinal(Reply& r, int timeoutMs);                                    // final reply of a transfer (skips NOOP answers)
    void keepAlive();
    void endTransferOk() { m_active = nullptr; m_lastCtl = (DWORD)GetTickCount64(); }
    // after an interrupted transfer: try to drain the reply for a moment, otherwise drop the control connection
    bool settleAfterBreak(int waitMs);
    bool cmd(const Str& line, Reply& r, bool secret = false);
    bool failReply(const char* what, const Reply& r);
    bool failNet(const char* what, const NetConn& c);
    bool ctlOpen() const { return m_ctl.isOpen(); }
    FtpXfer* m_active = nullptr;
    int timeoutMs() const { return m_tls.timeoutMs; }
    Str scrub(const Str& s) const { if (m_pass.size() >= 3 && s.find(m_pass.c_str()) >= 0) return s.replace_all(m_pass.c_str(), "***"); return s; }
    void touch() { m_lastCtl = (DWORD)GetTickCount64(); }
    bool hasSize() const { return m_hasSize; }

private:
    bool getLine(Str& line, int timeoutMs);
    bool readReply(Reply& r, int timeoutMs = -1);
    bool ready();
    bool openPassive(Data& d, Reply& r);
    bool openActive(Data& d, Reply& r);
    bool collect(const char* cmdLine, Str& out, Reply& r, bool& cmdRefused);
    bool changeDir();
    static bool safeName(const Str& n) { return !n.empty() && n.find('/') < 0 && n.find('\\') < 0 && n != "." && n != ".." && n.find('\r') < 0 && n.find('\n') < 0 && n.find(':') < 0; }
    static void parseNames(const Str& text, int kind, Vec<Str>& out);

    NetConn m_ctl;
    TlsCredRef m_cred;
    Str m_rbuf; size_t m_rpos;
    DWORD m_lastCtl;
    int m_noops;
    bool m_hasSize, m_hasMlsd, m_hasUtf8, m_epsvBroken, m_featKnown;
};

// ---- control channel -------------------------------------------------------------------------------------------------
bool FtpTarget::failNet(const char* what, const NetConn& c)
{
    return fail(scrub(Str(what) + ": " + c.err), !(c.certError || c.tlsError || c.aborted));
}

bool FtpTarget::failReply(const char* what, const Reply& r)
{
    bool temp = r.code >= 400 && r.code < 500;
    return fail(scrub(Str::fmt("%s: %d %s", what, r.code, r.first().c_str())), temp);
}

bool FtpTarget::getLine(Str& line, int tmo)
{
    int save = m_ctl.timeoutMs; if (tmo > 0) m_ctl.timeoutMs = tmo;
    bool ok = false;
    ULONGLONG began = 0;                                                       // set when the first byte of a line arrives
    for (;;) {
        int nl = m_rbuf.find('\n', m_rpos);
        if (nl >= 0) {
            size_t e = (size_t)nl; if (e > m_rpos && m_rbuf[e - 1] == '\r') --e;
            line = m_rbuf.substr(m_rpos, e - m_rpos); m_rpos = (size_t)nl + 1; ok = true; break;
        }
        if (m_rpos > 0) { m_rbuf = m_rbuf.substr(m_rpos); m_rpos = 0; }
        if (m_rbuf.size() > (1u << 20)) { fail("the FTP server sent an endless reply line", false); break; }
        char tmp[4096]; i64 k = m_ctl.recv(tmp, sizeof(tmp));
        if (k < 0) { failNet("control connection", m_ctl); m_ctl.close(); break; }
        if (k == 0) { fail("the FTP server closed the control connection", true); m_ctl.close(); break; }
        m_rbuf.append(tmp, (size_t)k);
        // a server that dribbles one byte at a time never trips the idle timeout: the whole line gets a budget as well
        if (!began) began = GetTickCount64();
        else if (GetTickCount64() - began > (ULONGLONG)(m_ctl.timeoutMs > 5000 ? m_ctl.timeoutMs * 4 : 20000)) { fail("the FTP server sends a reply line far too slowly", false); m_ctl.close(); break; }
    }
    m_ctl.timeoutMs = save;
    return ok;
}

bool FtpTarget::readReply(Reply& r, int tmo)
{
    r = Reply();
    Str line;
    if (!getLine(line, tmo)) return false;
    if (line.size() < 3 || !(line[0] >= '0' && line[0] <= '9') || !(line[1] >= '0' && line[1] <= '9') || !(line[2] >= '0' && line[2] <= '9')) {
        m_ctl.close(); return fail("the server did not answer with an FTP reply", false);
    }
    r.code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    r.text = line.size() > 4 ? line.substr(4) : Str();
    if (line.size() > 3 && line[3] == '-') {
        Str stop = line.substr(0, 3) + " ";
        ULONGLONG began = GetTickCount64(); int budget = (m_ctl.timeoutMs > 5000 ? m_ctl.timeoutMs * 4 : 20000);
        for (int guard = 0; guard < 5000; ++guard) {
            if (GetTickCount64() - began > (ULONGLONG)budget) { m_ctl.close(); return fail("an FTP reply that never ends", false); }
            if (!getLine(line, tmo)) return false;
            if (line.starts_with(stop.c_str()) || (line.size() == 3 && line == line.substr(0, 3) && line.starts_with(line.substr(0, 3).c_str()) && false)) { if (line.size() > 4) { r.text += "\n"; r.text += line.substr(4); } return true; }
            if (line.size() == 3 && line == stop.substr(0, 3)) return true;
            r.text += "\n"; r.text += line;
        }
        m_ctl.close(); return fail("an FTP reply with too many lines", false);
    }
    return true;
}

bool FtpTarget::cmd(const Str& line, Reply& r, bool secret)
{
    (void)secret;
    if (line.find('\r') >= 0 || line.find('\n') >= 0) return fail("bad FTP command text", false);
    if (!m_ctl.isOpen()) return fail("the FTP control connection is closed", true);
    Str out = line + "\r\n";
    if (!m_ctl.send(out.data(), out.size())) { failNet("control connection", m_ctl); m_ctl.close(); return false; }
    touch();
    if (!readReply(r)) return false;
    touch();
    if (r.code == 421) { Str t = scrub(Str::fmt("the server closed the connection: 421 %s", r.first().c_str())); m_ctl.close(); return fail(t, true); }
    return true;
}

// ---- connect ---------------------------------------------------------------------------------------------------------
bool FtpTarget::changeDir()
{
    Str d = m_dir.replace_all("\\", "/");
    Reply r;
    if (d.empty()) return true;
    size_t pos = 0;
    if (d[0] == '/') { if (!cmd("CWD /", r)) return false; if (r.code / 100 != 2) return failReply("cannot change to the root folder", r); pos = 1; }
    while (pos < d.size()) {
        int sl = d.find('/', pos);
        Str seg = sl < 0 ? d.substr(pos) : d.substr(pos, (size_t)sl - pos);
        pos = sl < 0 ? d.size() : (size_t)sl + 1;
        if (seg.empty() || seg == ".") continue;
        if (!cmd("CWD " + seg, r)) return false;
        if (r.code / 100 == 2) continue;
        if (r.code / 100 == 4) return failReply("cannot change folder", r);
        if (!cmd("MKD " + seg, r)) return false;          // not there: create it (a folder that exists but cannot be entered fails below)
        if (!cmd("CWD " + seg, r)) return false;
        if (r.code / 100 != 2) return failReply(Str::fmt("cannot enter or create the folder '%s'", seg.c_str()).c_str(), r);
    }
    return true;
}

bool FtpTarget::connect()
{
    close();
    m_hasSize = m_hasMlsd = m_hasUtf8 = false; m_featKnown = false;
    err.clear(); transient = false;
    TlsOptions o = m_tls;
    bool implicitTls = m_mode == Implicit;
    if (!m_ctl.connect(m_host, m_port, implicitTls, o, m_mode == Plain ? nullptr : &m_cred)) { failNet(Str::fmt("cannot connect to %s:%d", m_host.c_str(), m_port).c_str(), m_ctl); m_ctl.close(); return false; }
    Reply r;
    for (int i = 0; i < 4; ++i) { if (!readReply(r)) { m_ctl.close(); return false; } if (r.code != 120) break; }
    if (r.code == 421 || r.code / 100 == 4) { Reply rr = r; m_ctl.close(); return failReply("the server is not ready", rr); }
    if (r.code / 100 != 2) { Reply rr = r; m_ctl.close(); return failReply("the server refused the connection", rr); }
    if (m_mode == Explicit) {
        if (!cmd("AUTH TLS", r)) { m_ctl.close(); return false; }
        if (r.code != 234 && r.code != 334) { Reply rr = r; m_ctl.close(); fail(scrub(Str::fmt("the server does not accept explicit TLS (AUTH TLS): %d %s", rr.code, rr.first().c_str())), rr.code / 100 == 4); return false; }
        if (!m_ctl.starttls(m_host, o, &m_cred)) { failNet("TLS handshake on the control connection", m_ctl); m_ctl.close(); return false; }
    }
    if (m_mode != Plain) {
        if (!cmd("PBSZ 0", r)) { m_ctl.close(); return false; }
        if (r.code / 100 != 2) { Reply rr = r; m_ctl.close(); return failReply("PBSZ refused", rr); }
        if (!cmd("PROT P", r)) { m_ctl.close(); return false; }
        if (r.code / 100 != 2) { Reply rr = r; m_ctl.close(); return failReply("the server does not offer a protected data channel (PROT P)", rr); }
    }
    if (!cmd("USER " + m_user, r)) { m_ctl.close(); return false; }
    if (r.code == 331 || r.code == 332) {
        if (r.code == 332) { m_ctl.close(); return fail("the server wants an account (ACCT), which is not supported", false); }
        if (!cmd("PASS " + m_pass, r, true)) { m_ctl.close(); return false; }
    }
    if (r.code / 100 != 2) {
        Reply rr = r; m_ctl.close();
        if (rr.code / 100 == 4) return failReply("login failed", rr);
        if (rr.code == 530) return failAuth(scrub(Str::fmt("login failed: %d %s", rr.code, rr.first().c_str())), 530);
        return fail(scrub(Str::fmt("login failed: %d %s", rr.code, rr.first().c_str())), false);
    }
    if (cmd("FEAT", r) && r.code == 211) {
        m_featKnown = true;
        Str up = r.text.lower();
        m_hasSize = up.find("size") >= 0; m_hasMlsd = up.find("mlsd") >= 0; m_hasUtf8 = up.find("utf8") >= 0;
    } else if (!m_ctl.isOpen()) return false;
    if (m_hasUtf8) { if (!cmd("OPTS UTF8 ON", r) && !m_ctl.isOpen()) return false; }
    if (!cmd("TYPE I", r)) { m_ctl.close(); return false; }
    if (r.code / 100 != 2) { Reply rr = r; m_ctl.close(); return failReply("cannot select binary mode", rr); }
    if (!changeDir()) { Str e = err; bool t = transient; m_ctl.close(); err = e; transient = t; return false; }
    touch();
    return true;
}

// The control connection is usable for a new operation: reconnect when it is closed, spoken to, or does not answer a NOOP.
bool FtpTarget::ready()
{
    if (m_active) return fail("another transfer on this FTP target is still open", false);
    if (m_ctl.isOpen() && m_ctl.readableNow()) {
        // Something arrived while nobody asked: a 421 / EOF / reset means the connection is gone. (A TLS 1.3 session ticket also makes the
        // socket readable; the receive below consumes it and then times out, which leaves the connection alone.)
        int save = m_ctl.timeoutMs; m_ctl.timeoutMs = 50;
        char tmp[512]; i64 k = m_ctl.recv(tmp, sizeof(tmp));
        bool onlyTimeout = k < 0 && m_ctl.timedOut;
        m_ctl.timeoutMs = save;
        if (!onlyTimeout) m_ctl.close();
    }
    if (m_ctl.isOpen() && (DWORD)GetTickCount64() - m_lastCtl > 5000) {
        Reply r;
        if (!cmd("NOOP", r) || r.code / 100 != 2) m_ctl.close();
    }
    if (!m_ctl.isOpen()) return connect();
    err.clear(); transient = false;
    return true;
}

void FtpTarget::keepAlive()
{
    if (m_keepalive <= 0 || !m_ctl.isOpen()) return;
    if ((DWORD)GetTickCount64() - m_lastCtl < (DWORD)m_keepalive * 1000u) return;
    if (m_ctl.send("NOOP\r\n", 6)) { ++m_noops; touch(); }
}

bool FtpTarget::readFinal(Reply& r, int tmo)
{
    for (;;) {
        if (!readReply(r, tmo)) return false;
        if (m_noops > 0 && (r.code == 200 || (r.code >= 500 && r.code <= 504))) { --m_noops; continue; }
        break;
    }
    m_noops = 0;
    touch();
    return true;
}

bool FtpTarget::settleAfterBreak(int waitMs)
{
    if (!m_ctl.isOpen()) return false;
    Reply r;
    bool got = readFinal(r, waitMs);          // 426 / 451 / 226 ...
    if (!got || !m_ctl.isOpen()) { m_ctl.close(); return false; }
    return true;
}

// ---- data connections ------------------------------------------------------------------------------------------------
bool FtpTarget::openPassive(Data& d, Reply& r)
{
    Str host = m_ctl.peerAddress();
    int port = 0;
    bool haveEndpoint = false;
    if (!m_epsvBroken) {
        if (!cmd("EPSV", r)) return false;
        if (r.code == 229) {
            int a = r.text.find('(');
            if (a >= 0 && (size_t)a + 4 < r.text.size()) {
                char dl = r.text[(size_t)a + 1];
                // (<d><d><d>port<d>)
                size_t p = (size_t)a + 2; int cnt = 0;
                while (p < r.text.size() && cnt < 2 && r.text[p] == dl) { ++p; ++cnt; }
                if (cnt == 2 && p < r.text.size() && r.text[p] == dl) { ++p; port = atoi(r.text.c_str() + p); }
            }
            if (port > 0 && port < 65536) haveEndpoint = true;
        } else if (r.code / 100 == 4) return failReply("EPSV failed", r);
        if (!haveEndpoint) m_epsvBroken = true;         // not understood: PASV from now on
    }
    Str alt;
    if (!haveEndpoint) {
        if (!cmd("PASV", r)) return false;
        if (r.code != 227) return failReply("the server refused passive mode (PASV)", r);
        int n[6]; int cnt = 0; const char* s = r.text.c_str();
        while (*s && cnt < 6) {
            if (*s >= '0' && *s <= '9') { n[cnt++] = atoi(s); while (*s >= '0' && *s <= '9') ++s; } else ++s;
        }
        if (cnt < 6) return fail("cannot read the PASV reply", false);
        port = n[4] * 256 + n[5];
        Str ip = Str::fmt("%d.%d.%d.%d", n[0], n[1], n[2], n[3]);
        // A server behind NAT announces its private address: the control connection's address is the one that is reachable.
        if (!m_ctl.peerIsV6() && ip != host) { if (IsPrivateV4((unsigned)n[0], (unsigned)n[1])) { /* use host */ } else { alt = host; host = ip; } }
    }
    if (port <= 0 || port > 65535) return fail("the server announced a bad data port", false);
    TlsOptions o = m_tls;
    if (!d.conn.connect(host, port, false, o)) {
        if (!alt.empty() && !d.conn.certError) {                     // a public address that does not work: the control address next
            if (d.conn.connect(alt, port, false, o)) return true;
        }
        return failNet(Str::fmt("cannot open the data connection to %s:%d", host.c_str(), port).c_str(), d.conn);
    }
    return true;
}

bool FtpTarget::openActive(Data& d, Reply& r)
{
    Str local = m_ctl.localAddress(), e;
    if (!d.lis.listen(local, e)) return fail("active mode: " + e, true);
    d.activeMode = true;
    int port = d.lis.port();
    if (!m_ctl.peerIsV6()) {
        Str ip = local.replace_all(".", ",");
        if (!cmd(Str::fmt("PORT %s,%d,%d", ip.c_str(), port / 256, port % 256), r)) return false;
    } else {
        if (!cmd(Str::fmt("EPRT |2|%s|%d|", local.c_str(), port), r)) return false;
    }
    if (r.code / 100 != 2) return failReply("the server refused the active-mode address (PORT)", r);
    return true;
}

// Data connection, the transfer command and the TLS handshake on the data connection (after the 1xx reply).
bool FtpTarget::beginTransfer(const Str& cmdLine, Data& d, Reply& r)
{
    bool ok = m_passive ? openPassive(d, r) : openActive(d, r);
    if (!ok) return false;
    if (!cmd(cmdLine, r)) { d.conn.close(); return false; }
    if (r.code / 100 != 1) { Reply rr = r; d.conn.close(); r = rr; return false; }   // the caller classifies r
    if (d.activeMode) { if (!d.conn.accept(d.lis, m_tls)) { Str e = d.conn.err; settleAfterBreak(2000); return fail("active mode: " + e, true); } }
    if (m_mode != Plain) {
        if (!d.conn.starttls(m_host, m_tls, &m_cred)) {
            bool perm = d.conn.certError || d.conn.tlsError;
            Str e = scrub("TLS handshake on the data connection: " + d.conn.err);
            d.conn.close(); settleAfterBreak(2000);
            return fail(e, !perm);
        }
    }
    return true;
}

// ---- upload ----------------------------------------------------------------------------------------------------------
class FtpWriter : public Sink, public FtpXfer {
public:
    FtpWriter(FtpTarget* t, const Str& name, const Str& part) : m_t(t), m_name(name), m_part(part), m_data(new FtpTarget::Data()), m_done(false), m_dead(false) { m_buf.reserve(kBuf); }
    ~FtpWriter() override { if (!m_done) abort(); if (m_t && m_t->m_active == this) m_t->m_active = nullptr; delete m_data; }

    // Opens the data connection and starts STOR; on failure err / transient of the target describe it.
    bool start() {
        bool ok = start2();
        if (!ok) m_done = true;                       // nothing was started: no abort / cleanup needed (it would also overwrite err)
        return ok;
    }
    bool start2() {
        Reply r;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (m_t->beginTransfer("STOR " + m_part, *m_data, r)) { m_t->m_active = this; return true; }
            if (r.code / 100 == 5 && attempt == 0 && m_t->ctlOpen()) {
                // the temporary name is refused: a stale .part that cannot be overwritten? remove it once and retry
                Reply dr;
                if (m_t->cmd("DELE " + m_part, dr) && dr.code / 100 == 2) { delete m_data; m_data = new FtpTarget::Data(); continue; }
            }
            if (r.code / 100 == 1 || r.code == 0) return false;      // (a 1xx never gets here) transport error: the target has the text
            m_t->failReply(Str::fmt("the server refused to store '%s'", m_part.c_str()).c_str(), r);
            return false;
        }
        return false;
    }

    bool write(const void* p, size_t n) override {
        if (m_dead) return fail(err.empty() ? Str("upload failed") : err, transient);
        if (m_done) return fail("the upload is already finished", false);
        if (!m_t) return fail("the FTP target was closed during the upload", true);
        const u8* s = (const u8*)p;
        while (n) {
            if (m_buf.empty() && n >= kBuf) { return push(s, n); }              // a big write goes straight out
            size_t room = kBuf - m_buf.size(), k = n < room ? n : room;
            size_t o = m_buf.size(); m_buf.resize(o + k); memcpy(m_buf.data() + o, s, k);
            s += k; n -= k;
            if (m_buf.size() >= kBuf) { bool ok = push(m_buf.data(), m_buf.size()); m_buf.clear(); if (!ok) return false; }
        }
        return true;
    }

    bool finish() override {
        if (m_done) return true;
        if (m_dead) return fail(err.empty() ? Str("upload failed") : err, transient);
        if (!m_t) return fail("the FTP target was closed during the upload", true);
        if (!m_buf.empty()) { bool ok = push(m_buf.data(), m_buf.size()); m_buf.clear(); if (!ok) return false; }
        m_data->conn.shutdown();                                                // close_notify + FIN: the server sees the end of the file
        Reply r; int wait = m_t->timeoutMs() > 60000 ? m_t->timeoutMs() : 60000;
        m_done = true; m_t->m_active = nullptr;
        if (!m_t->readFinal(r, wait)) { m_dead = true; Str e = m_t->err; bool t = m_t->transient; m_t->close(); return fail(e, t); }
        if (r.code / 100 != 2) { m_dead = true; m_t->failReply("the upload was not accepted", r); return failFromTarget(); }
        // commit: RNFR / RNTO; a server that will not overwrite an existing file gets it removed first
        Reply rr;
        if (!m_t->cmd("RNFR " + m_part, rr)) return failFromTarget();
        if (rr.code != 350) { m_t->failReply(Str::fmt("cannot rename '%s' (RNFR)", m_part.c_str()).c_str(), rr); return failFromTarget(); }
        if (!m_t->cmd("RNTO " + m_name, rr)) return failFromTarget();
        if (rr.code / 100 == 5) {
            Reply first = rr, dr;
            if (m_t->cmd("DELE " + m_name, dr) && dr.code / 100 == 2) {
                if (m_t->cmd("RNFR " + m_part, rr) && rr.code == 350 && m_t->cmd("RNTO " + m_name, rr) && rr.code / 100 == 2) { m_t->touch(); return true; }
            }
            if (!m_t->ctlOpen()) return failFromTarget();
            m_t->failReply(Str::fmt("cannot rename '%s' to '%s' (RNTO refused)", m_part.c_str(), m_name.c_str()).c_str(), first);
            Str e = m_t->err; bool t = m_t->transient;
            Reply dd; m_t->cmd("DELE " + m_part, dd);                              // do not leave the finished-but-unnamed file around
            return fail(e, t);
        }
        if (rr.code / 100 != 2) { m_t->failReply(Str::fmt("cannot rename '%s' to '%s'", m_part.c_str(), m_name.c_str()).c_str(), rr); return failFromTarget(); }
        m_t->touch();
        return true;
    }

    void abort() override {
        if (m_done) return;
        bool wasDead = m_dead;
        m_done = true;
        m_data->conn.close();                                                   // hard close: the server stops waiting for data
        if (!m_t) return;
        if (m_t->m_active == this) m_t->m_active = nullptr;
        bool alive = m_t->ctlOpen() && m_t->settleAfterBreak(2000);
        if (!alive && !wasDead) alive = m_t->connect();                          // a user abort must not leave the .part behind
        if (alive) { Reply r; m_t->cmd("DELE " + m_part, r); }
    }
    void orphan() override { m_data->conn.close(); m_t = nullptr; m_done = true; }

private:
    static const size_t kBuf = 262144;
    bool failFromTarget() { err = m_t->err; transient = m_t->transient; return false; }
    bool push(const void* p, size_t n) {
        if (m_data->conn.send(p, n)) { m_t->keepAlive(); return true; }
        Str e = m_data->conn.err; bool t = !(m_data->conn.certError || m_data->conn.tlsError || m_data->conn.aborted);
        m_dead = true; m_data->conn.close();
        // the server's own verdict (426 / 451 / 552 ...) says more than "connection reset"
        Reply r; Str why; bool temp = t;
        if (m_t->ctlOpen() && m_t->readFinal(r, 1500) && r.code >= 400) { why = Str::fmt("the server aborted the upload: %d %s", r.code, r.first().c_str()); temp = r.code < 500; }
        m_t->m_active = nullptr; m_t->close();
        if (!why.empty()) return fail(m_t->scrub(why), temp);
        return fail(m_t->scrub("upload failed: " + e), t);
    }
    FtpTarget* m_t; Str m_name, m_part; Bytes m_buf; FtpTarget::Data* m_data; bool m_done, m_dead;
};

// ---- download --------------------------------------------------------------------------------------------------------
class FtpReader : public Source, public FtpXfer {
public:
    FtpReader(FtpTarget* t, i64 size) : m_t(t), m_size(size), m_got(0), m_data(new FtpTarget::Data()), m_eof(false), m_failed(false), m_closed(false) {}
    ~FtpReader() override { close(); if (m_t && m_t->m_active == this) m_t->m_active = nullptr; delete m_data; }
    bool start(const Str& name) {
        Reply r;
        if (m_t->beginTransfer("RETR " + name, *m_data, r)) { m_t->m_active = this; return true; }
        m_closed = true;                              // nothing to clean up
        if (r.code != 0 && r.code / 100 != 1) m_t->failReply(Str::fmt("cannot read '%s'", name.c_str()).c_str(), r);
        return false;
    }
    i64 read(void* p, size_t n) override {
        if (m_failed) return -1;
        if (m_eof || m_closed) return 0;
        if (!m_t) return breakWith("the FTP target was closed during the download", true);
        if (n == 0) return 0;
        m_t->keepAlive();
        i64 k = m_data->conn.recv(p, n);
        if (k > 0) {
            m_got += (u64)k;
            if (m_size >= 0 && m_got > (u64)m_size) return breakWith(Str::fmt("the file is longer than the announced size of %lld bytes", (long long)m_size), false);
            return k;
        }
        bool complete = m_size >= 0 && m_got == (u64)m_size;
        if (k < 0 && !complete) return breakWith(m_t->scrub("download failed: " + m_data->conn.err), !(m_data->conn.certError || m_data->conn.tlsError || m_data->conn.aborted));
        if (k == 0 && m_size >= 0 && m_got < (u64)m_size)
            return breakWith(Str::fmt("the data connection ended after %llu of %lld bytes", (unsigned long long)m_got, (long long)m_size), true);
        // the data ended (an end without close_notify is accepted: the byte count or the 226 reply vouches for it)
        m_data->conn.close();
        Reply r; int wait = m_t->timeoutMs() > 60000 ? m_t->timeoutMs() : 60000;
        m_t->m_active = nullptr;
        if (!m_t->readFinal(r, wait)) {
            Str e = m_t->err; bool t = m_t->transient; m_t->close();
            if (complete) { m_eof = true; return 0; }                           // all announced bytes arrived: only the 226 is missing
            return breakWith(e, t, false);
        }
        if (r.code / 100 != 2) { m_t->failReply("the download did not complete", r); return breakWith(m_t->err, m_t->transient, true); }
        m_eof = true; m_t->endTransferOk();
        return 0;
    }
    void close() override {
        if (m_closed) return;
        m_closed = true;
        if (m_eof || m_failed) { m_data->conn.close(); return; }
        m_data->conn.close();
        if (m_t) { if (m_t->m_active == this) m_t->m_active = nullptr; m_t->settleAfterBreak(2000); }
    }
    void orphan() override { m_data->conn.close(); m_t = nullptr; m_closed = true; }
private:
    i64 breakWith(const Str& e, bool t, bool dropCtl = true) {
        m_failed = true; m_data->conn.close();
        if (m_t) { m_t->m_active = nullptr; if (dropCtl) m_t->close(); }
        return fail(e, t);
    }
    FtpTarget* m_t; i64 m_size; u64 m_got; FtpTarget::Data* m_data; bool m_eof, m_failed, m_closed;
};

// ---- the target's operations -----------------------------------------------------------------------------------------
Sink* FtpTarget::open_write(const Str& name)
{
    if (!safeName(name)) { fail("bad file name: " + name, false); return nullptr; }
    if (!ready()) return nullptr;
    FtpWriter* w = new FtpWriter(this, name, name + ".part");
    if (!w->start()) {
        // a failure at this point may be a stale control connection: one transparent retry on a fresh one
        bool stale = transient && !m_ctl.isOpen();
        delete w;
        if (!stale) return nullptr;
        if (!connect()) return nullptr;
        w = new FtpWriter(this, name, name + ".part");
        if (!w->start()) { delete w; return nullptr; }
    }
    return w;
}

Source* FtpTarget::open_read(const Str& name)
{
    if (!safeName(name)) { fail("bad file name: " + name, false); return nullptr; }
    if (!ready()) return nullptr;
    i64 size = -1;
    if (m_hasSize || !m_featKnown) {
        Reply r;
        if (!cmd("SIZE " + name, r)) return nullptr;
        if (r.code == 213) { size = (i64)strtoull(r.text.trim().c_str(), nullptr, 10); }
        else if (r.code / 100 == 4) { failReply(Str::fmt("cannot read '%s'", name.c_str()).c_str(), r); return nullptr; }
        else if (r.code == 550) { failReply(Str::fmt("cannot read '%s'", name.c_str()).c_str(), r); return nullptr; }
        else m_hasSize = false;                                                 // SIZE not implemented: rely on the 226
    }
    FtpReader* rd = new FtpReader(this, size);
    if (!rd->start(name)) { delete rd; return nullptr; }
    return rd;
}

// Runs a listing command; out gets the data. cmdRefused: the server answered the command itself with an error (see r).
bool FtpTarget::collect(const char* cmdLine, Str& out, Reply& r, bool& cmdRefused)
{
    cmdRefused = false; out.clear();
    Data d;
    if (!beginTransfer(cmdLine, d, r)) { cmdRefused = r.code / 100 >= 4 && m_ctl.isOpen(); return false; }
    char buf[16384];
    for (;;) {
        i64 k = d.conn.recv(buf, sizeof(buf));
        if (k < 0) { Str e = scrub("listing failed: " + d.conn.err); bool t = !(d.conn.certError || d.conn.tlsError); d.conn.close(); close(); return fail(e, t); }
        if (k == 0) break;
        out.append(buf, (size_t)k);
        if (out.size() > (64u << 20)) { d.conn.close(); close(); return fail("the folder listing is absurdly large", false); }
        keepAlive();
    }
    d.conn.close();
    int wait = timeoutMs() > 60000 ? timeoutMs() : 60000;
    if (!readFinal(r, wait)) { Str e = err; bool t = transient; close(); return fail(e, t); }   // the listing without the server's 226 is not trusted
    if (r.code / 100 != 2) { cmdRefused = true; failReply("the listing was not completed", r); return false; }
    return true;
}

// kind 0 = NLST, 1 = MLSD, 2 = LIST (unix or DOS style)
void FtpTarget::parseNames(const Str& text, int kind, Vec<Str>& out)
{
    size_t pos = 0;
    while (pos <= text.size()) {
        int nl = text.find('\n', pos);
        Str line = nl < 0 ? text.substr(pos) : text.substr(pos, (size_t)nl - pos);
        pos = nl < 0 ? text.size() + 1 : (size_t)nl + 1;
        while (line.size() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == '\n')) line.resize(line.size() - 1);
        if (line.empty()) continue;
        Str name;
        if (kind == 0) {
            name = line;
            int sl = name.rfind('/'); if (sl >= 0) name = name.substr((size_t)sl + 1);
        } else if (kind == 1) {
            int sp = line.find(' '); if (sp < 0) continue;
            Str facts = line.substr(0, (size_t)sp).lower(); name = line.substr((size_t)sp + 1);
            if (facts.find("type=file") < 0) continue;
        } else {
            if (line.starts_with("total ")) continue;
            if (line[0] == '+') {                                                      // EPLF (publicfile, old embedded servers): "+i1.2,m123,r,s45,\tname"
                int tab = line.find('\t'); if (tab < 0) continue;
                Str facts = "," + line.substr(1, (size_t)tab - 1);
                if (facts.find(",/") >= 0 || facts.find(",r") < 0) continue;            // a folder, or not a retrievable file
                name = line.substr((size_t)tab + 1);
            } else if ((line[0] == '-' || line[0] == 'l' || line[0] == 'd') && line.size() > 10 && (line[1] == 'r' || line[1] == '-')) {
                if (line[0] == 'd') continue;
                size_t i = 0; int fields = 0;
                while (i < line.size() && fields < 8) { while (i < line.size() && line[i] == ' ') ++i; while (i < line.size() && line[i] != ' ') ++i; ++fields; }
                while (i < line.size() && line[i] == ' ') ++i;
                if (fields < 8 || i >= line.size()) continue;
                name = line.substr(i);
                if (line[0] == 'l') { int ar = name.find(" -> "); if (ar >= 0) name = name.substr(0, (size_t)ar); }
            } else if (line.size() > 18 && line[2] == '-' && line[5] == '-') {         // DOS: "10-24-26  03:04PM       <DIR>   name" / "... 12345 name"
                size_t i = 0; int fields = 0;
                while (i < line.size() && fields < 3) { while (i < line.size() && line[i] == ' ') ++i; size_t st = i; while (i < line.size() && line[i] != ' ') ++i; if (fields == 2 && line.substr(st, i - st) == "<DIR>") { i = line.size(); fields = 99; break; } ++fields; }
                if (fields >= 99) continue;
                while (i < line.size() && line[i] == ' ') ++i;
                if (i >= line.size()) continue;
                name = line.substr(i);
            } else continue;
        }
        if (name.empty() || name == "." || name == ".." || name.ends_with(".part")) continue;
        out.push_back(name);
    }
}

bool FtpTarget::list(Vec<Str>& names)
{
    names.clear();
    if (!ready()) return false;
    Str lastErr; bool lastTemp = false, anyOk = false;
    Vec<Str> found;
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 1 && m_featKnown && !m_hasMlsd) continue;
        Str out; Reply r; bool refused = false;
        bool ok = collect(kind == 0 ? "NLST" : kind == 1 ? "MLSD" : "LIST", out, r, refused);
        if (!ok) {
            if (!refused) return false;                                        // connection trouble: the caller retries
            Str low = r.text.lower();
            bool emptyMsg = low.find("no files") >= 0 || low.find("empty") >= 0 || low.find("nothing") >= 0;
            if (emptyMsg && (r.code == 450 || r.code == 550)) { anyOk = true; continue; }
            lastErr = err; lastTemp = transient;
            if (r.code / 100 == 4 && r.code != 450) return false;              // "try again later": no fallback
            if (!m_ctl.isOpen() && !connect()) return false;                    // the failed command may have cost the connection
            continue;
        }
        anyOk = true;
        found.clear(); parseNames(out, kind, found);
        if (!found.empty()) { names = found; return true; }
    }
    if (anyOk) return true;                                                    // every method agrees: the folder is empty
    return fail(lastErr.empty() ? Str("cannot list the folder") : lastErr, lastTemp);
}

bool FtpTarget::exists(const Str& name, bool& yes)
{
    yes = false;
    if (!safeName(name)) return fail("bad file name: " + name, false);
    if (!ready()) return false;
    if (m_hasSize || !m_featKnown) {
        Reply r;
        if (!cmd("SIZE " + name, r)) return false;
        if (r.code == 213) { yes = true; return true; }
        if (r.code / 100 == 4) return failReply("cannot check the file", r);
        if (r.code == 550 && m_hasSize) { yes = false; return true; }
        if (r.code != 550) m_hasSize = false;
    }
    Vec<Str> names;
    if (!list(names)) return false;
    for (const Str& n : names) if (n == name) { yes = true; break; }
    return true;
}

bool FtpTarget::remove(const Str& name)
{
    if (!safeName(name)) return fail("bad file name: " + name, false);
    if (!ready()) return false;
    Reply r;
    if (!cmd("DELE " + name, r)) return false;
    if (r.code / 100 == 2) return true;
    if (r.code / 100 == 4) return failReply(Str::fmt("cannot delete '%s'", name.c_str()).c_str(), r);
    // 550: no such file (fine) or no permission (an error): ask
    Reply dele = r; bool yes = true;
    if (!exists(name, yes)) return false;
    if (!yes) return true;
    return failReply(Str::fmt("cannot delete '%s'", name.c_str()).c_str(), dele);
}

}  // namespace

Target* NewFtpTarget(const Json& cfg, Str& err)
{
    Str type = cfg.get("type").str("ftp");
    FtpTarget* t = new FtpTarget();
    if (type == "ftp") t->m_mode = FtpTarget::Plain;
    else if (type == "ftpes") t->m_mode = FtpTarget::Explicit;
    else if (type == "ftps") t->m_mode = FtpTarget::Implicit;
    else { err = "unknown FTP type: " + type; delete t; return nullptr; }
    t->m_host = cfg.get("host").str().trim();
    if (t->m_host.empty()) { err = "FTP target: the server name is not set"; delete t; return nullptr; }
    t->m_port = (int)cfg.get("port").integer(t->m_mode == FtpTarget::Implicit ? 990 : 21);
    if (t->m_port <= 0 || t->m_port > 65535) { err = "FTP target: bad port"; delete t; return nullptr; }
    t->m_user = cfg.get("user").str("anonymous");
    t->m_pass = cfg.get("password").str(t->m_user == "anonymous" ? "anonymous@" : "");
    t->m_dir = cfg.get("directory").str();
    t->m_passive = cfg.get("passive").boolean(true);
    t->m_keepalive = (int)cfg.get("keepalive").integer(30);
    t->m_tls = TlsOptions::FromJson(cfg);
    if (t->m_user.find('\r') >= 0 || t->m_user.find('\n') >= 0 || t->m_pass.find('\r') >= 0 || t->m_pass.find('\n') >= 0) { err = "FTP target: bad user name or password"; delete t; return nullptr; }
    return t;
}

}  // namespace vm2b
