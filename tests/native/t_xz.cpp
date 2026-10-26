// Native test of xz.cpp:  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -pthread tests/native/t_xz.cpp src/xz.cpp src/base.cpp -llzma -o t_xz
//   t_xz self            round trips, determinism, damage, fuzz (no external tools)
//   t_xz c L T in out    compress a file (level, threads); prints time and sizes
//   t_xz d in out        decompress a file (any .xz)
//   t_xz gen dir         writes the reference inputs dir/in_<n>.bin and our own dir/our_<n>_L<level>.xz
//   t_xz check dir       decodes dir/{xz,py,z7}_<n>_<v>.xz made by the other tools and compares with dir/in_<n>.bin
//   t_xz interop dir     all of it: gen, then xz / 7z / python decode our files and write theirs, then check (needs xz, 7z, python3)
#include <initializer_list>
#include <time.h>
#include "../../src/xz.h"
using namespace vm2b;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int fails = 0;
static u64 rs = 88172645463325252ull;
static u32 rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static int g_cpu;
static double now() { timespec t; clock_gettime(g_cpu ? CLOCK_PROCESS_CPUTIME_ID : CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static Bytes genCode(size_t n) {                        // x86-like: random filler and CALL / JMP rel32 to 48 functions (BCJ makes the operands repeat)
    Bytes b; b.resize(n); u32 tgt[48]; for (u32& t : tgt) t = n ? rnd() % (u32)n : 0; size_t i = 0;
    while (i < n) {
        size_t fill = 4 + rnd() % 28; for (size_t k = 0; k < fill && i < n; ++k) b[i++] = (rnd() & 1) ? (u8)(0x48 + rnd() % 8) : (u8)rnd();
        if (i + 5 <= n) { int32_t rel = (int32_t)(tgt[rnd() % 48] - (u32)(i + 5)); b[i] = (rnd() % 4) ? 0xE8 : 0xE9; for (int k = 0; k < 4; ++k) b[i + 1 + k] = (u8)((u32)rel >> (8 * k)); i += 5; }
    }
    return b;
}

static Bytes gen(int kind, size_t n) {
    Bytes b; b.resize(n);
    size_t i = 0;
    switch (kind) {
    case 7: return genCode(n);
    case 0: break;                                                                                     // zeros
    case 1: for (; i < n; ++i) b[i] = (u8)rnd(); break;                                                 // noise
    case 2: { static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n", "VM2Backup ", "vmdk "};
              while (i < n) { const char* s = w[rnd() % 10]; for (; *s && i < n; ++s, ++i) b[i] = (u8)*s; } break; }
    case 3: for (; i < n; ++i) b[i] = (u8)(i % 251); break;
    case 4: for (; i < n; ++i) b[i] = (rnd() % 16 == 0) ? (u8)rnd() : (u8)'a'; break;
    case 5: for (; i < n; ++i) b[i] = (i / 100000) % 2 ? (u8)rnd() : 0; break;                           // zeros / noise in turn
    case 6: {                                                                                            // "disk": zero pages, repeated pages, text, noise
        Bytes page; page.resize(4096); for (size_t k = 0; k < 4096; ++k) page[k] = (u8)rnd();
        while (i < n) {
            u32 t = rnd() % 5; size_t len = 4096 * (1 + rnd() % 16);
            for (size_t k = 0; k < len && i < n; ++k, ++i) b[i] = t == 0 ? 0 : t == 1 ? page[k % 4096] : t == 2 ? (u8)"abcdefgh ijkl\n"[(k + i / 7) % 14] : t == 3 ? (u8)rnd() : (u8)(k * 3 + (k >> 5));
        } break; }
    }
    return b;
}
static bool comp(const Bytes& in, int level, int threads, size_t chunk, Bytes& out, Str& err) {
    MemSink ms; Sink* g = MakeXzSink(&ms, level, threads);
    for (size_t o = 0; o < in.size(); o += chunk) { size_t k = in.size() - o < chunk ? in.size() - o : chunk; if (!g->write(in.data() + o, k)) { err = g->err; delete g; return false; } }
    bool ok = g->finish(); if (!ok) err = g->err;
    ok = ok && g->finish();                                                                            // repeated finish()
    delete g;
    out = static_cast<Bytes&&>(ms.data); return ok;
}
static bool decomp(const Bytes& c, Bytes& out, size_t chunk, Str& err) {
    MemSource ms(c.data(), c.size()); Source* g = MakeXzSource(&ms);
    out.clear(); Bytes buf; buf.resize(chunk);
    bool ok = true;
    for (;;) { i64 k = g->read(buf.data(), chunk); if (k < 0) { err = g->err; ok = false; break; } if (k == 0) break; size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf.data(), (size_t)k); }
    delete g; return ok;
}
static bool same(const Bytes& a, const Bytes& b) { return a.size() == b.size() && (a.size() == 0 || memcmp(a.data(), b.data(), a.size()) == 0); }

// Decoding must never crash or hang: returns true when it ended cleanly (an error is fine, too).
static bool decodeAny(const Bytes& c, size_t* total) {
    MemSource ms(c.data(), c.size()); Source* g = MakeXzSource(&ms);
    Bytes buf; buf.resize(1 << 16); size_t t = 0; bool ok = true;
    for (;;) { i64 k = g->read(buf.data(), buf.size()); if (k < 0) { ok = false; break; } if (k == 0) break; t += (size_t)k; if (t > ((size_t)1 << 30)) { ok = false; break; } }
    delete g; if (total) *total = t; return ok;
}

static void selfTests() {
    size_t sizes[] = {0, 1, 2, 3, 5, 100, 1000, 65535, 65536, 65537, 200000, 1500000};
    for (int kind = 0; kind < 8; ++kind) for (size_t sz : sizes) {
        Bytes d = gen(kind, sz);
        for (int lv : {1, 2, 3, 4, 5, 7, 9}) {
            if ((lv >= 7 && sz > 300000) || (lv % 2 == 0 && sz > 700000 && kind != 0)) continue;
            Bytes c, o; Str e;
            int thr = lv == 5 ? 3 : 1;
            bool ok = comp(d, lv, thr, (lv == 5 || lv == 2) ? 7919 : 1 << 20, c, e);
            if (!ok) { printf("compress FAIL kind %d size %zu level %d: %s\n", kind, sz, lv, e.c_str()); fails++; continue; }
            ok = decomp(c, o, lv == 1 ? 1 : (lv == 3 ? 777 : 65536), e) && same(o, d);
            if (!ok) { printf("roundtrip FAIL kind %d size %zu level %d: %s\n", kind, sz, lv, e.c_str()); fails++; }
            if (kind == 0 && sz == 1500000 && lv == 5) printf("zeros 1.5MB -> %zu bytes\n", c.size());
            if (kind == 1 && sz == 1500000 && lv == 5) printf("noise 1.5MB -> %zu bytes\n", c.size());
        }
    }
    // determinism over thread counts and write chunking, with several segments (level 1: 16 MiB segments -> 40 MB)
    {
        Bytes d = gen(6, 40u << 20); Bytes ref; Str e;
        CHECK(comp(d, 1, 1, 1 << 20, ref, e));
        for (int t : {2, 3, 8, 0}) { Bytes c; CHECK(comp(d, 1, t, t == 3 ? 100000 : 1 << 22, c, e)); CHECK(same(c, ref)); }
        Bytes o; CHECK(decomp(ref, o, 5000, e)); CHECK(same(o, d));
        printf("disk-like 40MB level 1 -> %zu\n", ref.size());
    }
    // incompressible data: uncompressed LZMA2 chunks, small overhead
    {
        Bytes d = gen(1, 5u << 20), c, o; Str e;
        CHECK(comp(d, 6, 2, 1 << 20, c, e)); CHECK(c.size() < d.size() + d.size() / 100 + 200); CHECK(decomp(c, o, 4096, e)); CHECK(same(o, d));
        printf("noise 5MB level 6 -> %zu\n", c.size());
    }
    // a SOURCE that fails at any byte (lost connection / abort): that error must come out, not a guess like "unexpected end of data"
    {
        struct Cut : Source { const u8* p; size_t n, pos, at; Cut(const Bytes& b, size_t a) : p(b.data()), n(b.size()), pos(0), at(a) {}
            i64 read(void* o, size_t k) override { if (pos >= at) { err = "link lost (test)"; transient = true; return -1; } size_t m = k < at - pos ? k : at - pos; if (m > n - pos) m = n - pos; memcpy(o, p + pos, m); pos += m; return (i64)m; } };
        Bytes d = gen(6, 300000), c; Str e; CHECK(comp(d, 3, 1, 1 << 20, c, e)); int wrong = 0;
        for (int t = 0; t < 500; ++t) {
            size_t at = t < 80 ? (size_t)t : rnd() % (c.size() - 1);
            Cut src(c, at); Source* g = MakeXzSource(&src); Bytes buf; buf.resize(t % 2 ? 100 : 65536); i64 k;
            while ((k = g->read(buf.data(), buf.size())) > 0) {}
            if (k >= 0 || g->err != "link lost (test)" || !g->transient) { if (wrong++ < 5) printf("xz cut at %zu: k=%lld err='%s' transient=%d\n", at, (long long)k, g->err.c_str(), (int)g->transient); }
            delete g;
        }
        CHECK(wrong == 0);
    }
    // damage
    {
        Bytes d = gen(6, 600000), c; Str e; CHECK(comp(d, 3, 1, 1 << 20, c, e));
        int survived = 0;
        for (int t = 0; t < 1500; ++t) {
            Bytes x = c;
            int m = rnd() % 4;
            if (m == 0) x.resize(rnd() % x.size());
            else if (m == 1) x[rnd() % x.size()] ^= (u8)(1 << (rnd() % 8));
            else if (m == 2) { for (int k = 0; k < 4; ++k) x[rnd() % x.size()] = (u8)rnd(); }
            else { size_t p = rnd() % x.size(); for (size_t k = p; k < x.size() && k < p + 20; ++k) x[k] = (u8)rnd(); }
            size_t tot; bool ok = decodeAny(x, &tot);
            if (ok && tot == d.size()) ++survived;
        }
        printf("damage: %d of 1500 survived unchanged\n", survived);
        CHECK(survived < 30);
        for (int t = 0; t < 500; ++t) { Bytes x; x.resize(rnd() % 200); for (size_t k = 0; k < x.size(); ++k) x[k] = (u8)rnd(); if (t % 2 && x.size() > 12) memcpy(x.data(), "\xFD" "7zXZ\0\0\x04", 8); decodeAny(x, nullptr); }
    }
}

static const char* kInteropPy = R"PY(
import sys, os, subprocess, lzma, glob, re
d = sys.argv[1]; bad = 0; n = 0
def run(cmd, **kw): return subprocess.run(cmd, capture_output=True, **kw)
# 1. what we write must be read by xz, Python and 7-Zip
for f in sorted(glob.glob(d + '/our_*.xz')):
    idx = re.match(r'.*our_(\d+)_L(\d+)\.xz', f).group(1)
    ref = open('%s/in_%s.bin' % (d, idx), 'rb').read(); n += 1
    r = run(['xz', '-dc', f])
    if r.returncode or r.stdout != ref: print('XZ FAIL', f, r.stderr[:200]); bad += 1
    if run(['xz', '-tv', f]).returncode: print('XZ -t FAIL', f); bad += 1
    try:
        if lzma.decompress(open(f, 'rb').read()) != ref: print('PY MISMATCH', f); bad += 1
    except Exception as e: print('PY FAIL', f, e); bad += 1
    r = run(['7z', 'x', '-so', '-bd', f])
    if r.returncode or r.stdout != ref: print('7Z FAIL', f, r.stderr[:200]); bad += 1
print('our files checked', n, 'bad', bad); sys.stdout.flush()
# 2. make files with the other tools for the decoder (xz_<n>_<v>.xz, py_..., z7_...)
for f in sorted(glob.glob(d + '/in_*.bin')):
    idx = re.match(r'.*in_(\d+)\.bin', f).group(1); data = open(f, 'rb').read(); big = len(data) > 1000000; v = 0
    for lvl in (['-0', '-6'] if big else ['-0', '-1', '-3', '-6', '-9', '-9e']):
        open('%s/xz_%s_%d.xz' % (d, idx, v), 'wb').write(run(['xz', '-c', lvl], input=data).stdout); v += 1
    for extra in [['--check=crc32'], ['--check=none'], ['-T4', '--block-size=100000'], ['--block-size=4096', '-1']]:
        open('%s/xz_%s_%d.xz' % (d, idx, v), 'wb').write(run(['xz', '-c'] + extra, input=data).stdout); v += 1
    v = 0
    for preset in ([0, 6] if big else [0, 1, 6, 9, 6 | lzma.PRESET_EXTREME]):
        open('%s/py_%s_%d.xz' % (d, idx, v), 'wb').write(lzma.compress(data, preset=preset)); v += 1
    open('%s/py_%s_%d.xz' % (d, idx, v), 'wb').write(lzma.compress(data, check=lzma.CHECK_CRC32)); v += 1
    open('%s/py_%s_%d.xz' % (d, idx, v), 'wb').write(lzma.compress(data, check=lzma.CHECK_NONE)); v += 1
    v = 0
    for mx in (['-mx=1', '-mx=5'] if big else ['-mx=1', '-mx=5', '-mx=9']):
        out = '%s/z7_%s_%d.xz' % (d, idx, v)
        if os.path.exists(out): os.remove(out)
        run(['7z', 'a', '-txz', mx, '-mmt=4', '-bd', '-y', out, f]); v += 1
sys.exit(1 if bad else 0)
)PY";

static bool readFile(const char* p, Bytes& b) { Str e; return ReadWholeFile(p, b, e, (size_t)1 << 31); }
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    Str mode = argv[1];
    if (mode == "self") { if (const char* sd = getenv("XZ_SEED")) { rs ^= strtoull(sd, nullptr, 10) * 0x9E3779B97F4A7C15ull; if (!rs) rs = 1; printf("xz seed %s\n", sd); } selfTests(); printf(fails ? "FAILED: %d\n" : "OK\n", fails); return fails != 0; }
    if (mode == "c" && argc >= 6) {
        Bytes d, c; Str e; if (!readFile(argv[4], d)) return 1;
        g_cpu = atoi(argv[3]) == 1;                  // one thread: CPU time (the machine may be busy)
        double t = now(); bool ok = comp(d, atoi(argv[2]), atoi(argv[3]), 1 << 22, c, e); t = now() - t;
        if (!ok) { printf("fail: %s\n", e.c_str()); return 1; }
        Str er; WriteWholeFile(argv[5], c.data(), c.size(), er);
        printf("%.4f %zu\n", t, c.size()); return 0;
    }
    if (mode == "d" && argc >= 4) {
        Bytes c, o; Str e; if (!readFile(argv[2], c)) return 1;
        double t = now(); bool ok = decomp(c, o, 1 << 16, e); t = now() - t;
        if (!ok) { printf("fail: %s\n", e.c_str()); return 1; }
        Str er; WriteWholeFile(argv[3], o.data(), o.size(), er);
        printf("%.4f %zu\n", t, o.size()); return 0;
    }
    if (mode == "interop" && argc >= 3) {
        Str dir = argv[2], cmd = Str("mkdir -p '") + dir + "'";
        if (system(cmd.c_str())) return 1;
        Str e; Str script = dir + "/interop.py";
        if (!WriteWholeFile(script.c_str(), kInteropPy, strlen(kInteropPy), e)) { printf("%s\n", e.c_str()); return 1; }
        Str self = argv[0];
        cmd = "'" + self + "' gen '" + dir + "'"; if (system(cmd.c_str())) return 1;
        cmd = "python3 -I '" + script + "' '" + dir + "'"; int pr = system(cmd.c_str());
        cmd = "'" + self + "' check '" + dir + "'"; int cr = system(cmd.c_str());
        printf(pr || cr ? "INTEROP FAILED\n" : "INTEROP OK\n"); return pr || cr;
    }
    if (mode == "gen" && argc >= 3) {
        int idx = 0; size_t sizes[] = {0, 1, 100, 65536, 300000, 2000000};
        for (int kind = 0; kind < 8; ++kind) for (size_t sz : sizes) {
            Bytes d = gen(kind, sz); char p[512]; snprintf(p, sizeof p, "%s/in_%d.bin", argv[2], idx++); Str e; WriteWholeFile(p, d.data(), d.size(), e);
            for (int lv : {1, 4, 5, 9}) { if (lv == 9 && sz > 300000) continue; Bytes c; comp(d, lv, 2, 1 << 20, c, e); snprintf(p, sizeof p, "%s/our_%d_L%d.xz", argv[2], idx - 1, lv); WriteWholeFile(p, c.data(), c.size(), e); }
        }
        {   // code / noise / code / zeros / code: at level 1 (3 MiB blocks) the BCJ decision flips from block to block
            Bytes d; size_t parts[5][2] = { {7, 3500000}, {1, 3500000}, {7, 3500000}, {0, 1000000}, {7, 2500000} };
            for (auto& pt : parts) { Bytes q = gen((int)pt[0], pt[1]); size_t o = d.size(); d.resize(o + q.size()); memcpy(d.data() + o, q.data(), q.size()); }
            char p[512]; snprintf(p, sizeof p, "%s/in_%d.bin", argv[2], idx++); Str e; WriteWholeFile(p, d.data(), d.size(), e);
            for (int lv : {1, 2}) { Bytes c; comp(d, lv, 3, 1 << 20, c, e); snprintf(p, sizeof p, "%s/our_%d_L%d.xz", argv[2], idx - 1, lv); WriteWholeFile(p, c.data(), c.size(), e); }
        }
        return 0;
    }
    if (mode == "check" && argc >= 3) {          // files named in_<n>.bin and <prefix>_<n>_<v>.xz  -> compare
        int count = 0;
        for (int n = 0; n < 42; ++n) {
            char p[512]; snprintf(p, sizeof p, "%s/in_%d.bin", argv[2], n); Bytes d; if (!readFile(p, d)) continue;
            for (const char* pre : {"xz", "py", "z7"}) for (int v = 0; v < 12; ++v) {
                snprintf(p, sizeof p, "%s/%s_%d_%d.xz", argv[2], pre, n, v); Bytes c, o; if (!readFile(p, c)) continue;
                Str e; ++count; bool ok = decomp(c, o, 3000 + v, e) && same(o, d);
                if (!ok) { printf("FAIL %s: %s\n", p, e.c_str()); ++fails; }
            }
        }
        printf("checked %d files, %d failures\n", count, fails); return fails != 0;
    }
    return 2;
}
