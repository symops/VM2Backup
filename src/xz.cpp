// VM2Backup — xz on top of liblzma (xz-utils, built by tools/deps.sh: see xz.h for the stream layout and the guarantees).
#include "xz.h"
#include <lzma.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace vm2b {

namespace {

const size_t kBufSize = 128 * 1024;
const u64 kMaxDecoderMem = (u64)1536 << 20;           // a stream that needs more than this (a 1.5 GiB dictionary) is refused

// Our own level table (not the xz presets): the preset's match finder / nice length / depth, but a larger dictionary from level 4 on. The
// block size is 3 x the dictionary (cap 192 MiB) and depends on the level only, never on the thread count, so the output depends only on the
// data and the level. Per thread about 3 x block + the match finder (~6 x dictionary) of memory is held.
const u32 kDictMiB[10] = { 0, 1, 2, 4, 8, 16, 16, 32, 64, 64 };

void LevelOptions(int level, u32 dictMiB, lzma_options_lzma& o, u64& block)
{
    lzma_lzma_preset(&o, (uint32_t)level);
    o.dict_size = dictMiB << 20;
    block = (u64)dictMiB * 3 << 20;
    if (block > ((u64)192 << 20)) block = (u64)192 << 20;
}

const char* EncErr(lzma_ret r)
{
    switch (r) {
    case LZMA_MEM_ERROR: case LZMA_MEMLIMIT_ERROR: return "xz: out of memory";
    case LZMA_OPTIONS_ERROR: case LZMA_UNSUPPORTED_CHECK: return "xz: unsupported compression options";
    default: return "xz: compression failed";
    }
}

// ---- the x86 BCJ decision, per block ------------------------------------------------------------------------------------------------
// The BCJ filter turns the relative operands of x86 CALL / JMP (E8 / E9 + 32-bit offset) into absolute ones, which repeat and compress better.
// Executable code gains 3-5%; anything else gains nothing or loses a little. A VM disk mixes both, so every block decides for itself, from its own
// bytes only (never from timing or thread count), in two steps:
//   1. one cheap scan counts the "plausible" E8 / E9 candidates: the offset's top byte is 00 or FF and |offset| < the block size. Per 256 KiB
//      window the count is kept. Random data and text give < 0.01% of the bytes, x86 code 0.3-1%. Below kBcjMinPer10000 (0.15% of the bytes): no BCJ.
//   2. otherwise a TRIAL: the 3 windows with the most candidates (ties: the lowest offset) are compressed with a fast preset with and without the
//      filter; BCJ is used only if it saves more than kBcjMinGainPer1000 (1.5%) on that sample.
const size_t kBcjWindow = 256 * 1024;
const u64 kBcjMinPer10000 = 15;             // candidates per 10000 bytes (0.15%)
const u64 kBcjMinGainPer1000 = 15;         // 1.5% smaller on the sample

} // namespace

// (outside the anonymous namespace so that the test tool can include this file and look at the numbers)
bool XzChooseBcj(const u8* p, size_t n, XzBcjStats* st)
{
    if (st) { st->cand = st->per10000 = st->plain = st->bcj = 0; st->trial = st->use = false; }
    if (n < 64 * 1024) return false;
    size_t nw = (n + kBcjWindow - 1) / kBcjWindow;
    u32* cnt = (u32*)calloc(nw, sizeof(u32));
    if (!cnt) return false;
    u64 cand = 0;
    for (size_t i = 0; i + 5 <= n; ++i) {
        if ((p[i] & 0xFE) != 0xE8) continue;
        u8 hi = p[i + 4];
        if (hi != 0 && hi != 0xFF) continue;
        int32_t rel = (int32_t)((u32)p[i + 1] | (u32)p[i + 2] << 8 | (u32)p[i + 3] << 16 | (u32)hi << 24);
        u64 a = rel < 0 ? (u64)(-(int64_t)rel) : (u64)rel;
        if (a >= n) continue;
        ++cand; ++cnt[i / kBcjWindow]; i += 4;
    }
    u64 per = cand * 10000 / n;
    if (st) { st->cand = cand; st->per10000 = per; }
    bool use = false;
    if (per >= kBcjMinPer10000) {
        size_t pick[3]; int np = 0;
        for (int k = 0; k < 3; ++k) {
            size_t best = (size_t)-1; u32 bc = 0;
            for (size_t w = 0; w < nw; ++w) { bool taken = false; for (int q = 0; q < np; ++q) if (pick[q] == w) taken = true; if (!taken && cnt[w] > bc) { bc = cnt[w]; best = w; } }
            if (best == (size_t)-1) break;
            pick[np++] = best;
        }
        // in file order, so the sample is a function of the block only
        for (int a = 0; a < np; ++a) for (int b = a + 1; b < np; ++b) if (pick[b] < pick[a]) { size_t t = pick[a]; pick[a] = pick[b]; pick[b] = t; }
        size_t slen = 0; for (int k = 0; k < np; ++k) { size_t off = pick[k] * kBcjWindow; slen += (n - off < kBcjWindow ? n - off : kBcjWindow); }
        u8* smp = (u8*)malloc(slen ? slen : 1); size_t cap = slen + slen / 2 + 4096; u8* o1 = (u8*)malloc(cap);
        if (smp && o1) {
            size_t at = 0;
            for (int k = 0; k < np; ++k) { size_t off = pick[k] * kBcjWindow, l = (n - off < kBcjWindow ? n - off : kBcjWindow); memcpy(smp + at, p + off, l); at += l; }
            lzma_options_lzma o; lzma_lzma_preset(&o, 1);
            lzma_filter f1[2], f2[3];
            f1[0].id = LZMA_FILTER_LZMA2; f1[0].options = &o; f1[1].id = LZMA_VLI_UNKNOWN; f1[1].options = nullptr;
            f2[0].id = LZMA_FILTER_X86; f2[0].options = nullptr; f2[1] = f1[0]; f2[2] = f1[1];
            size_t p1 = 0, p2 = 0;
            bool ok1 = lzma_raw_buffer_encode(f1, nullptr, smp, slen, o1, &p1, cap) == LZMA_OK;
            bool ok2 = lzma_raw_buffer_encode(f2, nullptr, smp, slen, o1, &p2, cap) == LZMA_OK;
            if (ok1 && ok2) { if (st) { st->trial = true; st->plain = p1; st->bcj = p2; } use = p2 * 1000 < p1 * (1000 - kBcjMinGainPer1000); }
        }
        free(smp); free(o1);
    }
    free(cnt);
    if (st) st->use = use;
    return use;
}

namespace {

// ---- threads (Win32 or pthread) ---------------------------------------------------------------------------------------------------------
#ifdef _WIN32
struct Mutex { CRITICAL_SECTION cs; Mutex() { InitializeCriticalSection(&cs); } ~Mutex() { DeleteCriticalSection(&cs); } void lock() { EnterCriticalSection(&cs); } void unlock() { LeaveCriticalSection(&cs); } };
struct Cond { CONDITION_VARIABLE cv; Cond() { InitializeConditionVariable(&cv); } void wait(Mutex& m) { SleepConditionVariableCS(&cv, &m.cs, INFINITE); } void broadcast() { WakeAllConditionVariable(&cv); } };
typedef HANDLE ThreadH;
typedef DWORD (WINAPI *ThreadFn)(LPVOID);
#define XZ_THREAD_RET DWORD WINAPI
bool ThreadStart(ThreadH& h, ThreadFn fn, void* arg) { h = CreateThread(nullptr, 0, fn, arg, 0, nullptr); return h != nullptr; }
void ThreadJoin(ThreadH h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
#else
struct Mutex { pthread_mutex_t m; Mutex() { pthread_mutex_init(&m, nullptr); } ~Mutex() { pthread_mutex_destroy(&m); } void lock() { pthread_mutex_lock(&m); } void unlock() { pthread_mutex_unlock(&m); } };
struct Cond { pthread_cond_t c; Cond() { pthread_cond_init(&c, nullptr); } ~Cond() { pthread_cond_destroy(&c); } void wait(Mutex& mu) { pthread_cond_wait(&c, &mu.m); } void broadcast() { pthread_cond_broadcast(&c); } };
typedef pthread_t ThreadH;
typedef void* (*ThreadFn)(void*);
#define XZ_THREAD_RET void*
bool ThreadStart(ThreadH& h, ThreadFn fn, void* arg) { return pthread_create(&h, nullptr, fn, arg) == 0; }
void ThreadJoin(ThreadH h) { pthread_join(h, nullptr); }
#endif

// One block: the raw bytes in, a complete .xz block (header + data + padding + check) out.
struct Job {
    u8* in; size_t inLen;
    u8* out; size_t outLen;
    u64 unpadded, usize;
    int state;                   // 0 free, 1 queued, 2 running, 3 done
    int err;                     // 0, or an lzma_ret / kAborted
    Job() : in(nullptr), inLen(0), out(nullptr), outLen(0), unpadded(0), usize(0), state(0), err(0) {}
};
const int kAbortedJob = -1000;

class XzSink : public Sink {
public:
    XzSink(Sink* out, int level, int threads)
        : m_out(out), m_level(level < 1 ? 1 : level > 9 ? 9 : level), m_threads(threads), m_init(false), m_ok(true), m_finished(false), m_headerDone(false),
          m_block(0), m_cur(nullptr), m_curLen(0), m_ring(nullptr), m_N(0), m_T(1), m_submitted(0), m_written(0), m_nextRun(0), m_stopping(false), m_nth(0),
          m_abort(0), m_index(nullptr) {
        memset(&m_opt, 0, sizeof m_opt);
        m_index = lzma_index_init(nullptr);
    }
    ~XzSink() {
        m_abort = 1;
        shutdown();
        if (m_ring) { for (int i = 0; i < m_N; ++i) { free(m_ring[i].in); free(m_ring[i].out); } delete[] m_ring; }
        free(m_cur);
        if (m_index) lzma_index_end(m_index, nullptr);
    }
    bool write(const void* p, size_t n) override {
        if (!m_ok) return false;
        if (m_finished) return fail("xz: write after finish");
        if (!n) return true;
        if (!start()) return false;
        const u8* d = (const u8*)p;
        while (n) {
            size_t take = m_block - m_curLen < n ? (size_t)(m_block - m_curLen) : n;
            memcpy(m_cur + m_curLen, d, take);
            m_curLen += take; d += take; n -= take;
            if (m_curLen == m_block && !submit()) return false;
        }
        return true;
    }
    bool finish() override {
        if (m_finished) return m_ok;
        m_finished = true;
        if (!m_ok) { shutdown(); return false; }
        if (!start()) { shutdown(); return false; }
        if (m_curLen && !submit()) { shutdown(); return false; }
        while (m_written < m_submitted) if (!drainOne()) { shutdown(); return false; }
        shutdown();
        if (!ensureHeader()) return false;
        // the index and the footer
        u32 isz = (u32)lzma_index_size(m_index);
        u8* ib = (u8*)malloc(isz ? isz : 1);
        if (!ib) return bad("xz: out of memory");
        size_t ip = 0;
        lzma_ret r = lzma_index_buffer_encode(m_index, ib, &ip, isz);
        if (r != LZMA_OK) { free(ib); return bad(EncErr(r)); }
        lzma_stream_flags fl; memset(&fl, 0, sizeof fl);
        fl.version = 0; fl.check = LZMA_CHECK_CRC64; fl.backward_size = lzma_index_size(m_index);
        u8 foot[LZMA_STREAM_HEADER_SIZE];
        r = lzma_stream_footer_encode(&fl, foot);
        bool ok = r == LZMA_OK && outWrite(ib, ip) && outWrite(foot, sizeof foot);
        free(ib);
        if (r != LZMA_OK) return bad(EncErr(r));
        if (!ok) return false;
        if (!m_out->finish()) return bad2();
        return true;
    }
    void abort() override { m_abort = 1; m_ok = false; m_finished = true; shutdown(); m_out->abort(); }

private:
    Sink* m_out; int m_level, m_threads;
    bool m_init, m_ok, m_finished, m_headerDone;
    u64 m_block; lzma_options_lzma m_opt;
    u8* m_cur; u64 m_curLen;
    Job* m_ring; int m_N, m_T; u64 m_submitted, m_written, m_nextRun;
    Mutex m_mu; Cond m_cvWork, m_cvDone;
    bool m_stopping; ThreadH m_th[64]; int m_nth;
    volatile int m_abort;
    lzma_index* m_index;

    bool bad(const char* e) { m_ok = false; return fail(e); }
    bool bad2() { m_ok = false; return failFrom(m_out); }
    bool outWrite(const void* p, size_t n) { if (!n) return true; if (!m_out->write(p, n)) return bad2(); return true; }
    bool ensureHeader() {
        if (m_headerDone) return true;
        m_headerDone = true;
        lzma_stream_flags fl; memset(&fl, 0, sizeof fl);
        fl.version = 0; fl.check = LZMA_CHECK_CRC64;
        u8 h[LZMA_STREAM_HEADER_SIZE];
        lzma_ret r = lzma_stream_header_encode(&fl, h);
        if (r != LZMA_OK) return bad(EncErr(r));
        return outWrite(h, sizeof h);
    }
    bool start() {
        if (m_init) return true;
        u32 dictMiB = kDictMiB[m_level];
        lzma_filter fl[2];
        fl[0].id = LZMA_FILTER_LZMA2; fl[0].options = &m_opt; fl[1].id = LZMA_VLI_UNKNOWN; fl[1].options = nullptr;
        LevelOptions(m_level, dictMiB, m_opt, m_block);
        // per thread: the encoder, the raw block, the compressed block
        auto perThread = [&]() -> u64 { u64 e = lzma_raw_encoder_memusage(fl); return (e == UINT64_MAX ? (u64)1 << 40 : e) + 2 * m_block + (m_block >> 2); };
        // A 32-bit process cannot hold the big dictionaries: shrink until one thread fits in ~900 MiB (streams stay readable everywhere).
        if (sizeof(size_t) == 4)
            while (dictMiB > 4 && perThread() + m_block > ((u64)900 << 20)) { dictMiB /= 2; LevelOptions(m_level, dictMiB, m_opt, m_block); }
        const u64 budget = sizeof(size_t) == 4 ? ((u64)700 << 20) : ((u64)2560 << 20);
        u32 t = m_threads > 0 ? (u32)m_threads : (u32)NumCpus();
        if (t < 1) t = 1;
        if (t > 64) t = 64;
        if (m_threads <= 0 || sizeof(size_t) == 4) { u64 per = perThread(); while (t > 1 && per * t + m_block > budget) --t; }
        m_T = (int)t; m_N = m_T;
        m_ring = new Job[m_N];
        m_cur = (u8*)malloc((size_t)m_block);
        if (!m_cur) return bad("xz: out of memory");
        m_curLen = 0;
        m_init = true;
        return true;
    }
    static int NumCpus() {
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors;
#else
        long n = sysconf(_SC_NPROCESSORS_ONLN); return n < 1 ? 1 : (int)n;
#endif
    }

    // Compresses job j (runs in a worker, or in the caller when there is one thread).
    void encodeJob(Job& j) {
        j.err = 0;
        bool bcj = XzChooseBcj(j.in, j.inLen, nullptr);
        lzma_options_lzma o = m_opt;
        lzma_filter fl[3]; int k = 0;
        if (bcj) { fl[k].id = LZMA_FILTER_X86; fl[k].options = nullptr; ++k; }
        fl[k].id = LZMA_FILTER_LZMA2; fl[k].options = &o; ++k;
        fl[k].id = LZMA_VLI_UNKNOWN; fl[k].options = nullptr;
        lzma_block blk; memset(&blk, 0, sizeof blk);
        blk.version = 0; blk.check = LZMA_CHECK_CRC64; blk.filters = fl;
        blk.compressed_size = LZMA_VLI_UNKNOWN; blk.uncompressed_size = LZMA_VLI_UNKNOWN;
        lzma_ret r = lzma_block_header_size(&blk);
        if (r != LZMA_OK) { j.err = r; return; }
        size_t cap = j.inLen + j.inLen / 50 + blk.header_size + 65536;       // (lzma_block_buffer_bound() is for the one-shot coder: the streaming one needs a little more for noise)
        j.out = (u8*)malloc(cap);
        if (!j.out) { j.err = LZMA_MEM_ERROR; return; }
        r = lzma_block_header_encode(&blk, j.out);
        if (r != LZMA_OK) { j.err = r; return; }
        lzma_stream s = LZMA_STREAM_INIT;
        r = lzma_block_encoder(&s, &blk);
        if (r != LZMA_OK) { j.err = r; return; }
        s.next_out = j.out + blk.header_size; s.avail_out = cap - blk.header_size;
        size_t pos = 0;
        while (pos < j.inLen) {                                  // 256 KiB at a time, so an abort is noticed within a fraction of a second
            if (m_abort) { lzma_end(&s); j.err = kAbortedJob; return; }
            size_t take = j.inLen - pos < ((size_t)256 << 10) ? j.inLen - pos : ((size_t)256 << 10);
            s.next_in = j.in + pos; s.avail_in = take; pos += take;
            while (s.avail_in) { r = lzma_code(&s, LZMA_RUN); if (r != LZMA_OK) { lzma_end(&s); j.err = r; return; } if (m_abort) { lzma_end(&s); j.err = kAbortedJob; return; } }
        }
        do { r = lzma_code(&s, LZMA_FINISH); } while (r == LZMA_OK);
        if (r != LZMA_STREAM_END) { lzma_end(&s); j.err = r; return; }
        j.outLen = blk.header_size + (size_t)s.total_out;
        lzma_end(&s);
        j.unpadded = lzma_block_unpadded_size(&blk);
        j.usize = blk.uncompressed_size;
        if (j.unpadded == 0) j.err = LZMA_PROG_ERROR;
    }
    static XZ_THREAD_RET workerMain(void* arg) { ((XzSink*)arg)->worker(); return 0; }
    void worker() {
        for (;;) {
            m_mu.lock();
            Job* j = nullptr;
            for (;;) {
                if (m_stopping) { m_mu.unlock(); return; }
                Job& c = m_ring[m_nextRun % (u64)m_N];
                if (m_nextRun < m_submitted && c.state == 1) { j = &c; c.state = 2; ++m_nextRun; break; }
                m_cvWork.wait(m_mu);
            }
            m_mu.unlock();
            encodeJob(*j);
            m_mu.lock(); j->state = 3; m_cvDone.broadcast(); m_mu.unlock();
        }
    }
    void shutdown() {
        if (!m_nth) return;
        m_mu.lock(); m_stopping = true; m_cvWork.broadcast(); m_mu.unlock();
        for (int i = 0; i < m_nth; ++i) ThreadJoin(m_th[i]);
        m_nth = 0;
    }
    // Takes the oldest block (waits for it), appends it to the stream and the index.
    bool drainOne() {
        Job& j = m_ring[m_written % (u64)m_N];
        m_mu.lock();
        while (j.state != 3) m_cvDone.wait(m_mu);
        m_mu.unlock();
        bool ok = j.err == 0;
        if (!ok) { if (j.err == kAbortedJob) m_ok = false; else bad(EncErr((lzma_ret)j.err)); }
        else if (!ensureHeader() || !outWrite(j.out, j.outLen)) ok = false;
        else if (lzma_index_append(m_index, nullptr, j.unpadded, j.usize) != LZMA_OK) ok = bad("xz: out of memory");
        free(j.out); j.out = nullptr; j.outLen = 0;
        m_mu.lock(); j.state = 0; ++m_written; m_mu.unlock();
        return ok && m_ok;
    }
    // The block in m_cur goes to a worker (or is compressed right here with one thread).
    bool submit() {
        if (m_T == 1) {
            Job& j = m_ring[0];
            free(j.in); j.in = m_cur; j.inLen = (size_t)m_curLen; j.state = 2;
            m_cur = (u8*)malloc((size_t)m_block); m_curLen = 0;
            if (!m_cur) return bad("xz: out of memory");
            encodeJob(j);
            j.state = 3; ++m_submitted; ++m_nextRun;
            return drainOne();
        }
        while (m_submitted - m_written >= (u64)m_N) if (!drainOne()) return false;
        Job& j = m_ring[m_submitted % (u64)m_N];
        u8* t = j.in; j.in = m_cur; j.inLen = (size_t)m_curLen;
        if (t) m_cur = t; else { m_cur = (u8*)malloc((size_t)m_block); if (!m_cur) return bad("xz: out of memory"); }
        m_curLen = 0;
        m_mu.lock();
        j.state = 1; ++m_submitted;
        bool spawn = m_nth < m_T && (u64)m_nth < m_submitted - m_written;
        m_cvWork.broadcast();
        m_mu.unlock();
        if (spawn) { if (ThreadStart(m_th[m_nth], workerMain, this)) ++m_nth; }
        if (m_nth == 0) return bad("xz: cannot start a thread");
        return m_ok;
    }
};

class XzSource : public Source {
public:
    explicit XzSource(Source* in) : m_in(in), m_inbuf(nullptr), m_st(S_RUN), m_eof(false), m_total(0) {
        memset(&m_s, 0, sizeof m_s);
        lzma_ret r = lzma_stream_decoder(&m_s, kMaxDecoderMem, LZMA_CONCATENATED | LZMA_TELL_UNSUPPORTED_CHECK);
        m_inbuf = (uint8_t*)malloc(kBufSize);
        if (r != LZMA_OK || !m_inbuf) { m_st = S_ERR; err = "xz: out of memory"; }
    }
    ~XzSource() { lzma_end(&m_s); free(m_inbuf); }
    i64 read(void* p, size_t n) override {
        if (m_st == S_ERR) return -1;
        size_t got = 0;
        while (got < n && m_st == S_RUN) {
            if (m_s.avail_in == 0 && !m_eof) {
                i64 k = m_in->read(m_inbuf, kBufSize);
                if (k < 0) { m_st = S_ERR; return failFrom(m_in); }
                if (k == 0) m_eof = true;
                m_s.next_in = m_inbuf; m_s.avail_in = (size_t)k; m_total += (u64)k;
            }
            m_s.next_out = (uint8_t*)p + got; m_s.avail_out = n - got;
            lzma_ret r = lzma_code(&m_s, m_eof ? LZMA_FINISH : LZMA_RUN);
            got = n - m_s.avail_out;
            if (r == LZMA_OK) continue;
            if (r == LZMA_STREAM_END) { m_st = S_DONE; break; }
            m_st = S_ERR;
            if (r == LZMA_BUF_ERROR) return fail(m_total ? "xz: unexpected end of data" : "xz: not an xz file (empty input)");
            return fail(DecErr(r));
        }
        return (i64)got;
    }

private:
    enum { S_RUN, S_DONE, S_ERR };
    Source* m_in; lzma_stream m_s; uint8_t* m_inbuf; int m_st; bool m_eof; u64 m_total;
    static const char* DecErr(lzma_ret r) {
        switch (r) {
        case LZMA_MEM_ERROR: return "xz: out of memory (dictionary)";
        case LZMA_MEMLIMIT_ERROR: return "xz: the dictionary of the stream is too large (more than 1.5 GiB)";
        case LZMA_FORMAT_ERROR: return "xz: not an xz file (or garbage after the stream)";
        case LZMA_OPTIONS_ERROR: return "xz: unsupported stream options";
        case LZMA_UNSUPPORTED_CHECK: return "xz: unsupported check type";
        default: return "xz: damaged data";
        }
    }
};

} // namespace

Sink* MakeXzSink(Sink* out, int level, int threads) { return new XzSink(out, level, threads); }
Source* MakeXzSource(Source* in) { return new XzSource(in); }

} // namespace vm2b
