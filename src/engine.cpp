#include "engine.h"
#include "winutil.h"

namespace vm2b {

// ---- manifest --------------------------------------------------------------------------------------------------
Json Manifest::toJson() const
{
    Json j = Json::object();
    j["version"] = version; j["vm"] = vm; j["archive"] = archive; j["source"] = source; j["created"] = created; j["stamp"] = stamp;
    j["compression"] = codec; j["level"] = level; j["encrypted"] = encrypted;
    j["size"] = size; j["raw_size"] = rawSize; j["sha256"] = sha256; j["segment_bytes"] = segmentBytes; j["kdf_iterations"] = kdfIterations;
    Json a = Json::array();
    for (const Segment& s : segments) { Json o = Json::object(); o["size"] = s.size; o["raw"] = s.raw; o["sha256"] = s.sha256; a.push(o); }
    j["segments"] = a;
    return j;
}

bool Manifest::fromJson(const Json& j, Manifest& m)
{
    if (!j.is_object() || !j.has("archive") || !j.has("vm") || !j.has("segments")) return false;
    m.version = (int)j.get("version").integer(3); m.vm = j.get("vm").str(); m.archive = j.get("archive").str();
    m.source = j.get("source").str(); m.created = j.get("created").num(); m.stamp = j.get("stamp").str();
    m.codec = j.get("compression").str();                        // anything but "xz" (or nothing) is kept, listed, and refused when read (see openVolume)
    m.level = (int)j.get("level").integer(0); m.encrypted = j.get("encrypted").boolean();
    m.size = (u64)j.get("size").integer(); m.rawSize = (u64)j.get("raw_size").integer(); m.sha256 = j.get("sha256").str();
    m.segmentBytes = (u64)j.get("segment_bytes").integer(); m.kdfIterations = (u32)j.get("kdf_iterations").integer();
    m.segments.clear();
    const Json& a = j.get("segments");
    for (size_t i = 0; i < a.size(); ++i) {
        Segment s; s.size = (u64)a.at(i).get("size").integer(); s.raw = (u64)a.at(i).get("raw").integer(); s.sha256 = a.at(i).get("sha256").str();
        if (s.sha256.size() != 64) return false;
        m.segments.push_back(s);
    }
    return !m.archive.empty() && !m.vm.empty() && !m.segments.empty() && m.archive.find('/') < 0 && m.archive.find('\\') < 0;
}

namespace {

// ---- helpers ---------------------------------------------------------------------------------------------------
Str RootHash(const Vec<Segment>& segs)
{
    Bytes all;
    for (const Segment& s : segs) { Bytes h; HexDecode(s.sha256.data(), s.sha256.size(), h); for (size_t i = 0; i < h.size(); ++i) all.push_back(h[i]); }
    u8 d[32]; Sha256(all.data(), all.size(), d);
    return HexEncode(d, 32);
}

bool IsAborted(void* p) { Progress* pr = (Progress*)p; return pr && pr->aborted(); }

// Sleeps `ms`, waking up to look at the abort flag. Returns false when the job was aborted.
bool Pause(Progress* pr, int ms)
{
    for (int left = ms; left > 0; left -= 100) {
        if (pr && pr->aborted()) return false;
        Sleep(left > 100 ? 100 : (DWORD)left);
    }
    return !(pr && pr->aborted());
}

int DelayFor(const RetryPolicy& rp, int attempt)          // attempt is 1-based
{
    long long d = rp.baseDelayMs;
    for (int i = 1; i < attempt && d < rp.maxDelayMs; ++i) d *= 2;
    return (int)(d > rp.maxDelayMs ? rp.maxDelayMs : d);
}

bool IsVolumeName(const Str& n, const Str& archive)          // "<archive>.NNNNN"
{
    if (n.size() != archive.size() + 6 || !n.starts_with(archive.c_str()) || n[archive.size()] != '.') return false;
    for (size_t i = archive.size() + 1; i < n.size(); ++i) if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

// Connects (or reconnects) with the retry policy; false = gave up / aborted (err says why).
bool ConnectRetry(Target& t, const RetryPolicy& rp, Progress* pr, const char* what, bool first, Str& err)
{
    for (int attempt = 0;; ++attempt) {
        bool ok = (first && attempt == 0) ? t.connect() : t.reconnect();
        if (ok) return true;
        if (!t.transient || attempt >= rp.attempts) { err = t.err; return false; }
        int d = DelayFor(rp, attempt + 1);
        if (pr) pr->onRetry(what, attempt + 1, rp.attempts, d, t.err);
        if (!Pause(pr, d)) { err = kAbortedMessage; return false; }
    }
}

// Runs fn() (an operation on the target that returns bool); a transient failure is waited out, the connection is made again and fn()
// is run once more. On a failure that is final, err describes it.
template <class F> bool WithRetry(Target& t, const RetryPolicy& rp, Progress* pr, const char* what, Str& err, F fn)
{
    for (int attempt = 0;; ++attempt) {
        if (fn()) return true;
        if (!t.transient || attempt >= rp.attempts) return false;
        int d = DelayFor(rp, attempt + 1);
        if (pr) pr->onRetry(what, attempt + 1, rp.attempts, d, t.err);
        if (!Pause(pr, d)) { err = kAbortedMessage; t.transient = false; t.err = kAbortedMessage; return false; }
        if (!ConnectRetry(t, rp, pr, "reconnect", false, err)) { t.transient = false; t.err = err; return false; }
    }
}

// Removes the journal, the volumes and the manifest of a (partial) backup.
bool RemoveAllOf(Target& t, const Str& archive, Str& err)
{
    Vec<Str> names;
    if (!t.list(names)) { err = t.err; return false; }
    bool ok = true; int vols = 0;
    for (const Str& n : names)
        if (n == archive + ".resume" || n == archive + ".json" || IsVolumeName(n, archive)) { if (IsVolumeName(n, archive)) ++vols; if (!t.remove(n)) { err = t.err; ok = false; } }
    // The volume that was being written when the program or the machine died is still a "<volume>.part" (list() hides those, an abort removes them itself):
    // volumes are written one after another, so it can only be the next one.
    t.remove(archive + Str::fmt(".%05d.part", vols + 1));
    return ok;
}

// The bandwidth limit sits at the border with the target: only bytes that really go over the wire are counted.
struct ThrottleSink : Sink {
    Sink* out; RateLimiter* rl; Progress* pr;
    ThrottleSink(Sink* o, RateLimiter* l, Progress* p) : out(o), rl(l), pr(p) {}
    bool write(const void* p, size_t n) override {
        const u8* d = (const u8*)p;
        while (n) {                                                        // in pieces, so a big write does not overshoot the limit by seconds
            size_t k = n > (256u << 10) ? (256u << 10) : n;
            if (rl && !rl->acquire(k, IsAborted, pr)) return fail(kAbortedMessage);
            if (!out->write(d, k)) return failFrom(out);
            d += k; n -= k;
        }
        return true;
    }
    bool finish() override { if (!out->finish()) return failFrom(out); return true; }
    void abort() override { out->abort(); }
};

struct ThrottleSource : Source {
    Source* in; RateLimiter* rl; Progress* pr;
    ThrottleSource(Source* i, RateLimiter* l, Progress* p) : in(i), rl(l), pr(p) {}
    i64 read(void* p, size_t n) override {
        if (n > (256u << 10)) n = 256u << 10;
        i64 k = in->read(p, n);
        if (k < 0) return failFrom(in);
        if (k > 0 && rl && !rl->acquire((size_t)k, IsAborted, pr)) return fail(kAbortedMessage);
        return k;
    }
    void close() override { in->close(); }
};

// ---- the writing side: cuts the tar stream into volumes ---------------------------------------------------------
struct WriteCtx {
    Target* target; const BackupOptions* opt; Manifest* mf; Progress* pr; Vec<Segment>* done; u8 salt[16];
    Json journalBase;                                    // the journal without the segment list
    bool writeJournal(Str& err, bool& tmp) {
        Json j = journalBase;
        Json a = Json::array();
        for (const Segment& s : *done) { Json o = Json::object(); o["size"] = s.size; o["raw"] = s.raw; o["sha256"] = s.sha256; a.push(o); }
        j["segments"] = a;
        Str js = j.dump();
        if (!target->put_bytes(mf->archive + ".resume", js.data(), js.size())) { err = target->err; tmp = target->transient; return false; }
        return true;
    }
};

class SegmentWriter : public Sink {
public:
    explicit SegmentWriter(WriteCtx& c) : m_c(c), m_file(nullptr), m_thr(nullptr), m_hash(nullptr), m_enc(nullptr), m_comp(nullptr), m_raw(0), m_open(false), m_done(false) {}
    ~SegmentWriter() { dropChain(); }

    bool write(const void* p, size_t n) override {
        const u8* d = (const u8*)p;
        while (n) {
            // a volume is closed only when more data arrives for the next one: until then it might be the last
            if (m_open && m_c.opt->segmentBytes && m_raw == m_c.opt->segmentBytes) { if (!closeSegment(false)) return false; }
            if (!m_open && !openSegment()) return false;
            size_t k = n;
            if (m_c.opt->segmentBytes && (u64)k > m_c.opt->segmentBytes - m_raw) k = (size_t)(m_c.opt->segmentBytes - m_raw);
            if (!m_comp->write(d, k)) { Str e = m_comp->err; bool t = m_comp->transient; dropChain(); return fail(e, t); }
            m_raw += k; d += k; n -= k;
        }
        return true;
    }
    bool finish() override {
        if (m_done) return true;
        if (!m_open && !openSegment()) return false;            // an archive always has at least one volume
        if (!closeSegment(true)) return false;
        m_done = true;
        return true;
    }
    void abort() override { dropChain(); }

private:
    bool openSegment() {
        const BackupOptions& o = *m_c.opt;
        u32 index = (u32)m_c.done->size();
        m_file = m_c.target->open_write(m_c.mf->volumeName(index));
        if (!m_file) return fail(m_c.target->err, m_c.target->transient);
        m_thr = new ThrottleSink(m_file, o.limiter, m_c.pr);
        m_hash = new HashSink(m_thr);
        Sink* top = m_hash;
        if (!o.password.empty()) { m_enc = new EncryptSink(top, o.password, o.kdfIterations, m_c.salt, index); top = m_enc; }
        m_comp = CodecMakeSink(o.codec, top);
        if (!m_comp) { dropChain(); return fail("xz compression is not available"); }
        m_raw = 0; m_open = true;
        return true;
    }
    bool closeSegment(bool last) {
        if (m_enc) m_enc->setLastVolume(last);
        if (!m_comp->finish()) { Str e = m_comp->err; bool t = m_comp->transient; dropChain(); return fail(e, t); }
        Segment s; s.size = m_hash->size(); s.raw = m_raw; s.sha256 = m_hash->hex();
        delete m_comp; m_comp = nullptr; delete m_enc; m_enc = nullptr; delete m_hash; m_hash = nullptr; delete m_thr; m_thr = nullptr; delete m_file; m_file = nullptr;
        m_open = false;
        m_c.done->push_back(s);
        if (m_c.pr) m_c.pr->onVolume((int)m_c.done->size(), -1);
        if (!last && m_c.opt->segmentBytes) { Str e; bool t = false; if (!m_c.writeJournal(e, t)) return fail(e, t); }
        return true;
    }
    void dropChain() {
        if (m_comp) m_comp->abort();                           // goes down the chain and removes the .part file
        else if (m_file) m_file->abort();
        delete m_comp; m_comp = nullptr; delete m_enc; m_enc = nullptr; delete m_hash; m_hash = nullptr; delete m_thr; m_thr = nullptr; delete m_file; m_file = nullptr;
        m_open = false;
    }
    WriteCtx& m_c; Sink* m_file; ThrottleSink* m_thr; HashSink* m_hash; EncryptSink* m_enc; Sink* m_comp;
    u64 m_raw; bool m_open, m_done;
};

// Polls the abort flag, so a job stops promptly even in the middle of a big file.
struct AbortSink : Sink {
    Sink* out; Progress* pr;
    AbortSink(Sink* o, Progress* p) : out(o), pr(p) {}
    bool write(const void* p, size_t n) override {
        if (pr && pr->aborted()) return fail(kAbortedMessage);
        if (!out->write(p, n)) return failFrom(out);
        return true;
    }
    bool finish() override { if (!out->finish()) return failFrom(out); return true; }
    void abort() override { out->abort(); }
};

Str OptionsKey(const BackupOptions& o)
{
    return Str::fmt("%s|%d|%d|%llu|%u", CodecName(), CodecEffectiveLevel(o.codec), o.password.empty() ? 0 : 1, (unsigned long long)o.segmentBytes, o.kdfIterations);
}

Str PasswordCheck(const Str& pw, const u8 salt[16], u32 iter)
{
    if (pw.empty()) return "";
    u8 key[32], mac[32];
    if (!DeriveBackupKey(pw, salt, iter, key)) return "?";
    HmacSha256(key, 32, "vm2b-resume-check", 17, mac);
    memset(key, 0, 32);
    return HexEncode(mac, 8);
}

struct Journal {
    bool ok; Str vm, archive, stamp, options, pwcheck, fingerprint, token; double created; u8 salt[16]; Vec<Segment> segs;
    Journal() : ok(false), created(0) { memset(salt, 0, 16); }
};

bool ParseJournal(const Bytes& b, Journal& j)
{
    Json js; Str e;
    if (!Json::parse((const char*)b.data(), b.size(), js, e) || !js.is_object()) return false;
    if (js.has("options") && !js.get("options").str().starts_with("xz|")) return false;        // a journal of another (removed) codec: not resumable
    j.vm = js.get("vm").str(); j.archive = js.get("archive").str(); j.stamp = js.get("stamp").str(); j.options = js.get("options").str();
    j.pwcheck = js.get("pwcheck").str(); j.fingerprint = js.get("fingerprint").str(); j.token = js.get("token").str(); j.created = js.get("created").num();
    Str hs = js.get("salt").str(); Bytes s;
    if (!HexDecode(hs.c_str(), hs.size(), s) || s.size() != 16) return false;
    memcpy(j.salt, s.data(), 16);
    const Json& a = js.get("segments");
    for (size_t i = 0; i < a.size(); ++i) { Segment g; g.size = (u64)a.at(i).get("size").integer(); g.raw = (u64)a.at(i).get("raw").integer(); g.sha256 = a.at(i).get("sha256").str(); if (g.sha256.size() != 64) return false; j.segs.push_back(g); }
    j.ok = !j.archive.empty() && !j.vm.empty() && j.archive.find('/') < 0 && j.archive.find('\\') < 0;
    return j.ok;
}

bool ReadJournal(Target& t, const Str& archive, Journal& j)
{
    Bytes b;
    if (!t.get_bytes(archive + ".resume", b, 16 << 20)) return false;
    return ParseJournal(b, j) && j.archive == archive;
}

} // namespace

bool ListResumable(Target& target, const Str* vm, Vec<ResumeInfo>& out, Str& err)
{
    out.clear();
    Vec<Str> names;
    if (!target.list(names)) { err = target.err; return false; }
    for (const Str& n : names) {
        if (!n.ends_with(".resume")) continue;
        Str archive = n.substr(0, n.size() - 7);
        Journal j;
        if (!ReadJournal(target, archive, j)) continue;
        if (vm && j.vm != *vm) continue;
        bool finished = false;
        for (const Str& m : names) if (m == archive + ".json") finished = true;
        if (finished) continue;
        ResumeInfo r; r.vm = j.vm; r.archive = archive; r.stamp = j.stamp; r.created = j.created; r.volumesDone = (int)j.segs.size();
        for (const Segment& s : j.segs) { r.storedBytes += s.size; r.rawBytes += s.raw; }
        out.push_back(r);
    }
    for (size_t i = 1; i < out.size(); ++i) { ResumeInfo k = out[i]; size_t j = i; while (j > 0 && k.created < out[j - 1].created) { out[j] = out[j - 1]; --j; } out[j] = k; }
    return true;
}

bool DiscardResumable(Target& target, const Str& archive, Str& err)
{
    if (!target.connect()) { err = target.err; return false; }
    return RemoveAllOf(target, archive, err);
}

bool BackupVm(VmSource& src, const Str& vm, Target& target, const BackupOptions& o, Progress* pr, Manifest& out, Str& warning, Str& err)
{
    warning.clear();
    const RetryPolicy rp = o.retry;
    if (o.segmentBytes && (o.segmentBytes & 511)) { err = "the volume size must be a multiple of 512 bytes"; return false; }
    if (!ConnectRetry(target, rp, pr, "connect", true, err)) return false;
    const bool enc = !o.password.empty();

    // 1. is there an interrupted backup of this VM that can be continued?
    Journal jr; bool resuming = false;
    if (o.resume && o.segmentBytes) {
        Vec<ResumeInfo> rs; Str e2, e4;
        if (WithRetry(target, rp, pr, "resume", e4, [&]() { return ListResumable(target, &vm, rs, e2); })) {
            for (size_t i = rs.size(); i-- > 0;) {                            // the newest first
                Journal j;
                if (!WithRetry(target, rp, pr, "resume", e4, [&]() { return ReadJournal(target, rs[i].archive, j) || !target.transient; })) continue;
                if (!j.ok) continue;
                bool allThere = true;                                         // a volume the journal counts as done but the folder lost: start over
                {
                    Vec<Str> have; Str le;
                    if (WithRetry(target, rp, pr, "resume", le, [&]() { return target.list(have); })) {
                        for (size_t s = 0; s < j.segs.size() && allThere; ++s) {
                            Str v = rs[i].archive + Str::fmt(".%05d", (int)s + 1); bool f = false;
                            for (const Str& n : have) if (n == v) { f = true; break; }
                            if (!f) allThere = false;
                        }
                    }
                }
                bool same = allThere && j.options == OptionsKey(o) && j.pwcheck == PasswordCheck(o.password, j.salt, o.kdfIterations);
                if (same && !resuming) { jr = j; resuming = true; }
                else { Str e3; RemoveAllOf(target, rs[i].archive, e3); }       // another setting or password: this partial backup is of no use
            }
        }
    }

    // 2. the source
    Str fp, token, w2;
    if (!src.begin(vm, o.allowLive, resuming ? jr.token : Str(), fp, token, w2)) { err = src.err; return false; }
    warning = w2;
    if (resuming && jr.fingerprint != fp) {                                   // the VM changed since: the partial copy does not belong to it
        Str e3; RemoveAllOf(target, jr.archive, e3);
        src.end(false);
        resuming = false; jr = Journal();
        if (!src.begin(vm, o.allowLive, Str(), fp, token, w2)) { err = src.err; return false; }
        warning = w2;
    }

    // 3. names, salt, the state that survives passes
    double now = UnixNow();
    Manifest mf;
    Vec<Segment> done;
    u8 salt[16];
    if (resuming) {
        mf.archive = jr.archive; mf.stamp = jr.stamp; mf.created = jr.created; done = jr.segs; memcpy(salt, jr.salt, 16); if (!jr.token.empty()) token = jr.token;
        if (pr) { u64 raw = 0; for (const Segment& s : done) raw += s.raw; pr->onResume((int)done.size(), raw); }
    } else {
        mf.created = now; mf.stamp = LocalStamp(now);
        Vec<Str> names; Str le;
        if (!WithRetry(target, rp, pr, "list", le, [&]() { return target.list(names); })) { err = le.empty() ? target.err : le; src.end(false); return false; }
        for (int n = 1;; ++n) {                                                // two backups in one second must not collide
            Str base = SafeName(vm) + "_" + mf.stamp + (n > 1 ? Str::fmt("-%d", n) : Str()) + ".tar" + CodecExt() + (enc ? ".enc" : "");
            bool used = false;
            for (const Str& x : names) if (x == base + ".json" || x == base + ".resume" || x.starts_with((base + ".").c_str())) { used = true; break; }
            if (!used) { mf.archive = base; break; }
        }
        if (!RandomBytes(salt, 16)) { err = "no random numbers for the encryption"; src.end(false); return false; }
    }
    mf.vm = vm; mf.source = src.kind(); mf.codec = CodecName(); mf.level = CodecEffectiveLevel(o.codec); mf.encrypted = enc;
    mf.segmentBytes = o.segmentBytes; mf.kdfIterations = enc ? o.kdfIterations : 0;

    WriteCtx wc; wc.target = &target; wc.opt = &o; wc.mf = &mf; wc.pr = pr; wc.done = &done; memcpy(wc.salt, salt, 16);
    Json jb = Json::object();
    jb["version"] = 1; jb["vm"] = vm; jb["archive"] = mf.archive; jb["stamp"] = mf.stamp; jb["created"] = mf.created; jb["options"] = OptionsKey(o);
    jb["pwcheck"] = PasswordCheck(o.password, salt, o.kdfIterations); jb["fingerprint"] = fp; jb["token"] = token; jb["salt"] = HexEncode(salt, 16);
    wc.journalBase = jb;

    // 4. passes: each one writes the volumes that are still missing; a transient failure gives another pass after reconnecting
    bool ok = false, keepOnFailure = false; Str failure;
    for (int attempt = 0;; ++attempt) {
        u64 skipRaw = 0; for (const Segment& s : done) skipRaw += s.raw;
        SegmentWriter sw(wc);
        AbortSink ab(&sw, pr);
        TarWriter tar(&ab);
        tar.set_skip(skipRaw);
        Str passWarn;
        bool packed = src.pack(tar, pr, passWarn);
        bool good = packed && tar.finish() && ab.finish();
        if (good) { ok = true; if (!passWarn.empty()) { if (!warning.empty()) warning += "; "; warning += passWarn; } break; }
        // what failed? The sink side (target, crypto, codec) reports through `ab`; otherwise it was the source or the tar writer.
        Str cause; bool tmp = false, fromSource = false;
        if (!ab.err.empty()) { cause = ab.err; tmp = ab.transient; }
        else if (!packed) { cause = src.err.empty() ? tar.error() : src.err; tmp = src.transient; fromSource = true; }
        else cause = tar.error();
        sw.abort();
        failure = cause;
        if (cause == kAbortedMessage) { keepOnFailure = pr && pr->keepOnAbort(); break; }
        keepOnFailure = !fromSource;                                          // a target problem keeps the volumes for a later run
        if (!tmp || attempt >= rp.attempts) break;
        int d = DelayFor(rp, attempt + 1);
        if (pr) pr->onRetry("backup", attempt + 1, rp.attempts, d, cause);
        if (!Pause(pr, d)) { failure = kAbortedMessage; keepOnFailure = false; break; }
        Str ce;
        if (!ConnectRetry(target, rp, pr, "reconnect", false, ce)) { failure = ce; if (ce == kAbortedMessage) keepOnFailure = false; break; }
    }
    if (!ok) {
        err = failure;
        bool keep = keepOnFailure && o.resume && o.segmentBytes != 0 && !done.empty();   // resume off: nobody will continue the partial copy, so it must not stay on the target
        src.end(keep);
        if (keep) { Str e3; bool t3 = false; if (!wc.writeJournal(e3, t3) && target.reconnect()) wc.writeJournal(e3, t3); }       // without a journal the volumes would be orphans nobody can continue or discard
        else { Str e3; if (target.reconnect()) RemoveAllOf(target, mf.archive, e3); }
        return false;
    }

    // 5. the manifest: its presence means "complete"
    mf.segments = done;
    for (const Segment& s : done) { mf.size += s.size; mf.rawSize += s.raw; }
    mf.sha256 = RootHash(done);
    Str js = mf.toJson().dump(true);
    for (int attempt = 0;; ++attempt) {
        if (target.put_bytes(mf.archive + ".json", js.data(), js.size())) break;
        if (!target.transient || attempt >= rp.attempts) { err = target.err; src.end(true); return false; }
        int d = DelayFor(rp, attempt + 1);
        if (pr) pr->onRetry("manifest", attempt + 1, rp.attempts, d, target.err);
        Str e3;
        if (!Pause(pr, d) || !ConnectRetry(target, rp, pr, "reconnect", false, e3)) { err = e3.empty() ? Str(kAbortedMessage) : e3; src.end(true); return false; }
    }
    if (!target.remove(mf.archive + ".resume")) { target.reconnect(); target.remove(mf.archive + ".resume"); }       // a journal that stays behind is garbage for ever
    src.end(false);
    out = mf;
    if (o.verify) {
        bool lostLink = false;
        ReadOptions ro; ro.retry = rp; ro.resume = false; ro.limiter = o.limiter; ro.lostConnection = &lostLink;
        Str ve;
        if (!VerifyBackup(target, mf.archive, o.password, ro, pr, nullptr, ve)) {
            err = ve;
            if (lostLink) err = "the backup was written, but its verification was interrupted (" + ve + "); verify it again later";   // the connection, not the data, failed: the backup stays
            else if (ve != kAbortedMessage) { Str e3; DeleteBackup(target, mf.archive, e3); }
            return false;
        }
    }
    if (o.keep > 0) { Vec<Str> removed; Str pe; PruneBackups(target, vm, o.keep, removed, pe, mf.archive); }
    return true;
}

bool ReadManifest(Target& target, const Str& archive, Manifest& m, Str& err)
{
    Bytes b;
    if (!target.get_bytes(archive + ".json", b, 64 << 20)) { err = "no manifest for " + archive + ": " + target.err; return false; }
    Json j; Str e;
    if (!Json::parse((const char*)b.data(), b.size(), j, e) || !Manifest::fromJson(j, m)) { err = "bad manifest of " + archive; return false; }
    if (m.archive != archive) { err = "the manifest does not belong to " + archive; return false; }
    return true;
}

bool ListBackups(Target& target, const Str* vm, Vec<Manifest>& out, Str& err)
{
    out.clear();
    if (!target.connect()) { err = target.err; return false; }
    Vec<Str> names;
    if (!target.list(names)) { err = target.err; return false; }
    for (const Str& n : names) {
        if (!n.ends_with(".json")) continue;
        Manifest m; Str e;
        if (!ReadManifest(target, n.substr(0, n.size() - 5), m, e)) continue;           // foreign / damaged manifests are ignored
        if (vm && m.vm != *vm) continue;
        out.push_back(m);
    }
    for (size_t i = 1; i < out.size(); ++i) { Manifest k = out[i]; size_t j = i; while (j > 0 && k.created < out[j - 1].created) { out[j] = out[j - 1]; --j; } out[j] = k; }
    return true;
}

namespace {

// A manifest is not trusted: what goes into a message is cut short and printable only.
Str SafeText(const Str& s)
{
    Str o; for (size_t i = 0; i < s.size() && i < 16; ++i) { unsigned char ch = (unsigned char)s[i]; o += (ch >= 32 && ch < 127) ? (char)ch : '?'; }
    return o.empty() ? Str("none") : o;
}

// ---- the reading side: the volumes as one stream -----------------------------------------------------------------
struct AbortSource : Source {
    Source* in; Progress* pr;
    AbortSource(Source* i, Progress* p) : in(i), pr(p) {}
    i64 read(void* p, size_t n) override {
        if (pr && pr->aborted()) return fail(kAbortedMessage);
        i64 k = in->read(p, n);
        if (k < 0) return failFrom(in);
        if (k > 0 && pr) pr->onArchiveBytes((u64)k);
        return k;
    }
};

// One volume: file -> throttle -> hash -> abort check -> (decrypt) -> (decompress). The owner deletes the pieces in this order.
struct VolChain {
    Source* file; ThrottleSource* thr; HashSource* hash; Source* abortSrc; DecryptSource* dec; Source* comp; Source* top;
    VolChain() : file(nullptr), thr(nullptr), hash(nullptr), abortSrc(nullptr), dec(nullptr), comp(nullptr), top(nullptr) {}
    ~VolChain() { delete comp; delete dec; delete abortSrc; delete hash; delete thr; if (file) { file->close(); delete file; } }
};

// The stream of all volumes of a backup. Checks every volume's size and SHA-256 when its data has been read, and survives
// a lost connection: reconnects, opens the broken volume again and skips the bytes that were already handed out.
class SegmentedReader : public Source {
public:
    SegmentedReader(Target& t, const Manifest& m, const Str& pw, bool deep, const ReadOptions& ro, Progress* pr)
        : m_t(t), m_m(m), m_pw(pw), m_deep(deep), m_rp(ro.retry), m_rl(ro.limiter), m_pr(pr), m_cur(0), m_delivered(0), m_skipLeft(0), m_chain(nullptr), m_series(0), m_wrongPw(false), m_lost(false) {}
    ~SegmentedReader() { delete m_chain; }
    void startAt(size_t seg, u64 skipInSeg) { delete m_chain; m_chain = nullptr; m_cur = seg; m_skipLeft = skipInSeg; m_delivered = 0; m_series = 0; }
    size_t segment() const { return m_cur; }
    u64 deliveredInSegment() const { return m_delivered + m_skipLeft; }       // where the next byte is, within the volume
    bool wrongPassword() const { return m_wrongPw; }
    bool lostConnection() const { return m_lost; }       // it gave up because the target could not be reached / kept (not a data error)

    i64 read(void* p, size_t n) override {
        for (;;) {
            if (!m_chain) {
                if (m_cur >= m_m.segments.size()) return 0;
                if (!openVolume()) return -1;
            }
            if (m_skipLeft) {                                                    // after a reconnect: the bytes the consumer already has
                u8 junk[65536];
                i64 k = m_chain->top->read(junk, m_skipLeft > sizeof(junk) ? sizeof(junk) : (size_t)m_skipLeft);
                if (k > 0) { m_skipLeft -= (u64)k; m_delivered += (u64)k; continue; }
                if (k == 0) return fail(Str::fmt("volume %d is shorter than before", (int)m_cur + 1));
                if (!recover()) return -1;
                continue;
            }
            i64 k = m_chain->top->read(p, n);
            if (k > 0) { m_delivered += (u64)k; m_series = 0; return k; }
            if (k == 0) {
                int r = finishVolume();
                if (r < 0) return -1;
                continue;
            }
            if (!recover()) return -1;
        }
    }

private:
    bool openVolume() {
        for (int attempt = 0;; ++attempt) {
            Source* f = m_t.open_read(m_m.volumeName(m_cur));
            if (f) {
                VolChain* c = new VolChain;
                c->file = f; c->thr = new ThrottleSource(f, m_rl, m_pr); c->hash = new HashSource(c->thr); c->abortSrc = new AbortSource(c->hash, m_pr); c->top = c->abortSrc;
                if (m_deep) {
                    if (m_m.encrypted) { c->dec = new DecryptSource(c->top, m_pw, (u32)m_cur); c->top = c->dec; }
                    if (!CodecNameOk(m_m.codec.c_str())) { delete c; fail("this backup uses an unsupported compression '" + SafeText(m_m.codec) + "'; only xz is supported"); return false; }
                    c->comp = CodecMakeSource(c->top);
                    if (!c->comp) { delete c; fail("xz decompression is not available"); return false; }
                    c->top = c->comp;
                }
                m_chain = c;
                return true;
            }
            if (!m_t.transient || attempt >= m_rp.attempts) { err = m_t.err; transient = false; m_lost = m_t.transient; return false; }
            if (!wait(attempt + 1, m_t.err, "open")) return false;
        }
    }
    // The current chain failed. A transient failure is waited out and the same volume is opened again.
    bool recover() {
        Str e = m_chain->top->err; bool tmp = m_chain->top->transient;
        if (m_chain->dec && m_chain->dec->wrongPassword()) m_wrongPw = true;
        if (m_wrongPw || e == kAbortedMessage) { err = m_wrongPw ? Str("wrong password") : e; transient = false; return false; }
        if (!tmp || m_series >= m_rp.attempts) { err = e; transient = tmp; m_lost = tmp; return false; }
        if (truncatedAgain()) { err = e; transient = false; return false; }
        return reopen(e);
    }
    // A volume file that is simply too short on the server (it ends before its recorded size, and ends at the same place again
    // on the next try) is not a lost connection: the second time in a row it is an error instead of a long series of retries.
    bool truncatedAgain() {
        const Segment& s = m_m.segments[m_cur];
        if (!(m_chain->hash->sawEof() && m_chain->hash->size() < s.size)) { m_stuck = 0; m_haveFail = false; return false; }
        u64 got = m_chain->hash->size();
        if (m_haveFail && m_failVol == m_cur && got == m_failPos) ++m_stuck; else m_stuck = 0;
        m_haveFail = true; m_failVol = m_cur; m_failPos = got;
        return m_stuck >= 1;
    }
    bool reopen(const Str& reason) {
        delete m_chain; m_chain = nullptr;
        ++m_series;
        if (!wait(m_series, reason, "read")) return false;
        m_skipLeft += m_delivered; m_delivered = 0;                           // what was still to be skipped is skipped too: the place in the volume is delivered + skipLeft
        return true;
    }
    bool wait(int attempt, const Str& reason, const char* what) {
        int d = DelayFor(m_rp, attempt);
        if (m_pr) m_pr->onRetry(what, attempt, m_rp.attempts, d, reason);
        if (!Pause(m_pr, d)) { err = kAbortedMessage; transient = false; return false; }
        Str ce;
        if (!ConnectRetry(m_t, m_rp, m_pr, "reconnect", false, ce)) { err = ce; transient = false; m_lost = ce != kAbortedMessage; return false; }
        return true;
    }
    // 1 = the volume is done and checked, 0 = the connection broke while its tail was read (it is opened again), -1 = error
    int finishVolume() {
        const Segment& s = m_m.segments[m_cur];
        m_chain->hash->drain();                                                  // the checksum covers the whole file, also what the codec did not need
        if (!m_chain->hash->err.empty()) {
            Str e = m_chain->hash->err; bool tmp = m_chain->hash->transient;
            if (e == kAbortedMessage || !tmp || m_series >= m_rp.attempts || truncatedAgain()) { err = e; transient = false; m_lost = tmp && e != kAbortedMessage && m_series >= m_rp.attempts; return -1; }
            return reopen(e) ? 0 : -1;
        }
        if (m_chain->hash->size() != s.size || m_chain->hash->hex() != s.sha256) {
            err = Str::fmt("%s: volume %d: the checksum does not match (the file is damaged or was changed)", m_m.archive.c_str(), (int)m_cur + 1); transient = false; return -1;
        }
        if (m_deep && m_m.encrypted && m_chain->dec) {
            bool last = m_cur + 1 == m_m.segments.size();
            if (m_chain->dec->finalVolume() != last) { err = Str::fmt("%s: volume %d: the backup is incomplete or its volumes were changed", m_m.archive.c_str(), (int)m_cur + 1); transient = false; return -1; }
        }
        delete m_chain; m_chain = nullptr;
        if (m_pr) m_pr->onVolume((int)m_cur + 1, (int)m_m.segments.size());
        ++m_cur; m_delivered = 0; m_skipLeft = 0; m_series = 0;
        return 1;
    }
    Target& m_t; const Manifest& m_m; Str m_pw; bool m_deep; RetryPolicy m_rp; RateLimiter* m_rl; Progress* m_pr;
    size_t m_cur; u64 m_delivered, m_skipLeft; VolChain* m_chain; int m_series; bool m_wrongPw, m_lost;
    bool m_haveFail = false; size_t m_failVol = 0; u64 m_failPos = 0; int m_stuck = 0;
};

const char* const kRestoreState = ".vm2b-restore.json";

} // namespace

bool VerifyBackup(Target& target, const Str& archive, const Str& password, Progress* pr, Manifest* m, Str& err)
{
    ReadOptions ro; return VerifyBackup(target, archive, password, ro, pr, m, err);
}

bool VerifyBackup(Target& target, const Str& archive, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* mout, Str& err)
{
    auto lost = [&](bool yes) { if (ro.lostConnection) *ro.lostConnection = yes; };
    lost(false);
    if (!ConnectRetry(target, ro.retry, pr, "connect", true, err)) { lost(target.transient && err != kAbortedMessage); return false; }
    Manifest m; Str me;
    if (!WithRetry(target, ro.retry, pr, "manifest", me, [&]() { return ReadManifest(target, archive, m, err); })) { lost(target.transient || (!me.empty() && me != kAbortedMessage)); if (!me.empty()) err = me; return false; }
    if (mout) *mout = m;
    if (m.sha256 != RootHash(m.segments)) { err = "the manifest is damaged (its checksum list does not match)"; return false; }
    bool deep = !m.encrypted || !password.empty();
    SegmentedReader rd(target, m, password, deep, ro, pr);
    if (deep) {
        TarReader tr(&rd);
        TarEntry e; int rc; u8 buf[1 << 16];
        while ((rc = tr.next(e)) == 1) { while (tr.read(buf, sizeof(buf)) > 0) {} if (!tr.error().empty()) break; }
        if (rc < 0 || !tr.error().empty()) { err = !rd.err.empty() ? rd.err : tr.error(); if (rd.wrongPassword()) err = "wrong password"; lost(rd.lostConnection()); return false; }
    }
    // the end of the tar stream (zeros / the rest of the last volume) must still pass its checksum; for a backup that cannot be
    // decrypted this is the whole check
    u8 junk[65536]; i64 k;
    while ((k = rd.read(junk, sizeof(junk))) > 0) {}
    if (k < 0) { err = rd.err; if (rd.wrongPassword()) err = "wrong password"; lost(rd.lostConnection()); return false; }
    return true;
}

namespace {

// ---- the local folder as a restore destination -------------------------------------------------------------------
class LocalWriter : public RestoreWriter {
public:
    explicit LocalWriter(const Str& dir) : m_dir(dir), m_cur(INVALID_HANDLE_VALUE) {}
    ~LocalWriter() override { closeCur(); }
    const char* kind() const override { return "local"; }
    bool canResumeInsideFile() const override { return true; }
    bool foldsCase() const override { return true; }
    Str stateFile(const ReadOptions&, const Str&, const Str&) override { return JoinPath(m_dir, kRestoreState); }
    bool prepare(const Manifest&, bool) override { Str e; if (!MakeDirs(m_dir, e)) return fail(e); return true; }
    bool makeDir(const Str& rel) override { Str e; if (!MakeDirs(full(rel), e)) return fail(e); return true; }
    bool canContinue(const RestoreFile& f) override {
        if (f.resumeAt == 0) return true;
        HANDLE h = CreateFileW(FilePath(full(f.rel)), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz; sz.QuadPart = 0;
        bool ok = GetFileSizeEx(h, &sz) && (u64)sz.QuadPart >= f.resumeAt;
        CloseHandle(h);
        return ok;
    }
    bool writeFile(const RestoreFile& f, Source& body, RestoreTick* tick) override {
        Str path = full(f.rel);
        if (f.resumeAt) {                                                         // the rest of a file that was open when an earlier run stopped
            m_cur = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_cur == INVALID_HANDLE_VALUE) return fail(LastErrorText("cannot open", path));
            LARGE_INTEGER p; p.QuadPart = (LONGLONG)f.resumeAt; SetFilePointerEx(m_cur, p, nullptr, FILE_BEGIN); SetEndOfFile(m_cur);
        } else {
            int cut = path.rfind('\\'); Str me; if (cut > 0 && !MakeDirs(path.substr(0, (size_t)cut), me)) return fail(me);
            m_cur = CreateFileW(FilePath(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_cur == INVALID_HANDLE_VALUE) return fail(LastErrorText("cannot create", path));
        }
        Bytes buf; buf.resize(1 << 20); i64 k;
        while ((k = body.read(buf.data(), buf.size())) > 0) {
            const u8* d = buf.data(); size_t left = (size_t)k;
            while (left) {
                DWORD w = 0;
                if (!WriteFile(m_cur, d, left > (1u << 24) ? (1u << 24) : (DWORD)left, &w, nullptr)) { Str e = LastErrorText("cannot write", f.rel); closeCur(true); return fail(e); }
                if (!w) { Str e = Str("cannot write ") + f.rel + ": the system accepted no data"; closeCur(true); return fail(e); }
                d += w; left -= w;
            }
            if (tick) tick->chunk((size_t)k);
        }
        if (k < 0) { closeCur(true); return fail(body.err, body.transient); }
        if (f.mtime > 0) {                                                       // the file keeps its time
            u64 t = (u64)f.mtime * 10000000ull + 116444736000000000ull; FILETIME ft; ft.dwLowDateTime = (DWORD)t; ft.dwHighDateTime = (DWORD)(t >> 32);
            SetFileTime(m_cur, nullptr, nullptr, &ft);
        }
        closeCur();
        return true;
    }
    void checkpoint() override { if (m_cur != INVALID_HANDLE_VALUE) FlushFileBuffers(m_cur); }
private:
    Str full(const Str& rel) const { return JoinPath(m_dir, rel.replace_all("/", "\\")); }
    void closeCur(bool flush = false) { if (m_cur != INVALID_HANDLE_VALUE) { if (flush) FlushFileBuffers(m_cur); CloseHandle(m_cur); m_cur = INVALID_HANDLE_VALUE; } }
    Str m_dir; HANDLE m_cur;
};

// The data of the current tar entry as a Source for the writer; remembers that the archive (not the writer) failed.
struct EntryBody : Source {
    TarReader& tr; bool failed;
    explicit EntryBody(TarReader& t) : tr(t), failed(false) {}
    i64 read(void* p, size_t n) override { i64 k = tr.read(p, n); if (k < 0) { failed = true; err = tr.error(); } return k; }
};

// A checksum over the fields of the restore state: a state file that was damaged in a way that still parses (a flipped digit in the
// position) would otherwise continue at a wrong place and write wrong data without any error. A state without / with a wrong check is ignored.
Str StateCheck(const Str& archive, const Str& root, u64 seg, u64 skip, const Json& tar, const Json& writer)
{
    Str all = archive + "\n" + root + "\n" + Str::fmt("%llu\n%llu\n", (unsigned long long)seg, (unsigned long long)skip) + tar.dump() + "\n" + writer.dump();
    u8 h[32]; Sha256(all.data(), all.size(), h);
    return HexEncode(h, 16);
}

struct RPos { size_t seg; u64 skip; Json tar; bool valid; RPos() : seg(0), skip(0), valid(false) {} };

// One restore run through a writer.
class RestoreRun : public RestoreTick {
public:
    RestoreRun(Target& t, const Str& archive, RestoreWriter& w, const Str& pw, const ReadOptions& ro, Progress* pr)
        : m_target(t), m_archive(archive), m_w(w), m_pw(pw), m_ro(ro), m_pr(pr), m_rd(nullptr), m_tr(nullptr), m_sinceSave(0), m_savedSeg(0), m_series(0), m_inFile(w.canResumeInsideFile()) {}
    ~RestoreRun() { delete m_tr; delete m_rd; }
    bool run(Manifest* mout, Str& err);
    // RestoreTick: the writer wrote n bytes
    void chunk(size_t n) override {
        m_sinceSave += n;
        if (m_inFile && (m_rd->segment() != m_savedSeg || m_sinceSave >= ((u64)256 << 20))) { m_w.checkpoint(); m_lastCp = capture(); persist(m_lastCp); }
    }
private:
    RPos capture() { RPos p; p.seg = m_rd->segment(); p.skip = m_rd->deliveredInSegment(); p.tar = m_tr->saveState(); p.valid = true; return p; }
    void persist(const RPos& p) {
        if (m_statePath.empty()) return;
        Json j = Json::object();
        j["archive"] = m_archive; j["root"] = m_m.sha256; j["segment"] = (u64)p.seg; j["skip"] = p.skip; j["tar"] = p.tar;
        Json ws = m_w.saveState(); if (!ws.is_null()) j["writer"] = ws;
        j["check"] = StateCheck(m_archive, m_m.sha256, (u64)p.seg, p.skip, p.tar, ws);
        Str js = j.dump(); Str e; WriteWholeFile(m_statePath.c_str(), js.data(), js.size(), e);
        m_sinceSave = 0; m_savedSeg = m_rd->segment();
    }
    bool giveUp(const Str& e, bool keep, Str& err) {
        err = e; if (m_rd->wrongPassword()) err = "wrong password";
        if (e == kAbortedMessage && m_pr && m_pr->keepOnAbort()) keep = true;               // a pause
        if (!keep && m_w.cleansUpOnFailure()) { m_w.cleanup(); if (!m_statePath.empty()) DeleteFileW(FilePath(m_statePath)); }
        return false;
    }
    // waits out a transient failure of the writer; false = the retries are used up / aborted (err set)
    bool waitWriter(Str& err) {
        if (!m_w.transient || ++m_series > m_ro.retry.attempts) return giveUp(m_w.err, m_w.transient, err);
        int d = DelayFor(m_ro.retry, m_series);
        if (m_pr) m_pr->onRetry("restore", m_series, m_ro.retry.attempts, d, m_w.err);
        if (!Pause(m_pr, d)) return giveUp(kAbortedMessage, m_pr && m_pr->keepOnAbort(), err);
        for (int a = 0;; ++a) {
            if (m_w.reconnect()) return true;
            if (!m_w.transient || a >= m_ro.retry.attempts) return giveUp(m_w.err, m_w.transient, err);
            if (!Pause(m_pr, DelayFor(m_ro.retry, a + 1))) return giveUp(kAbortedMessage, m_pr && m_pr->keepOnAbort(), err);
        }
    }
    void seekTo(const RPos& p) { m_rd->startAt(p.seg, p.skip); m_tr->loadState(p.tar); m_savedSeg = m_rd->segment(); m_sinceSave = 0; }

    Target& m_target; Str m_archive; RestoreWriter& m_w; Str m_pw; const ReadOptions& m_ro; Progress* m_pr;
    Manifest m_m; SegmentedReader* m_rd; TarReader* m_tr; Str m_statePath;
    u64 m_sinceSave; size_t m_savedSeg; int m_series; bool m_inFile; Vec<u64> m_seen; size_t m_seenCount = 0;      // hashes of the lower-cased paths of the files met in this run (a hash set)
    RPos m_fileStart, m_lastCp;
};

bool RestoreRun::run(Manifest* mout, Str& err)
{
    if (!ConnectRetry(m_target, m_ro.retry, m_pr, "connect", true, err)) return false;
    Str me;
    if (!WithRetry(m_target, m_ro.retry, m_pr, "manifest", me, [&]() { return ReadManifest(m_target, m_archive, m_m, err); })) { if (!me.empty()) err = me; return false; }
    if (mout) *mout = m_m;
    if (m_m.sha256 != RootHash(m_m.segments)) { err = "the manifest is damaged (its checksum list does not match)"; return false; }
    if (m_m.encrypted && m_pw.empty()) { err = "the backup is encrypted: a password is needed"; return false; }

    m_rd = new SegmentedReader(m_target, m_m, m_pw, true, m_ro, m_pr);
    m_tr = new TarReader(m_rd);
    SegmentedReader& rd = *m_rd; TarReader& tr = *m_tr;
    m_statePath = m_w.stateFile(m_ro, m_archive, m_m.sha256);
    // A state file of an earlier, interrupted restore of this very backup: continue where it stopped.
    bool resumed = false;
    if (m_ro.resume && !m_statePath.empty()) {
        Bytes sb; Str se; Json sj;
        if (ReadWholeFile(m_statePath.c_str(), sb, se) && Json::parse((const char*)sb.data(), sb.size(), sj, se) &&
            sj.get("archive").str() == m_archive && sj.get("root").str() == m_m.sha256 && (size_t)sj.get("segment").integer() < m_m.segments.size() &&
            sj.get("check").str() == StateCheck(m_archive, m_m.sha256, (u64)sj.get("segment").integer(), (u64)sj.get("skip").integer(), sj.get("tar"), sj.get("writer")) &&
            m_w.loadState(sj.get("writer"))) {
            rd.startAt((size_t)sj.get("segment").integer(), (u64)sj.get("skip").integer());
            if (tr.loadState(sj.get("tar"))) resumed = true; else { rd.startAt(0, 0); *m_tr = TarReader(m_rd); }
        }
    }
    if (!m_w.prepare(m_m, resumed)) { err = m_w.err; return false; }

    bool pending = false; RestoreFile pf;                      // an entry that was open in the state: its rest comes first
    if (resumed && tr.inEntry() && !tr.current().isDir && !tr.current().skipped && tr.remaining() > 0) {
        Str rel;
        if (!SafeArchivePath(tr.current().path, rel)) { err = "unsafe path in the restore state"; return false; }
        pf.rel = rel; pf.size = tr.current().size; pf.mtime = tr.current().mtime; pf.resumeAt = tr.current().size - tr.remaining();
        if (!m_w.canContinue(pf)) { resumed = false; rd.startAt(0, 0); *m_tr = TarReader(m_rd); }          // the file does not match the state: start over
        else pending = true;
    }
    if (resumed && m_pr) { u64 raw = 0; for (size_t i = 0; i < rd.segment(); ++i) raw += m_m.segments[i].raw; m_pr->onResume((int)rd.segment(), raw); }
    m_savedSeg = rd.segment();

    TarEntry e; int rc = 1;
    for (;;) {
        RestoreFile f;
        if (pending) { f = pf; pending = false; }
        else {
            rc = tr.next(e);
            if (rc != 1) break;
            if (e.skipped) continue;
            Str rel;
            if (!SafeArchivePath(e.path, rel)) return giveUp("unsafe path in the archive: " + e.path, false, err);
            if (e.isDir) {
                while (!m_w.makeDir(rel)) { if (!waitWriter(err)) return false; }
                continue;
            }
            if (m_w.foldsCase()) {                           // "Disk.vmdk" and "disk.vmdk" (a VM from a case-sensitive file system) are ONE file in a Windows folder: the second would silently replace the first
                u64 h = 1469598103934665603ull; Str lo = rel.lower(); for (size_t i = 0; i < lo.size(); ++i) { h ^= (u8)lo[i]; h *= 1099511628211ull; }
                if (!h) h = 1;
                if ((m_seenCount + 1) * 2 > m_seen.size()) {          // an open-addressing set (0 = empty), doubled at half load: linear time for any number of files
                    Vec<u64> old; for (size_t i = 0; i < m_seen.size(); ++i) if (m_seen[i]) old.push_back(m_seen[i]);
                    size_t cap = 1024; while (cap < (m_seenCount + 1) * 4) cap *= 2;
                    m_seen.clear(); m_seen.resize(cap);
                    for (size_t i = 0; i < old.size(); ++i) { size_t at = old[i] & (cap - 1); while (m_seen[at]) at = (at + 1) & (cap - 1); m_seen[at] = old[i]; }
                }
                size_t mask = m_seen.size() - 1, at = h & mask;
                while (m_seen[at]) { if (m_seen[at] == h) return giveUp("the backup holds two files whose names differ only in letter case (or the same name twice), which a Windows folder cannot keep apart; the second one is: " + rel, false, err); at = (at + 1) & mask; }
                m_seen[at] = h; ++m_seenCount;
            }
            f.rel = rel; f.size = e.size; f.mtime = e.mtime; f.resumeAt = 0;
            m_fileStart = capture(); m_lastCp = RPos();
            if (!m_inFile) persist(m_fileStart);              // a writer that cannot continue inside a file: the state is the start of every file
        }
        // write it; a transient failure of the writer is waited out and the file is written again (from the last saved place, or from its start)
        for (;;) {
            EntryBody body(tr);
            if (m_w.writeFile(f, body, this)) break;
            if (body.failed) return giveUp(!rd.err.empty() ? rd.err : body.err, rd.transient, err);
            if (!waitWriter(err)) return false;
            // where to go on: inside the file (writers that can) or at its start
            RPos at = m_fileStart;
            if (m_inFile && m_lastCp.valid) at = m_lastCp;
            if (!at.valid) { rd.startAt(0, 0); *m_tr = TarReader(m_rd); m_fileStart = RPos(); m_lastCp = RPos(); break; }       // (cannot happen for a file entered through next())
            seekTo(at);
            f.resumeAt = tr.inEntry() ? tr.current().size - tr.remaining() : 0;
            if (f.resumeAt && !m_w.canContinue(f)) { seekTo(m_fileStart); f.resumeAt = 0; m_lastCp = RPos(); }
        }
        m_series = 0;
        if (m_pr) m_pr->onFile(f.rel);
        if (rd.segment() != m_savedSeg && m_inFile) persist(capture());
        else if (!m_inFile) persist(capture());           // between files too: what the writer learned from this file (the ESXi meta file) must be in the state
    }
    if (rc < 0) return giveUp(!rd.err.empty() ? rd.err : tr.error(), rd.transient, err);
    // what follows the end of the tar stream is read too: the last volume's checksum is checked at its end
    { u8 junk[65536]; i64 k; while ((k = rd.read(junk, sizeof(junk))) > 0) {} if (k < 0) return giveUp(rd.err.empty() ? Str("read error") : rd.err, rd.transient, err); }
    while (!m_w.finish()) { if (!waitWriter(err)) return false; }
    if (!m_statePath.empty()) DeleteFileW(FilePath(m_statePath));
    return true;
}

} // namespace

RestoreWriter* NewLocalWriter(const Str& destDir) { return new LocalWriter(destDir); }

bool RestoreBackup(Target& target, const Str& archive, const Str& destDir, const Str& password, Progress* pr, Manifest* m, Str& err)
{
    ReadOptions ro; return RestoreBackup(target, archive, destDir, password, ro, pr, m, err);
}

bool RestoreBackup(Target& target, const Str& archive, const Str& destDir, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* mout, Str& err)
{
    if (destDir.empty()) { err = "the destination folder is not set"; return false; }
    LocalWriter w(destDir);
    return RestoreBackup(target, archive, w, password, ro, pr, mout, err);
}

bool RestoreBackup(Target& target, const Str& archive, RestoreWriter& w, const Str& password, const ReadOptions& ro, Progress* pr, Manifest* mout, Str& err)
{
    RestoreRun run(target, archive, w, password, ro, pr);
    return run.run(mout, err);
}

bool DeleteBackup(Target& target, const Str& archive, Str& err)
{
    if (!target.connect()) { err = target.err; return false; }
    Vec<Str> names;
    if (!target.list(names)) { err = target.err; return false; }
    bool ok = true;
    // the manifest goes first: from that moment the backup is no longer listed, and a half-deleted one is never mistaken for a good one
    if (!target.remove(archive + ".json")) { err = target.err; ok = false; }
    for (const Str& n : names) if (IsVolumeName(n, archive) || n == archive + ".resume") if (!target.remove(n)) { err = target.err; ok = false; }
    return ok;
}

bool PruneBackups(Target& target, const Str& vm, int keep, Vec<Str>& removed, Str& err, const Str& protect)
{
    removed.clear();
    if (keep <= 0) return true;
    Vec<Manifest> all;
    if (!ListBackups(target, &vm, all, err)) return false;
    if (!protect.empty()) {                                                 // the protected archive counts as the newest
        Vec<Manifest> ord; Manifest mine; bool have = false;
        for (size_t i = 0; i < all.size(); ++i) { if (all[i].archive == protect) { mine = all[i]; have = true; } else ord.push_back(all[i]); }
        if (have) { ord.push_back(mine); all = ord; }
    }
    for (size_t i = 0; i + (size_t)keep < all.size(); ++i) {
        if (!DeleteBackup(target, all[i].archive, err)) return false;
        removed.push_back(all[i].archive);
    }
    return true;
}

} // namespace vm2b
