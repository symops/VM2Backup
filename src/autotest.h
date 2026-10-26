// VM2Backup — the autotest framework (see vm2bautotest.cpp): a registry of tests, a seeded random generator, helpers.
#pragma once
#include "base.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

struct Ctx {
    u64 seed, state;
    bool keep, failed, skipped;
    Str skipReason, name;
    Json servers;                 // addresses of the test servers (--servers), null when not given
    explicit Ctx(u64 s, bool k) : seed(s), state(s ? s : 88172645463325252ull), keep(k), failed(false), skipped(false) {}
    void begin(const char* n) { name = n; failed = false; skipped = false; skipReason.clear(); state = seed * 0x9E3779B97F4A7C15ull + Hash(n); if (!state) state = 1; }
    static u64 Hash(const char* s) { u64 h = 1469598103934665603ull; while (*s) { h ^= (u8)*s++; h *= 1099511628211ull; } return h; }
    // xorshift64*: one stream per test, derived from the seed and the test name, so --only does not change the numbers
    u64 next() { state ^= state >> 12; state ^= state << 25; state ^= state >> 27; return state * 2685821657736338717ull; }
    u32 rnd(u32 n) { return n ? (u32)(next() % n) : 0; }                                  // 0..n-1
    int range(int lo, int hi) { return lo + (int)rnd((u32)(hi - lo + 1)); }              // lo..hi
    bool coin(int percent = 50) { return (int)rnd(100) < percent; }
    void fill(void* p, size_t n) { u8* d = (u8*)p; for (size_t i = 0; i < n; ++i) d[i] = (u8)(next() >> 24); }
    void check(bool ok, const char* expr, const char* file, int line) {
        if (!ok) { failed = true; printf("      check failed: %s  (%s:%d)\n", expr, file, line); }
    }
    void note(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("      "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
    void skip(const char* why) { skipped = true; skipReason = why; }
};

struct TestInfo { const char* name; const char* group; bool (*fn)(Ctx&); };
Vec<TestInfo>& Registry();
struct Reg { Reg(const char* name, const char* group, bool (*fn)(Ctx&)); };

// A hook that runs after EVERY test of a group, whatever its result (e.g. the ESXi tests: delete every test VM on every mock server).
struct Teardown { const char* group; void (*fn)(Ctx&); };
Vec<Teardown>& Teardowns();
struct RegTeardown { RegTeardown(const char* group, void (*fn)(Ctx&)); };

#define T_CHECK(c) c_.check((c), #c, __FILE__, __LINE__)
#define VM2B_TEST(group, name) \
    static bool test_##name(Ctx& c_); \
    static ::vm2b::Reg reg_##name(#name, group, test_##name); \
    static bool test_##name(Ctx& c_)

// CRC-32 (the zlib / xz one) for the tests: Crc32(0, ...) starts, the result can be fed back to continue.
inline u32 Crc32(u32 crc, const void* data, size_t n)
{
    static u32 tab[256]; static bool init = false;
    if (!init) { for (u32 i = 0; i < 256; ++i) { u32 c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = true; }
    const u8* p = (const u8*)data; u32 c = ~crc;
    for (size_t i = 0; i < n; ++i) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return ~c;
}

} // namespace vm2b
