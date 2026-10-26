// Autotests of xz: round trips (levels, threads, chunked writes/reads), deterministic output, damage, and streams made by the real xz tool.
#include "autotest.h"
#include "xz.h"

namespace vm2b {

static Bytes XzData(Ctx& c, size_t n, int kind)
{
    Bytes b; b.resize(n);
    switch (kind) {
    case 0: break;                                                                                    // zeros
    case 1: c.fill(b.data(), n); break;                                                               // noise
    case 2: { static const char* w[] = { "vmware ", "esxi ", "backup ", "disk ", "snapshot\n", "flat ", "vmdk ", "thin " };
              size_t i = 0; while (i < n) { const char* s = w[c.rnd(8)]; for (; *s && i < n; ++s, ++i) b[i] = (u8)*s; } break; }
    case 3: { Bytes page; page.resize(4096); c.fill(page.data(), 4096);                                // a "disk": zero / repeated / text / noise pages
              size_t i = 0;
              while (i < n) {
                  u32 t = c.rnd(5); size_t len = 4096 * (size_t)(1 + c.rnd(16));
                  for (size_t k = 0; k < len && i < n; ++k, ++i) b[i] = t == 0 ? 0 : t == 1 ? page[k % 4096] : t == 2 ? (u8)"abcdefgh ijkl\n"[(k + i / 7) % 14] : t == 3 ? (u8)c.next() : (u8)(k * 3 + (k >> 5));
              } break; }
    default: { size_t i = 0; while (i < n) { size_t run = 1 + c.rnd(3000); int k = (int)c.rnd(3); for (size_t j = 0; j < run && i < n; ++j, ++i) b[i] = k == 0 ? 0 : k == 1 ? (u8)c.next() : (u8)((i * 7) >> 3); } }
    }
    return b;
}

struct XzCountSink : Sink {                       // counts calls so a test can see finish() / abort() being forwarded
    MemSink inner; int finishes, aborts;
    XzCountSink() : finishes(0), aborts(0) {}
    bool write(const void* p, size_t n) override { return inner.write(p, n); }
    bool finish() override { ++finishes; return true; }
    void abort() override { ++aborts; }
};
struct XzFailSink : Sink {                        // fails after `limit` bytes with a transient error
    size_t limit, got;
    explicit XzFailSink(size_t l) : limit(l), got(0) {}
    bool write(const void*, size_t n) override { got += n; if (got > limit) return fail("disk full", true); return true; }
    bool finish() override { return true; }
};

static bool XzCompress(const Bytes& in, int level, int threads, size_t chunk, Bytes& out, Str& err)
{
    MemSink ms; Sink* g = MakeXzSink(&ms, level, threads);
    bool ok = true;
    for (size_t o = 0; o < in.size() && ok; o += chunk) { size_t k = in.size() - o < chunk ? in.size() - o : chunk; ok = g->write(in.data() + o, k); }
    if (ok) ok = g->finish();
    if (ok) ok = g->finish();                      // a second finish() is harmless
    if (!ok) err = g->err;
    delete g;
    out = static_cast<Bytes&&>(ms.data);
    return ok;
}
static bool XzDecompress(const Bytes& c, Bytes& out, size_t chunk, Str& err)
{
    MemSource ms(c.data(), c.size()); Source* g = MakeXzSource(&ms);
    out.clear(); Bytes buf; buf.resize(chunk);
    bool ok = true;
    for (;;) { i64 k = g->read(buf.data(), chunk); if (k < 0) { err = g->err; ok = false; break; } if (k == 0) break; size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf.data(), (size_t)k); }
    delete g;
    return ok;
}
static bool XzSame(const Bytes& a, const Bytes& b) { return a.size() == b.size() && (a.size() == 0 || memcmp(a.data(), b.data(), a.size()) == 0); }
static bool XzDecodeEnds(const Bytes& c, size_t* total)       // true = decoded cleanly; it must end either way (never crash or hang)
{
    MemSource ms(c.data(), c.size()); Source* g = MakeXzSource(&ms);
    Bytes buf; buf.resize(1 << 16); size_t t = 0; bool ok = true;
    for (;;) { i64 k = g->read(buf.data(), buf.size()); if (k < 0) { ok = false; break; } if (k == 0) break; t += (size_t)k; if (t > ((size_t)1 << 28)) { ok = false; break; } }
    delete g; if (total) *total = t;
    return ok;
}

VM2B_TEST("xz", xz_roundtrip)
{
    for (int i = 0; i < 40; ++i) {
        size_t sizes[] = { 0, 1, 2, 7, 100, 4095, 65535, 65536, 65537, 131072, 300000 };
        size_t n = c_.coin(60) ? sizes[c_.rnd(11)] : (size_t)c_.range(1, 700000);
        int lv = c_.range(1, 9);
        if (lv >= 7 && n > 120000) n = 120000;                    // keep the group fast under Wine
        Bytes d = XzData(c_, n, (int)c_.rnd(5));
        int thr = c_.coin(50) ? 1 : c_.range(2, 4);
        size_t chunk = c_.coin() ? 1 + c_.rnd(70000) : (size_t)1 << 20, rc = c_.coin() ? 1 + c_.rnd(5000) : 65536;
        Bytes c, o; Str e;
        if (!XzCompress(d, lv, thr, chunk, c, e)) { c_.note("compress failed (size %zu level %d): %s", n, lv, e.c_str()); c_.failed = true; continue; }
        if (!XzDecompress(c, o, rc, e) || !XzSame(o, d)) { c_.note("round trip failed: size %zu level %d threads %d chunk %zu read %zu: %s", n, lv, thr, chunk, rc, e.c_str()); c_.failed = true; }
    }
    return true;
}

VM2B_TEST("xz", xz_all_levels_small)
{
    Bytes d = XzData(c_, 90000, 3);
    for (int lv = 1; lv <= 9; ++lv) {
        Bytes c, o; Str e;
        T_CHECK(XzCompress(d, lv, 1, 1 << 20, c, e));
        T_CHECK(XzDecompress(c, o, 4096, e)); T_CHECK(XzSame(o, d));
    }
    return true;
}

VM2B_TEST("xz", xz_deterministic_threads)
{
    // several segments (16 MiB at levels 1 and 4): the output must not depend on the thread count nor on the write sizes
    struct Case { int level; size_t n; } cases[] = { { 1, 34u << 20 }, { 4, 34u << 20 } };
    for (const Case& k : cases) {
        Bytes d; d.resize(k.n);
        // mostly zero pages with some structure: fast to compress, still several segments, with matches across their boundaries
        Bytes page; page.resize(4096); c_.fill(page.data(), 4096);
        for (size_t i = 0; i < k.n; i += 4096) { u32 t = c_.rnd(8); size_t len = k.n - i < 4096 ? k.n - i : 4096; if (t == 0) memcpy(d.data() + i, page.data(), len); else if (t == 1) for (size_t j = 0; j < len; ++j) d[i + j] = (u8)c_.next(); else memset(d.data() + i, 0, len); }
        Bytes ref; Str e;
        T_CHECK(XzCompress(d, k.level, 1, 1 << 20, ref, e));
        static const int thrs[3] = { 2, 3, 0 };
        for (int thr : thrs) { Bytes c; T_CHECK(XzCompress(d, k.level, thr, thr == 3 ? 100003 : 1 << 22, c, e)); T_CHECK(XzSame(c, ref)); }
        Bytes o; T_CHECK(XzDecompress(ref, o, 5000, e)); T_CHECK(XzSame(o, d));
    }
    return true;
}

VM2B_TEST("xz", xz_incompressible_and_zeros)
{
    Bytes d = XzData(c_, 3u << 20, 1), c, o; Str e;
    T_CHECK(XzCompress(d, 5, 2, 1 << 20, c, e));
    T_CHECK(c.size() < d.size() + d.size() / 100 + 200);
    T_CHECK(XzDecompress(c, o, 4096, e)); T_CHECK(XzSame(o, d));
    Bytes z; z.resize(40u << 20);
    static const int lvs[2] = { 1, 5 };
    for (int lv : lvs) {
        T_CHECK(XzCompress(z, lv, 2, 1 << 22, c, e));
        T_CHECK(c.size() < 40000);                                 // 40 MiB of zeros: a few KB
        T_CHECK(XzDecompress(c, o, 1 << 16, e)); T_CHECK(XzSame(o, z));
    }
    return true;
}

VM2B_TEST("xz", xz_sink_contract)
{
    { XzCountSink cs; Sink* g = MakeXzSink(&cs, 3, 2); T_CHECK(g->write("abc", 3)); T_CHECK(g->finish()); T_CHECK(cs.finishes == 1); T_CHECK(g->finish()); T_CHECK(cs.finishes == 1); T_CHECK(!g->write("x", 1)); delete g; }
    { XzCountSink cs; Sink* g = MakeXzSink(&cs, 3, 3); Bytes d = XzData(c_, 100000, 2); T_CHECK(g->write(d.data(), d.size())); g->abort(); T_CHECK(cs.aborts == 1); T_CHECK(cs.finishes == 0); delete g; }
    { XzCountSink cs; Sink* g = MakeXzSink(&cs, 5, 4); Bytes d = XzData(c_, 100000, 2); T_CHECK(g->write(d.data(), d.size())); delete g; }          // dropped without finish(): no hang, no leak
    { XzCountSink cs; Sink* g = MakeXzSink(&cs, 1, 1); T_CHECK(g->finish()); T_CHECK(cs.finishes == 1); delete g;      // empty input: a valid empty stream
      Bytes o; Str e; T_CHECK(XzDecompress(cs.inner.data, o, 100, e)); T_CHECK(o.empty()); }
    {   // a failing target: the error (with its transient flag) comes back from write() or finish()
        XzFailSink fs(100); Sink* g = MakeXzSink(&fs, 1, 2); Bytes d = XzData(c_, 400000, 1);
        bool ok = g->write(d.data(), d.size()) && g->finish();
        T_CHECK(!ok); T_CHECK(g->err == "disk full"); T_CHECK(g->transient); delete g;
    }
    return true;
}

VM2B_TEST("xz", xz_damage)
{
    Bytes d = XzData(c_, 400000, 3), c; Str e;
    T_CHECK(XzCompress(d, 3, 1, 1 << 20, c, e));
    int survived = 0;
    for (int t = 0; t < 400; ++t) {
        Bytes x = c;
        u32 m = c_.rnd(4);
        if (m == 0) x.resize(c_.rnd((u32)x.size()));
        else if (m == 1) x[c_.rnd((u32)x.size())] ^= (u8)(1 << c_.rnd(8));
        else if (m == 2) { for (int k = 0; k < 4; ++k) x[c_.rnd((u32)x.size())] = (u8)c_.next(); }
        else { u32 p = c_.rnd((u32)x.size()); for (size_t k = p; k < x.size() && k < p + 20; ++k) x[k] = (u8)c_.next(); }
        size_t tot; bool ok = XzDecodeEnds(x, &tot);
        if (ok && tot == d.size()) ++survived;
    }
    T_CHECK(survived < 10);
    for (int t = 0; t < 300; ++t) {                                // random bytes, some behind a valid stream header
        Bytes x; x.resize(c_.rnd(200)); c_.fill(x.data(), x.size());
        if (t % 2 && x.size() > 12) memcpy(x.data(), "\xFD" "7zXZ\0\0\x04", 8);
        XzDecodeEnds(x, nullptr);
    }
    return true;
}

// ---- streams made by the real xz tool (xz 5.8, -9): the text is TextOf(n)
static Bytes XzTextOf(size_t n) { static const char pat[] = "abcdefghij klmno\n"; Bytes b; b.resize(n); for (size_t i = 0; i < n; ++i) b[i] = (u8)pat[(i * 7 + i / 13) % 17]; return b; }
static const char* kXzRef_crc64 = "fd377a585a000004e6d6b44604c037c80121011c000000000000000092c3884ce000c7002f5d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3480a2e40000e3914885c0f55e58000153c801000000fb6a9628b1c467fb020000000004595a";
static const char* kXzRef_crc32 = "fd377a585a0000016922de3604c037c80121011c000000000000000092c3884ce000c7002f5d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3480a2e40000de953f4d00014fc8010000001ba8825c3e300d8b020000000001595a";
static const char* kXzRef_none = "fd377a585a000000ff12d94104c037c80121011c000000000000000092c3884ce000c7002f5d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3480a2e4000000014bc8010000000dea13c7a8000afc020000000000595a";
static const char* kXzRef_blocks = "fd377a585a000004e6d6b44603c03dac0221011c00000000ecdcdc77e0012b00355d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3f8777e543284af6000000000000080333d5ddf9d79103c03dac0221011c00000000ecdcdc77e0012b00355d00379989dabcbf70786a7edd7bbfc763cc031cee473a1f041c1680f102a83aa1af4acddc89cf0a12a24320e2f74512d83c009983d00000000000bcb5a551050dec4c03c03dac0221011c00000000ecdcdc77e0012b00355d0036990005120a4536c4b871f75d7d435071006ed8f59cc9742ceb7ddcaa994908a6a4c8ed12977d15ffa302ad79c9e07e67d976c00000000000380a8f56a0a0ea5203c0276421011c0000000000725b53e6e00063001f5d00359889729d3e11e7995aaa5049e0ee8a7a91543179f80d28b4208d857892ac0000eb8789a5f6569a22000455ac0255ac0255ac023f640000004fdbb173ac273e2d040000000004595a";
static const char* kXzRef_concat = "fd377a585a000004e6d6b44604c037c80121011c000000000000000092c3884ce000c7002f5d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3480a2e40000e3914885c0f55e58000153c801000000fb6a9628b1c467fb020000000004595a0000000000000000fd377a585a0000016922de3604c041e80721011c000000000000000074d56602e003e700395d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3f8777e5b455b927dbc78c1980000000000046c34be6000159e8070000004539a76d3e300d8b020000000001595a";
static const char* kXzRef_empty = "fd377a585a000004e6d6b446000000001cdf44211fb6f37d010000000004595a";
static const char* kXzRef_sha = "fd377a585a00000ae1fb0ca104c037c80121011c000000000000000092c3884ce000c7002f5d00309a0a1f0c2e7e7c5d9405cedebcf406c29de6f201734fd74136a872109767b10b07d85286dd547444ec7e3480a2e40000ad7933a1e6b6bd8ff8eb65d93bf2bb36d38405525fb5f73f71718ad32aeeadb100016bc8010000003befbfc0b6e9df1c02000000000a595a";

static Bytes XzFromHex(const char* h) { Bytes b; HexDecode(h, strlen(h), b); return b; }

VM2B_TEST("xz", xz_reads_real_xz_streams)
{
    Str e; Bytes o;
    struct { const char* name; const char* hex; size_t n1, n2; } t[] = {
        { "crc64", kXzRef_crc64, 200, 0 }, { "crc32", kXzRef_crc32, 200, 0 }, { "none", kXzRef_none, 200, 0 },
        { "blocks(4 blocks)", kXzRef_blocks, 1000, 0 }, { "concatenated + padding", kXzRef_concat, 200, 1000 }, { "empty", kXzRef_empty, 0, 0 } };
    for (auto& k : t) {
        Bytes c = XzFromHex(k.hex);
        static const size_t chunks[3] = { 1, 7, 4096 };
        for (size_t chunk : chunks) {
            if (!XzDecompress(c, o, chunk, e)) { c_.note("%s (read %zu): %s", k.name, chunk, e.c_str()); c_.failed = true; continue; }
            Bytes want = XzTextOf(k.n1);
            if (k.n2) { Bytes w2 = XzTextOf(k.n2); size_t m = want.size(); want.resize(m + w2.size()); memcpy(want.data() + m, w2.data(), w2.size()); }
            if (!XzSame(o, want)) { c_.note("%s: wrong data", k.name); c_.failed = true; }
        }
    }
    // SHA-256 check (supported by liblzma), and every single-bit flip of the small streams must not crash
    { Bytes c = XzFromHex(kXzRef_sha); T_CHECK(XzDecompress(c, o, 100, e)); T_CHECK(XzSame(o, XzTextOf(200))); }      // liblzma verifies SHA-256 too
    { Bytes c = XzFromHex(kXzRef_crc64); for (size_t i = 0; i < c.size(); ++i) for (int b = 0; b < 8; ++b) { Bytes x = c; x[i] ^= (u8)(1 << b); size_t tot; bool ok = XzDecodeEnds(x, &tot); if (ok) { T_CHECK(tot == 200); } } }
    return true;
}

static void XzLE32(Bytes& b, u32 v) { for (int i = 0; i < 4; ++i) b.push_back((u8)(v >> (8 * i))); }
static Bytes XzStreamWithBlockHeader(const u8* filters, size_t nf, u32 flagsByte, bool addLzmaData)
{
    Bytes s; const u8 hdr[8] = { 0xFD, '7', 'z', 'X', 'Z', 0, 0, 1 };      // CRC32 check
    for (int i = 0; i < 8; ++i) s.push_back(hdr[i]);
    XzLE32(s, Crc32(0, hdr + 6, 2));
    Bytes h; h.push_back(0); h.push_back((u8)flagsByte);
    for (size_t i = 0; i < nf; ++i) h.push_back(filters[i]);
    while ((h.size() + 4) % 4) h.push_back(0);
    h[0] = (u8)((h.size() + 4) / 4 - 1);
    XzLE32(h, Crc32(0, h.data(), h.size()));
    for (size_t i = 0; i < h.size(); ++i) s.push_back(h[i]);
    if (addLzmaData) { s.push_back(0x01); s.push_back(0); s.push_back(0); s.push_back('A'); s.push_back(0); }
    return s;
}

VM2B_TEST("xz", xz_rejects_unsupported)
{
    Str e; Bytes o;
    { const u8 f[] = { 0x04, 0x00, 0x21, 0x01, 0x16 };  Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x01, true); T_CHECK(!XzDecompress(s, o, 100, e)); }       // BCJ x86 + LZMA2
    { const u8 f[] = { 0x03, 0x01, 0x00 };              Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, true); T_CHECK(!XzDecompress(s, o, 100, e)); }       // delta
    { const u8 f[] = { 0x21, 0x01, 40 };                Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, true); T_CHECK(!XzDecompress(s, o, 100, e)); T_CHECK(e.find("dictionary") >= 0); }   // 4 GiB dictionary
    { const u8 f[] = { 0x21, 0x01, 41 };                Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, true); T_CHECK(!XzDecompress(s, o, 100, e)); }
    { const u8 f[] = { 0x21, 0x01, 38 };                Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, true); T_CHECK(!XzDecompress(s, o, 100, e)); T_CHECK(e.find("dictionary") >= 0); }   // 1.5 GiB + : refused
    { Bytes s; const u8 hdr[8] = { 0xFD, '7', 'z', 'X', 'Z', 0, 0, 3 }; for (int i = 0; i < 8; ++i) s.push_back(hdr[i]); XzLE32(s, Crc32(0, hdr + 6, 2)); T_CHECK(!XzDecompress(s, o, 100, e)); }  // reserved check type
    { Bytes s; T_CHECK(!XzDecompress(s, o, 100, e)); }                                                                                                                                              // empty input
    { const u8 f[] = { 0x21, 0x01, 0x16 };              Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, true); T_CHECK(!XzDecompress(s, o, 100, e)); }                                     // good header, but the stream is cut short
    {   // hostile chunks: a huge claimed size with almost no data must end with an error (no hang, no big allocation)
        const u8 f[] = { 0x21, 0x01, 0x16 };
        Bytes s = XzStreamWithBlockHeader(f, sizeof f, 0x00, false);
        const u8 ch[] = { 0xE0, 0x1F, 0xFF, 0x00, 0x04, 0x5D, 0, 0, 0, 0, 0 };
        for (u8 b : ch) s.push_back(b);
        T_CHECK(!XzDecompress(s, o, 1 << 20, e));
        Bytes s2 = XzStreamWithBlockHeader(f, sizeof f, 0x00, false);
        const u8 ch2[] = { 0x01, 0xFF, 0xFF, 'a', 'b', 'c' };
        for (u8 b : ch2) s2.push_back(b);
        T_CHECK(!XzDecompress(s2, o, 1 << 20, e)); T_CHECK(e.find("end of data") >= 0);
        Bytes s3 = XzStreamWithBlockHeader(f, sizeof f, 0x00, false);
        const u8 ch3[] = { 0x02, 0x00, 0x00, 'a', 0 };                           // no dictionary reset first
        for (u8 b : ch3) s3.push_back(b);
        T_CHECK(!XzDecompress(s3, o, 1 << 20, e));
    }
    return true;
}


// ---- per-block x86 BCJ filter ------------------------------------------------------------------------------------------------------------
// x86-like code: filler and CALL / JMP rel32 to 48 "functions" (after the BCJ filter the operands repeat, so it compresses far better)
static Bytes XzCode(Ctx& c, size_t n)
{
    Bytes b; b.resize(n); u32 tgt[48]; for (u32& t : tgt) t = n ? c.rnd((u32)n) : 0; size_t i = 0;
    while (i < n) {
        size_t fill = 4 + c.rnd(28); for (size_t k = 0; k < fill && i < n; ++k) b[i++] = c.coin() ? (u8)(0x48 + c.rnd(8)) : (u8)c.next();
        if (i + 5 <= n) { u32 rel = tgt[c.rnd(48)] - (u32)(i + 5); b[i] = c.rnd(4) ? 0xE8 : 0xE9; for (int k = 0; k < 4; ++k) b[i + 1 + k] = (u8)(rel >> (8 * k)); i += 5; }
    }
    return b;
}
static u32 XzLe32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
// Walks the blocks of a stream (footer -> index -> block headers) and tells for each whether its first filter is x86 BCJ (id 4).
static bool XzBlockBcj(const Bytes& x, Vec<bool>& bcj)
{
    bcj.clear(); size_t n = x.size();
    if (n < 24) return false;
    size_t isz = ((size_t)XzLe32(&x[n - 8]) + 1) * 4, ipos = n - 12 - isz;
    if (isz > n || x[ipos] != 0) return false;
    size_t q = ipos + 1; u64 cnt = 0; int sh = 0;
    for (;;) { u8 b = x[q++]; cnt |= (u64)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
    size_t pos = 12;
    for (u64 k = 0; k < cnt; ++k) {
        u64 unp = 0; sh = 0; for (;;) { u8 b = x[q++]; unp |= (u64)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
        for (;;) { u8 b = x[q++]; if (!(b & 0x80)) break; }                      // the uncompressed size
        bcj.push_back(x[pos + 2] == 0x04);
        pos += (size_t)((unp + 3) & ~(u64)3);
    }
    return true;
}

VM2B_TEST("xz", xz_bcj_per_block)
{
    // level 1: 3 MiB blocks. code / noise / code / text / code (short): the decision flips from block to block, whatever the thread count
    const size_t B = 3u << 20;
    Bytes d; Vec<int> want;
    int kinds[] = { 0, 1, 0, 2, 0 };
    for (int k = 0; k < 5; ++k) {
        size_t len = k == 4 ? B / 2 : B; Bytes q = kinds[k] == 0 ? XzCode(c_, len) : kinds[k] == 1 ? XzData(c_, len, 1) : XzData(c_, len, 2);
        size_t o = d.size(); d.resize(o + q.size()); memcpy(d.data() + o, q.data(), q.size()); want.push_back(kinds[k] == 0 ? 1 : 0);
    }
    for (size_t k = 0; k < 5; ++k) { size_t off = k * B, len = d.size() - off < B ? d.size() - off : B; T_CHECK(XzChooseBcj(d.data() + off, len, nullptr) == (want[k] != 0)); }
    Bytes ref, c, o; Str e;
    T_CHECK(XzCompress(d, 1, 1, 1 << 20, ref, e));
    Vec<bool> bcj; T_CHECK(XzBlockBcj(ref, bcj)); T_CHECK(bcj.size() == 5);
    if (bcj.size() == 5) for (size_t k = 0; k < 5; ++k) T_CHECK(bcj[k] == (want[k] != 0));
    static const int thrs[3] = { 2, 3, 0 };
    for (int thr : thrs) { T_CHECK(XzCompress(d, 1, thr, thr == 3 ? 100003 : 1 << 22, c, e)); T_CHECK(XzSame(c, ref)); }
    T_CHECK(XzDecompress(ref, o, 70000, e)); T_CHECK(XzSame(o, d));
    // the filter pays: the operands repeat after it
    Bytes code = XzCode(c_, B), cc; T_CHECK(XzCompress(code, 1, 1, 1 << 20, cc, e)); T_CHECK(cc.size() < code.size() / 100 * 85);   // the filler is noise; what BCJ removes is the repeated operands
    return true;
}

VM2B_TEST("xz", xz_bcj_never_inflates_or_breaks)
{
    // noise full of E8 00 00 00 00 / E9 FF FF FF FF patterns: candidates everywhere, but the filter gains nothing: no BCJ, no inflation
    Bytes d = XzData(c_, 4u << 20, 1);
    for (size_t i = 0; i + 5 < d.size(); i += 7 + c_.rnd(20)) { d[i] = c_.coin() ? 0xE8 : 0xE9; d[i + 1] = (u8)c_.next(); d[i + 2] = (u8)c_.next(); d[i + 3] = (u8)c_.next(); d[i + 4] = c_.coin() ? 0 : 0xFF; }
    Bytes c, o; Str e; T_CHECK(XzCompress(d, 2, 2, 1 << 20, c, e));
    T_CHECK(c.size() < d.size() + d.size() / 100 + 200);
    T_CHECK(XzDecompress(c, o, 4096, e)); T_CHECK(XzSame(o, d));
    {   // a block of noise as big as a level-5 block (48 MiB): the output buffer of the block coder must be big enough for the stored chunks
        Bytes big = XzData(c_, (50u << 20) + c_.rnd(1000), 1), z, r;
        T_CHECK(XzCompress(big, 5, 2, 1 << 22, z, e)); T_CHECK(z.size() < big.size() + big.size() / 1000 + 1000);
        T_CHECK(XzDecompress(z, r, 1 << 16, e)); T_CHECK(XzSame(r, big));
    }
    // random mixes of code-like and other blocks, odd sizes (a block shorter than 64 KiB never gets the filter)
    for (int i = 0; i < 6; ++i) {
        Bytes m; int parts = 1 + (int)c_.rnd(5);
        for (int k = 0; k < parts; ++k) { size_t len = c_.coin() ? c_.range(1, 70000) : c_.range(100000, 2500000); Bytes q = c_.coin() ? XzCode(c_, len) : XzData(c_, len, (int)c_.rnd(5)); size_t at = m.size(); m.resize(at + q.size()); memcpy(m.data() + at, q.data(), q.size()); }
        int lv = c_.coin() ? 1 : 2; int thr = c_.coin() ? 1 : c_.range(2, 4);
        Bytes z, r; T_CHECK(XzCompress(m, lv, thr, c_.coin() ? 777 : 1 << 20, z, e)); T_CHECK(XzDecompress(z, r, c_.coin() ? 100 : 65536, e)); T_CHECK(XzSame(r, m));
    }
    return true;
}

} // namespace vm2b
