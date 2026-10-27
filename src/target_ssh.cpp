// The SCP target: a folder on a server reached over SSH. See target.h for the configuration keys, ssh.h for the client.
//   scp : the classic `scp -t` / `scp -f` protocol. SCP needs the size of a file before
//         it sends it, so a write is spooled to a temporary file and sent when finish() is called, into <name>.part, then `mv`.
#include "ssh.h"
#include "target.h"
#include "winutil.h"

namespace vm2b {

namespace {

Str Quote(const Str& s)          // a POSIX shell word: '...' with embedded quotes as '\''; a leading "~" or "~/" is the user's home (quoted, it would be a folder named "~")
{
    if (s == "~") return "\"$HOME\"";
    if (s.starts_with("~/")) return "\"$HOME\"" + Quote(s.substr(1));
    Str r = "'"; for (size_t i = 0; i < s.size(); ++i) { if (s[i] == '\'') r += "'\\''"; else r.push_back(s[i]); }
    r += "'"; return r;
}

Str JoinRemote(const Str& dir, const Str& name)
{
    if (dir.empty()) return name.starts_with("~") ? "./" + name : name;      // (a file named "~" is not the home folder)
    if (dir.ends_with("/")) return dir + name;
    return dir + "/" + name;
}

bool RemoteNameOk(const Str& n)
{
    if (n.empty() || n == "." || n == ".." || n.find('/') >= 0 || n.find('\\') >= 0 || n.find('\n') >= 0 || n.find('\r') >= 0) return false;
    for (size_t i = 0; i < n.size(); ++i) if (n[i] == 0) return false;
    return n.size() < 250;
}

Str OneLine(const Bytes& b)
{
    Str r; for (size_t i = 0; i < b.size() && r.size() < 200; ++i) { char c = (char)b[i]; r.push_back((u8)c < 32 ? ' ' : c); }
    return r.trim();
}

class SshTargetBase : public Target {
public:
    SshTargetBase(const SshConfig& cfg, const Str& dir) : m_cfg(cfg), m_dir(dir), m_abort(nullptr) {}
    void set_abort(volatile long* f) { m_abort = f; m_sess.set_abort_flag(f); }
protected:
    bool open_session() {
        m_sess.set_abort_flag(m_abort);
        if (!m_sess.connect(m_cfg)) { if (m_sess.authRefused) return failAuth(m_sess.err, m_sess.authRc); return fail(m_sess.err, m_sess.transient); }
        return true;
    }
    bool session_ok() { return m_sess.connected(); }
    bool sessionFail() { return fail(m_sess.err.empty() ? Str("connection lost") : m_sess.err, m_sess.err.empty() ? true : m_sess.transient); }
    SshConfig m_cfg; Str m_dir; SshSession m_sess; volatile long* m_abort;
};

// ============================== SCP ==================================================================================
// reads the ack byte of the scp protocol: 0 = ok, 1 / 2 = a message line follows
bool ScpAck(SshChannel* ch, Str& msg, bool& fatalErr)
{
    u8 b; msg.clear(); fatalErr = false;
    if (!ch->read_exact(&b, 1)) return false;
    if (b == 0) return true;
    for (int i = 0; i < 1024; ++i) { u8 c; if (!ch->read_exact(&c, 1)) break; if (c == '\n') break; if (c >= 32) msg.push_back((char)c); }
    fatalErr = true;
    if (msg.empty()) msg = "the scp program reported an error";
    return false;
}

class ScpWriter : public Sink {
public:
    ScpWriter(SshSession* s, const Str& dir, const Str& name, volatile long* abortFlag) : m_s(s), m_dir(dir), m_name(name), m_h(INVALID_HANDLE_VALUE), m_size(0), m_done(false), m_abort(abortFlag) {
        wchar_t tdir[MAX_PATH + 1], tmp[MAX_PATH + 1];
        if (!GetTempPathW(MAX_PATH, tdir) || !GetTempFileNameW(tdir, L"v2b", 0, tmp)) { err = "cannot create a temporary file for the upload"; return; }
        m_h = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (m_h == INVALID_HANDLE_VALUE) { DeleteFileW(tmp); err = "cannot open the temporary file for the upload"; }
    }
    ~ScpWriter() { if (!m_done) abort(); closeSpool(); }
    bool ok() const { return m_h != INVALID_HANDLE_VALUE; }
    bool write(const void* p, size_t n) override {
        if (m_h == INVALID_HANDLE_VALUE) return false;
        const u8* d = (const u8*)p;
        while (n) {
            DWORD k = n > (1u << 24) ? (1u << 24) : (DWORD)n, w = 0;
            if (!WriteFile(m_h, d, k, &w, nullptr)) return fail(LastErrorText("cannot write the temporary upload file", ""), false);
            d += w; n -= w; m_size += w;
        }
        m_s->idle_tick();                                        // the connection sits idle while the volume is spooled: keep it alive
        return true;
    }
    bool finish() override;
    void abort() override { m_done = true; closeSpool(); }
    bool finishImpl();
    void removePart();
private:
    void closeSpool() { if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; } }
    SshSession* m_s; Str m_dir, m_name; HANDLE m_h; u64 m_size; bool m_done; volatile long* m_abort;
};

class ScpReader : public Source {
public:
    ScpReader(SshSession* s) : m_s(s), m_ch(nullptr), m_left(0), m_state(0) {}
    ~ScpReader() { close(); }
    bool open(const Str& path);
    i64 read(void* p, size_t n) override;
    void close() override { if (m_ch) { m_ch->close(); delete m_ch; m_ch = nullptr; } m_state = 3; }
private:
    SshSession* m_s; SshChannel* m_ch; u64 m_left; int m_state;      // 0 data, 1 finished, 3 closed
};

class ScpTarget : public SshTargetBase {
public:
    ScpTarget(const SshConfig& cfg, const Str& dir) : SshTargetBase(cfg, dir) {}
    ~ScpTarget() { close(); }
    const char* kind() const override { return "scp"; }
    bool connect() override {
        close();
        if (!open_session()) return false;
        if (!m_dir.empty()) {
            Str e; bool t;
            int st = run("mkdir -p -- " + Quote(DirArg()), nullptr, e, t);
            if (st != 0) { if (st < 0) { Str ee = err; bool tt = transient; close(); return fail(ee, tt); } close(); return fail("cannot create the folder on the server: " + e, false); }
        }
        return true;
    }
    void close() override { m_sess.disconnect(); }
    Sink* open_write(const Str& name) override {
        if (!RemoteNameOk(name)) { err = "bad file name: " + name; transient = false; return nullptr; }
        if (!session_ok()) { err = "not connected"; transient = true; return nullptr; }
        ScpWriter* w = new ScpWriter(&m_sess, DirArg(), name, m_abort);
        if (!w->ok()) { err = w->err; transient = false; delete w; return nullptr; }
        return w;
    }
    Source* open_read(const Str& name) override {
        if (!RemoteNameOk(name)) { err = "bad file name: " + name; transient = false; return nullptr; }
        if (!session_ok()) { err = "not connected"; transient = true; return nullptr; }
        ScpReader* r = new ScpReader(&m_sess);
        if (!r->open(JoinRemote(m_dir, name))) { err = r->err; transient = r->transient; delete r; return nullptr; }
        return r;
    }
    bool list(Vec<Str>& names) override {
        names.clear();
        if (!session_ok()) return fail("not connected", true);
        Bytes out; Str e; bool t;
        int st = run("ls -1Ap -- " + Quote(DirArg()), &out, e, t);
        if (st < 0) return false;
        if (st != 0) return fail("cannot list the folder on the server: " + e, false);
        size_t i = 0;
        while (i < out.size()) {
            size_t j = i; while (j < out.size() && out[j] != '\n') ++j;
            Str line((const char*)out.data() + i, j - i); i = j + 1;
            if (line.empty() || line.ends_with("/") || line.ends_with(".part")) continue;
            names.push_back(line);
        }
        return true;
    }
    bool remove(const Str& name) override {
        if (!RemoteNameOk(name)) return fail("bad file name: " + name);
        if (!session_ok()) return fail("not connected", true);
        Str e; bool t;
        int st = run("rm -f -- " + Quote(JoinRemote(DirArg(), name)), nullptr, e, t);
        if (st < 0) return false;
        if (st != 0) return fail("cannot delete " + name + ": " + e, false);
        return true;
    }
    Str DirArg() const { Str d = m_dir.empty() ? Str(".") : m_dir; if (d.starts_with("-")) d = "./" + d; return d; }
private:
    // runs a command; returns the exit status, or -1 when the session failed (err / transient are set)
    int run(const Str& cmd, Bytes* out, Str& errText, bool& temp) {
        Bytes o, eb; int st = -1;
        if (!m_sess.exec(cmd, o, eb, st, (size_t)64 << 20)) { sessionFail(); temp = transient; return -1; }
        if (out) *out = o;
        errText = OneLine(eb); if (errText.empty()) errText = Str::fmt("exit status %d", st);
        temp = false;
        if (st < 0) { fail("the server did not report the exit status of the command", true); return -1; }
        return st;
    }
};

bool ScpWriter::finish()
{
    if (m_done) return true;
    bool ok = finishImpl();
    if (!ok) removePart();
    return ok;
}

// A failed upload (disk full, rename refused, ...) must not leave a half-written "<name>.part" eating the space the next try needs.
void ScpWriter::removePart()
{
    if (!m_s->connected()) return;
    Bytes o, eb; int st = -1;
    Str e = err; bool t = transient;
    m_s->exec("rm -f -- " + Quote(JoinRemote(m_dir, m_name + ".part")), o, eb, st);
    err = e; transient = t;
}

bool ScpWriter::finishImpl()
{
    if (m_done) return true;
    m_done = true;
    if (m_h == INVALID_HANDLE_VALUE) return false;
    if (!m_s->connected()) { closeSpool(); return fail(m_s->err.empty() ? Str("connection lost") : m_s->err, m_s->err.empty() ? true : m_s->transient); }
    SetFilePointer(m_h, 0, nullptr, FILE_BEGIN);
    SshChannel* ch = m_s->open_session();
    if (!ch) { closeSpool(); return fail(m_s->err, m_s->transient); }
    struct Guard { SshChannel* c; ~Guard() { if (c) { c->close(); delete c; } } } guard{ ch };
    Str part = m_name + ".part", msg; bool fatalErr = false;
    auto chfail = [&]() { closeSpool(); return fail(ch->err.empty() ? Str("connection lost") : ch->err, ch->transient); };
    if (!ch->exec("scp -t -- " + Quote(m_dir))) { closeSpool(); Bytes eb = ch->stderr_data(); return fail("cannot start scp on the server: " + (ch->err.empty() ? OneLine(eb) : ch->err), ch->err.empty() ? false : ch->transient); }
    if (!ScpAck(ch, msg, fatalErr)) { if (!fatalErr) { Bytes eb = ch->stderr_data(); if (!eb.empty() && !ch->transient) { closeSpool(); return fail("scp on the server failed: " + OneLine(eb), false); } return chfail(); } closeSpool(); return fail("scp: " + msg, false); }
    Str cline = Str::fmt("C0600 %llu ", (unsigned long long)m_size) + part + "\n";
    if (!ch->write(cline.data(), cline.size())) return chfail();
    if (!ScpAck(ch, msg, fatalErr)) { if (!fatalErr) return chfail(); closeSpool(); return fail("scp: " + msg, false); }
    u8 buf[32768]; u64 left = m_size;
    while (left) {
        DWORD want = left > sizeof(buf) ? (DWORD)sizeof(buf) : (DWORD)left, got = 0;
        if (m_abort && *m_abort) { closeSpool(); return fail("aborted", false); }
        if (!ReadFile(m_h, buf, want, &got, nullptr) || got == 0) { closeSpool(); return fail("cannot read the temporary upload file", false); }
        if (!ch->write(buf, got)) return chfail();
        left -= got;
    }
    u8 zero = 0; if (!ch->write(&zero, 1)) return chfail();
    if (!ScpAck(ch, msg, fatalErr)) { if (!fatalErr) return chfail(); closeSpool(); return fail("scp: " + msg, false); }
    closeSpool();
    if (!ch->send_eof()) return fail(ch->err, ch->transient);
    if (!ch->wait_close(10000)) return fail(ch->err, ch->transient);
    if (ch->exit_status() < 0) return fail("the server did not confirm that scp finished", true);
    if (ch->exit_status() != 0) return fail("scp on the server ended with status " + Str::fmt("%d", ch->exit_status()) + ": " + OneLine(ch->stderr_data()), false);
    // the finished file takes its name; the scp channel goes first (a server with MaxSessions 1 refuses a second one)
    ch->close(); delete ch; guard.c = nullptr;
    Bytes o, eb; int st = -1;
    if (!m_s->exec("mv -f -- " + Quote(JoinRemote(m_dir, part)) + " " + Quote(JoinRemote(m_dir, m_name)), o, eb, st)) return fail(m_s->err, m_s->transient);
    if (st != 0) return fail("cannot rename the uploaded file: " + OneLine(eb), st < 0);
    return true;
}

bool ScpReader::open(const Str& path)
{
    m_ch = m_s->open_session();
    if (!m_ch) { err = m_s->err; transient = m_s->transient; return false; }
    auto chfail = [&]() { err = m_ch->err.empty() ? Str("connection lost") : m_ch->err; transient = m_ch->err.empty() ? true : m_ch->transient; delete m_ch; m_ch = nullptr; return false; };
    Str p = path; if (p.starts_with("-")) p = "./" + p;
    if (!m_ch->exec("scp -f -- " + Quote(p))) return chfail();
    u8 zero = 0; if (!m_ch->write(&zero, 1)) return chfail();
    // the answer: "C<mode> <size> <name>\n", or an error (1 / 2 + message)
    u8 t; if (!m_ch->read_exact(&t, 1)) { Bytes eb = m_ch->stderr_data(); if (!eb.empty()) { err = "scp on the server failed: " + OneLine(eb); transient = false; delete m_ch; m_ch = nullptr; return false; } return chfail(); }
    Str line;
    for (int i = 0; i < 2048; ++i) { u8 c; if (!m_ch->read_exact(&c, 1)) return chfail(); if (c == '\n') break; line.push_back((char)c); }
    if (t == 1 || t == 2) { for (size_t i = 0; i < line.size(); ++i) if ((u8)line[i] < 32) line.mut()[i] = ' '; err = "scp: " + line.trim(); transient = false; delete m_ch; m_ch = nullptr; return false; }
    if (t != 'C') { err = "unexpected answer from scp on the server"; transient = false; delete m_ch; m_ch = nullptr; return false; }
    // mode, size, name
    int s1 = line.find(' '); int s2 = s1 < 0 ? -1 : line.find(' ', (size_t)s1 + 1);
    if (s1 < 0 || s2 < 0) { err = "malformed scp answer"; transient = false; delete m_ch; m_ch = nullptr; return false; }
    Str sz = line.substr((size_t)s1 + 1, (size_t)(s2 - s1 - 1));
    u64 size = 0; if (sz.empty() || sz.size() > 18) { err = "malformed scp size"; transient = false; delete m_ch; m_ch = nullptr; return false; }
    for (size_t i = 0; i < sz.size(); ++i) { if (sz[i] < '0' || sz[i] > '9') { err = "malformed scp size"; transient = false; delete m_ch; m_ch = nullptr; return false; } size = size * 10 + (u64)(sz[i] - '0'); }
    m_left = size;
    if (!m_ch->write(&zero, 1)) return chfail();
    return true;
}

i64 ScpReader::read(void* p, size_t n)
{
    if (m_state == 3) return fail("the file is closed", false);
    if (m_state == 1 || n == 0) return 0;
    if (m_left == 0) {
        // the trailing ack from the server, ours back
        Str msg; bool fatalErr;
        if (!ScpAck(m_ch, msg, fatalErr)) { m_state = 3; return fatalErr ? fail("scp: " + msg, false) : fail(m_ch->err.empty() ? Str("connection lost") : m_ch->err, m_ch->err.empty() ? true : m_ch->transient); }
        u8 zero = 0; m_ch->write(&zero, 1);
        m_state = 1; return 0;
    }
    size_t want = n > m_left ? (size_t)m_left : n;
    i64 k = m_ch->read(p, want);
    if (k < 0) { m_state = 3; return fail(m_ch->err.empty() ? Str("connection lost") : m_ch->err, m_ch->err.empty() ? true : m_ch->transient); }
    if (k == 0) { m_state = 3; return fail("the connection ended before the whole file was received", true); }
    m_left -= (u64)k;
    return k;
}

} // namespace

void SshTargetSetAbortFlag(Target* t, volatile long* flag)
{
    if (!t) return;
    Str k = t->kind();
    if (k == "scp") static_cast<SshTargetBase*>(t)->set_abort(flag);
}

Target* NewSshTarget(const Json& cfg, Str& err)
{
    Str type = cfg.get("type").str();
    if (type != "scp") { err = "ssh target: type must be scp"; return nullptr; }
    SshConfig c;
    c.host = cfg.get("host").str().trim(); c.port = (int)cfg.get("port").integer(22); c.user = cfg.get("user").str(); c.password = cfg.get("password").str();
    c.keyFile = cfg.get("key_file").str(); c.keyText = cfg.get("key_text").str(); c.keyPassphrase = cfg.get("key_passphrase").str(); c.hostKey = cfg.get("host_key").str();
    c.insecureAcceptHostKey = cfg.get("insecure_accept_host_key").boolean(false);
    const Json& kh = cfg.get("known_host_fingerprints");
    if (kh.is_array()) for (size_t i = 0; i < kh.size(); ++i) c.knownFingerprints.push_back(kh.at(i).str());
    else if (kh.is_string()) c.knownFingerprints.push_back(kh.str());
    if (cfg.has("connect_timeout_ms")) c.connectTimeoutMs = (int)cfg.get("connect_timeout_ms").integer(15000);
    if (cfg.has("io_timeout_ms")) c.ioTimeoutMs = (int)cfg.get("io_timeout_ms").integer(60000);
    if (cfg.has("keepalive_ms")) c.keepaliveMs = (int)cfg.get("keepalive_ms").integer(15000);
    if (cfg.has("rekey_bytes")) c.rekeyBytes = (u64)cfg.get("rekey_bytes").integer(0);
    c.kexList = cfg.get("kex").str(); c.hostKeyList = cfg.get("host_key_algorithms").str(); c.cipherList = cfg.get("ciphers").str(); c.macList = cfg.get("macs").str();
    if (c.host.empty()) { err = "ssh target: the host is not set"; return nullptr; }
    if (c.port <= 0 || c.port > 65535) { err = "ssh target: bad port"; return nullptr; }
    if (c.user.empty()) { err = "ssh target: the user name is not set"; return nullptr; }
    if (c.password.empty() && c.keyFile.empty() && c.keyText.empty()) { err = "ssh target: neither a password nor a private key is set"; return nullptr; }
    Str dir = cfg.get("directory").str().replace_all("\\", "/");
    while (dir.size() > 1 && dir.ends_with("/")) dir.resize(dir.size() - 1);
    return new ScpTarget(c, dir);
}

} // namespace vm2b
