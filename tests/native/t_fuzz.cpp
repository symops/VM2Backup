// Mutation fuzzer of every parser reachable from files / the network that builds natively: Json, tar reader, xz decoder,
// SafeArchivePath, Base64/Hex.  Built with ASan+UBSan: any crash, leak, huge allocation or hang (>5 s per case) is a failure.
//   t_fuzz <seed> <seconds>
#include <initializer_list>
#include <time.h>
#include "../../src/base.h"
#include "../../src/tar.h"
#include "../../src/xz.h"
using namespace vm2b;
static u64 rs;
static u32 rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int fails = 0;

static Bytes mutate(const Bytes& in)
{
    Bytes b = in;
    int n = 1 + (int)(rnd() % 4);
    for (int k = 0; k < n; ++k) {
        if (b.empty()) { b.push_back((u8)rnd()); continue; }
        size_t p = rnd() % b.size();
        switch (rnd() % 9) {
        case 0: b[p] ^= (u8)(1 << (rnd() % 8)); break;
        case 1: b[p] = (u8)rnd(); break;
        case 2: b.resize(p); break;                                                    // truncate
        case 3: { size_t l = 1 + rnd() % 16; for (size_t i = 0; i < l && p + i < b.size(); ++i) b[p + i] = (rnd() & 1) ? 0xff : 0x00; } break;
        case 4: { size_t l = 1 + rnd() % 64; size_t q = rnd() % b.size(); Bytes c; for (size_t i = 0; i < l && q + i < b.size(); ++i) c.push_back(b[q + i]); for (size_t i = 0; i < c.size(); ++i) b.insert(p + i, c[i]); } break;
        case 5: b.insert(p, (u8)rnd()); break;
        case 6: b.erase(p); break;
        case 7: { u8 v[] = { 0x7f, 0x80, 0xff, 0x00, '9', '-', ' ' }; b[p] = v[rnd() % 7]; } break;
        case 8: { size_t l = rnd() % 5; for (size_t i = 0; i < l && p + i < b.size(); ++i) b[p + i] = (u8)'9'; } break;
        }
    }
    return b;
}

static void drain(Source& s, size_t chunk, u64 cap)
{
    Bytes buf; buf.resize(chunk); u64 tot = 0;
    for (;;) { i64 k = s.read(buf.data(), chunk); if (k <= 0) break; tot += (u64)k; if (tot > cap) break; }
}

static void tarCase(const Bytes& d)
{
    MemSource src(d.data(), d.size()); TarReader r(&src); TarEntry e; int rc; int n = 0;
    while ((rc = r.next(e)) == 1 && n++ < 1000) {
        Str o; SafeArchivePath(e.path, o);
        Bytes buf; buf.resize(1 + rnd() % 3000); u64 tot = 0;
        if (rnd() % 3) { for (;;) { i64 k = r.read(buf.data(), buf.size()); if (k <= 0) break; tot += (u64)k; if (tot > (1u << 22)) break; } }
    }
    TarReader r2(&src); (void)r2;
    Json j = Json::object(); TarReader r3(&src); r3.loadState(j);
}

int main(int argc, char** argv)
{
    u64 seed = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1; double secs = argc > 2 ? atof(argv[2]) : 30;
    rs = seed * 0x9E3779B97F4A7C15ull | 1; for (int i = 0; i < 10; ++i) rnd();
    printf("t_fuzz seed %llu for %.0f s\n", (unsigned long long)seed, secs);
    // seed corpus
    Vec<Bytes> tars, xzs, jsons;
    for (int v = 0; v < 6; ++v) {
        MemSink ms; TarWriter w(&ms);
        w.add_dir("vm", 1700000000);
        Bytes c; c.resize(rnd() % 3000); for (size_t i = 0; i < c.size(); ++i) c[i] = (u8)(i * 7);
        w.add_bytes("vm/a.vmdk", c.data(), c.size(), 1700000000);
        Str lp = "vm/"; for (int i = 0; i < (int)(v * 30); ++i) lp += "long_name_part_";
        w.add_bytes(lp + "/\xd0\xb4\xd0\xb8\xd1\x81\xd0\xba.vmx", c.data(), c.size() / 3, 1700000001);
        w.begin_file("vm/big", (u64)9 << 30 >> (v & 1 ? 0 : 20), 5); w.skip_data(0);
        w.finish(); tars.push_back(ms.data);
    }
    {   // a big-size entry whose body we do not need: headers only (the writer refuses a short file, so build PAX records by hand)
        MemSink ms; TarWriter w(&ms); Bytes c; c.resize(10); w.add_bytes("x", c.data(), 10, 1); w.finish(); tars.push_back(ms.data);
    }
    for (int v = 0; v < 6; ++v) {
        Bytes d; d.resize(rnd() % 100000); for (size_t i = 0; i < d.size(); ++i) d[i] = (u8)(v < 3 ? (i * 31) >> (v + 2) : rnd());
        MemSink x; Sink* xs = MakeXzSink(&x, 1 + (v % 3), 1); xs->write(d.data(), d.size()); xs->finish(); delete xs; xzs.push_back(x.data);
    }
    const char* js[] = { "{\"a\":[1,2.5,-3,true,null,\"x\\u00e9\\ud83d\\ude00\\n\"],\"b\":{\"c\":\"d\"},\"e\":9223372036854775807,\"f\":1e308,\"g\":-0.0}",
        "[[[[[[[[[[1]]]]]]]]]]", "{\"version\":3,\"vm\":\"v\",\"archive\":\"a.tar.xz\",\"segments\":[{\"size\":1,\"raw\":2,\"sha256\":\"00\"}],\"kdf_iterations\":600000}", "\"\\ud83d\\ude00\"", "123456789012345678901234567890" };
    for (const char* s : js) { Bytes b; b.resize(strlen(s)); memcpy(b.data(), s, b.size()); jsons.push_back(b); }
    double t0 = now(); u64 cases = 0, slow = 0;
    while (now() - t0 < secs) {
        int kind = (int)(rnd() % 5); double c0 = now(); size_t chunk = rnd() % 4 ? 1 + rnd() % 70000 : 1 + rnd() % 17;
        switch (kind) {
        case 0: tarCase(mutate(tars[rnd() % tars.size()])); break;
        case 1: { Bytes d = mutate(xzs[rnd() % xzs.size()]); MemSource s(d.data(), d.size()); Source* g = MakeXzSource(&s); drain(*g, chunk, (u64)1 << 28); delete g; } break;
        case 2: { Bytes d = mutate(xzs[rnd() % xzs.size()]); MemSource s(d.data(), d.size()); Source* g = MakeXzSource(&s); drain(*g, chunk, (u64)1 << 28); delete g; } break;
        case 3: { Bytes d = mutate(jsons[rnd() % jsons.size()]); Json j; Str e; if (Json::parse((const char*)d.data(), d.size(), j, e)) { Str o = j.dump(rnd() & 1); Json k; Str e2; if (!Json::parse(o, k, e2)) { printf("FAIL: re-parse of own dump\n"); ++fails; } } } break;
        case 4: { Bytes d; d.resize(rnd() % 200); for (auto& x : d) x = (u8)(rnd() % 4 ? "/.:\\ aZ0\xc3\xa9"[rnd() % 10] : rnd()); Str o; SafeArchivePath(Str((const char*)d.data(), d.size()), o); Bytes q; Base64Decode((const char*)d.data(), d.size(), q); HexDecode((const char*)d.data(), d.size(), q); Utf8Valid((const char*)d.data(), d.size()); } break;
        }
        double dt = now() - c0; if (dt > 5) { printf("SLOW case kind %d: %.1f s\n", kind, dt); ++slow; ++fails; }
        ++cases;
    }
    printf("%llu cases, %d failures\n", (unsigned long long)cases, fails);
    return fails ? 1 : 0;
}
