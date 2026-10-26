// Autotests of the core: JSON, tar, hashes, AES-GCM, stream encryption.
#include "autotest.h"
#include "tar.h"
#include "crypto.h"

namespace vm2b {

static Str Hex(const u8* p, size_t n) { return HexEncode(p, n); }

static Bytes RandomData(Ctx& c, size_t n, int kind)
{
    Bytes b; b.resize(n);
    switch (kind) {
    case 0: break;                                                          // zeros
    case 1: c.fill(b.data(), n); break;                                     // noise
    case 2: { static const char* w[] = { "vmware ", "esxi ", "backup ", "disk ", "snapshot\n", "flat ", "vmdk ", "thin " };
              size_t i = 0; while (i < n) { const char* s = w[c.rnd(8)]; for (; *s && i < n; ++s, ++i) b[i] = (u8)*s; } break; }
    default: { size_t i = 0; while (i < n) { size_t run = 1 + c.rnd(3000); int k = (int)c.rnd(3); for (size_t j = 0; j < run && i < n; ++j, ++i) b[i] = k == 0 ? 0 : k == 1 ? (u8)c.next() : (u8)((i * 7) >> 3); } }
    }
    return b;
}

VM2B_TEST("core", json_roundtrip)
{
    Json j = Json::object();
    j["name"] = "VM \xd0\x94\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xb5 \"q\" \\ \n";
    j["n"] = (i64)-9007199254740993ll; j["f"] = 0.1; j["t"] = true; j["z"] = Json();
    Json arr = Json::array(); for (int i = 0; i < 50; ++i) arr.push(Json(Str::fmt("item %d", i))); j["list"] = arr;
    for (int pretty = 0; pretty < 2; ++pretty) {
        Json k; Str e;
        T_CHECK(Json::parse(j.dump(pretty != 0), k, e));
        T_CHECK(k == j);
        T_CHECK(k.get("n").integer() == -9007199254740993ll);
    }
    const char* bad[] = { "", "{", "[1,]", "{\"a\":}", "01", "\"\\x\"", "[1] x", "\"\\ud800\"", "nul", "{\"a\" 1}", "-", "1.", "\"a\nb\"" };
    for (const char* b : bad) { Json k; Str e; T_CHECK(!Json::parse(b, strlen(b), k, e)); }
    Str deep; for (int i = 0; i < 1000; ++i) deep += '['; Json k; Str e; T_CHECK(!Json::parse(deep, k, e));
    return true;
}

VM2B_TEST("core", tar_roundtrip)
{
    struct F { Str path; Bytes data; i64 mtime; };
    Vec<F> files;
    int n = c_.range(1, 25);
    for (int i = 0; i < n; ++i) {
        F f;
        Str p = Str::fmt("vm%d/", i % 3);
        if (c_.coin(30)) { int depth = c_.range(1, 8); for (int k = 0; k < depth; ++k) p += Str::fmt("dir%u_%s/", c_.rnd(99), c_.coin() ? "long_long_long_long_long" : "d"); }
        p += c_.coin(30) ? "\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba" : "disk";
        p += Str::fmt("-%d.vmdk", i);
        f.path = p; f.mtime = 1700000000 + (i64)c_.rnd(100000000);
        size_t sz = c_.coin(20) ? c_.rnd(2) * 512 : (size_t)c_.range(0, 200000);
        f.data = RandomData(c_, sz, (int)c_.rnd(4));
        files.push_back(f);
    }
    MemSink ms; TarWriter w(&ms);
    for (const F& f : files) T_CHECK(w.add_bytes(f.path, f.data.data(), f.data.size(), f.mtime));
    T_CHECK(w.finish());
    MemSource src(ms.data.data(), ms.data.size()); TarReader r(&src);
    TarEntry e; size_t idx = 0; int rc;
    while ((rc = r.next(e)) == 1) {
        if (idx >= files.size()) { T_CHECK(false); break; }
        if (e.path != files[idx].path) c_.note("path differs (%zu vs %zu bytes): %s", e.path.size(), files[idx].path.size(), files[idx].path.c_str());
        T_CHECK(e.path == files[idx].path); T_CHECK(e.size == files[idx].data.size()); T_CHECK(e.mtime == files[idx].mtime);
        Bytes got; got.resize((size_t)e.size);
        size_t o = 0; while (o < got.size()) { i64 k2 = r.read(got.data() + o, got.size() - o); if (k2 <= 0) break; o += (size_t)k2; }
        T_CHECK(o == got.size() && (got.empty() || memcmp(got.data(), files[idx].data.data(), got.size()) == 0));
        ++idx;
    }
    T_CHECK(rc == 0); T_CHECK(idx == files.size());
    return true;
}

VM2B_TEST("core", hashes_known_vectors)
{
    u8 h[32];
    Sha256("abc", 3, h);
    T_CHECK(Hex(h, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256("", 0, h);
    T_CHECK(Hex(h, 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    u8 s1[20]; Sha1("abc", 3, s1);
    T_CHECK(Hex(s1, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    u8 key[20]; memset(key, 0x0b, 20);                                       // RFC 4231 test case 1
    HmacSha256(key, 20, "Hi There", 8, h);
    T_CHECK(Hex(h, 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    HmacSha256("Jefe", 4, "what do ya want for nothing?", 28, h);           // case 2
    T_CHECK(Hex(h, 32) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    u8 dk[32];                                                              // PBKDF2-HMAC-SHA256 vectors (RFC 7914 / widely published)
    T_CHECK(Pbkdf2Sha256("password", 8, "salt", 4, 1, dk, 32));
    T_CHECK(Hex(dk, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    T_CHECK(Pbkdf2Sha256("password", 8, "salt", 4, 4096, dk, 32));
    T_CHECK(Hex(dk, 32) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    Sha256Stream st; for (int i = 0; i < 1000; ++i) st.update("a", 1);        // streaming = one shot
    st.final(h); u8 h2[32]; Bytes a; a.resize(1000); memset(a.data(), 'a', 1000); Sha256(a.data(), 1000, h2);
    T_CHECK(memcmp(h, h2, 32) == 0);
    u8 r1[32], r2[32]; T_CHECK(RandomBytes(r1, 32) && RandomBytes(r2, 32)); T_CHECK(memcmp(r1, r2, 32) != 0);
    return true;
}

VM2B_TEST("core", aesgcm_known_vectors)
{
    AesGcm g; u8 key[32] = {0}, nonce[12] = {0}, tag[16], ct[16], pt[16] = {0};
    T_CHECK(g.init(key));
    T_CHECK(g.encrypt(nonce, nullptr, 0, nullptr, 0, nullptr, tag));                          // NIST GCM test case 13
    T_CHECK(Hex(tag, 16) == "530f8afbc74536b9a963b4f1c4cb738b");
    T_CHECK(g.encrypt(nonce, nullptr, 0, pt, 16, ct, tag));                                   // test case 14
    T_CHECK(Hex(ct, 16) == "cea7403d4d606b6e074ec5d3baf39d18"); T_CHECK(Hex(tag, 16) == "d0d1c8a799996bf0265b98b5d48ab919");
    u8 back[16]; T_CHECK(g.decrypt(nonce, nullptr, 0, ct, 16, back, tag)); T_CHECK(memcmp(back, pt, 16) == 0);
    tag[3] ^= 1; T_CHECK(!g.decrypt(nonce, nullptr, 0, ct, 16, back, tag)); tag[3] ^= 1;
    u8 aad = 1; T_CHECK(!g.decrypt(nonce, &aad, 1, ct, 16, back, tag));                       // additional data is authenticated
    return true;
}

static bool EncryptTo(Ctx& c_, const Bytes& d, const Str& pw, size_t chunk, Bytes& out)
{
    MemSink ms; EncryptSink es(&ms, pw, 2000);                                               // few iterations: speed (the KDF itself is tested above)
    for (size_t o = 0; o < d.size(); o += chunk) { size_t k = d.size() - o < chunk ? d.size() - o : chunk; if (!es.write(d.data() + o, k)) return false; }
    if (!es.finish()) return false;
    out = ms.data;
    return true;
}

static int DecryptFrom(const Bytes& enc, const Str& pw, size_t readChunk, Bytes& out, bool* wrongPw = nullptr)
{
    MemSource src(enc.data(), enc.size()); DecryptSource ds(&src, pw);
    out.clear(); Bytes buf; buf.resize(readChunk);
    for (;;) { i64 k = ds.read(buf.data(), readChunk); if (k < 0) { if (wrongPw) *wrongPw = ds.wrongPassword(); return -1; } if (k == 0) return 0; size_t o = out.size(); out.resize(o + (size_t)k); memcpy(out.data() + o, buf.data(), (size_t)k); }
}

VM2B_TEST("core", encrypt_roundtrip_and_tamper)
{
    size_t sizes[] = { 0, 1, 15, 1048575, 1048576, 1048577, 3000000 };
    for (int i = 0; i < 6; ++i) {
        size_t n = sizes[c_.rnd(7)];
        Bytes d = RandomData(c_, n, 1 + (int)c_.rnd(3)), enc, out;
        Str pw = Str::fmt("pa$$w0rd-%u-\xd0\xbf\xd0\xb0\xd1\x80\xd0\xbe\xd0\xbb\xd1\x8c", c_.rnd(100000));
        T_CHECK(EncryptTo(c_, d, pw, c_.coin() ? 1 + c_.rnd(300000) : (size_t)1 << 21, enc));
        T_CHECK(enc.size() == 32 + 4 * ((n + 1048575) / 1048576 + (n == 0 ? 1 : 0)) + n + 16 * ((n + 1048575) / 1048576 + (n == 0 ? 1 : 0)));
        T_CHECK(DecryptFrom(enc, pw, c_.coin() ? 777 : 100000, out) == 0);
        T_CHECK(out.size() == d.size() && (d.empty() || memcmp(out.data(), d.data(), d.size()) == 0));
        bool wrong = false; Bytes o2;
        T_CHECK(DecryptFrom(enc, pw + "x", 4096, o2, &wrong) == -1); T_CHECK(wrong);        // wrong password is reported as such
        // any damage is detected: a flipped bit anywhere after the magic, a cut-off, a removed or appended block
        for (int t = 0; t < 12; ++t) {
            Bytes x = enc;
            switch (t % 4) {
            case 0: x[8 + c_.rnd((u32)x.size() - 8)] ^= (u8)(1 << c_.rnd(8)); break;
            case 1: x.resize(1 + c_.rnd((u32)x.size() - 1)); break;
            case 2: x.resize(x.size() - 1); break;
            case 3: { x.resize(x.size() + 20); c_.fill(x.data() + enc.size(), 20); break; }
            }
            Bytes o3;
            int rc = DecryptFrom(x, pw, 4096, o3);
            T_CHECK(rc == -1);
        }
    }
    // blocks swapped / dropped: needs more than one block
    Bytes d = RandomData(c_, 3 * 1048576 + 5, 1), enc;
    T_CHECK(EncryptTo(c_, d, "pw", 1 << 20, enc));
    size_t o = 32, starts[8], nb = 0; while (o < enc.size() && nb < 8) { starts[nb++] = o; u32 len = (u32)enc[o] << 24 | (u32)enc[o + 1] << 16 | (u32)enc[o + 2] << 8 | enc[o + 3]; o += 4 + len; }
    T_CHECK(nb == 4);
    Bytes swapped(enc), dropped; swapped.resize(0);
    for (size_t i = 0; i < 32; ++i) swapped.push_back(enc[i]);
    size_t order[4] = { 1, 0, 2, 3 };
    for (size_t b = 0; b < 4; ++b) { size_t s = starts[order[b]], e = order[b] + 1 < nb ? starts[order[b] + 1] : enc.size(); for (size_t i = s; i < e; ++i) swapped.push_back(enc[i]); }
    Bytes oo; T_CHECK(DecryptFrom(swapped, "pw", 4096, oo) == -1);
    for (size_t i = 0; i < starts[1]; ++i) dropped.push_back(enc[i]);
    for (size_t i = starts[2]; i < enc.size(); ++i) dropped.push_back(enc[i]);
    T_CHECK(DecryptFrom(dropped, "pw", 4096, oo) == -1);
    Bytes lastCut(enc); lastCut.resize(starts[3]); T_CHECK(DecryptFrom(lastCut, "pw", 4096, oo) == -1);        // the last block removed
    return true;
}

VM2B_TEST("core", hash_passthrough)
{
    Bytes d = RandomData(c_, 500000, 3);
    MemSink ms; HashSink hs(&ms);
    for (size_t o = 0; o < d.size(); o += 12345) hs.write(d.data() + o, d.size() - o < 12345 ? d.size() - o : 12345);
    T_CHECK(hs.finish());
    u8 h[32]; Sha256(d.data(), d.size(), h);
    T_CHECK(hs.hex() == HexEncode(h, 32)); T_CHECK(hs.size() == d.size()); T_CHECK(ms.data.size() == d.size());
    MemSource src(d.data(), d.size()); HashSource hr(&src); hr.drain();
    T_CHECK(hr.hex() == HexEncode(h, 32)); T_CHECK(hr.size() == d.size());
    return true;
}

} // namespace vm2b
