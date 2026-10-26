#include <initializer_list>
#include "../../src/tar.h"
using namespace vm2b;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int fails = 0;
static u64 rs = 1234567;
static u32 rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static Bytes content(size_t n, u32 seed) { Bytes b; b.resize(n); u32 x = seed; for (size_t i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; b[i] = (u8)(x >> 16); } return b; }
int main(int argc, char** argv) {
    const char* dir = argv[2];
    if (!strcmp(argv[1], "write")) {            // our tar for Python to read: dir/our.tar + dir/names.txt
        MemSink ms; TarWriter w(&ms);
        Str names;
        const char* ns[] = {"a.txt", "dir/b.bin", "very/long/path/" "0123456789012345678901234567890123456789/0123456789012345678901234567890123456789/0123456789012345678901234567890123456789/file.vmdk",
                            "unicode/\xd0\x92\xd0\x9c-\xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80/\xe6\x97\xa5\xe6\x9c\xac.vmx", "empty", "with space/x y.txt"};
        size_t sizes[] = {0, 1, 511, 512, 513, 100000, 0, 7};
        int k = 0;
        w.add_dir("dir", 1700000000);
        for (const char* n : ns) { Bytes c = content(sizes[k], (u32)k); CHECK(w.add_bytes(n, c.data(), c.size(), 1700000000 + k)); names += Str(n) + "\t" + Str::fmt("%zu %u\n", sizes[k], k); ++k; }
        CHECK(w.begin_file("chunked.bin", 300000, 1600000000));
        Bytes c = content(300000, 99); for (size_t o = 0; o < c.size(); o += 4099) CHECK(w.write(c.data() + o, c.size() - o < 4099 ? c.size() - o : 4099));
        CHECK(w.end_file()); names += "chunked.bin\t300000 99\n";
        CHECK(w.begin_file("short", 10, 0)); CHECK(w.write("12345", 5)); CHECK(!w.end_file());      // a short file is an error
        MemSink ms2; TarWriter w2(&ms2); CHECK(w2.begin_file("x", 3, 0)); CHECK(!w2.write("12345", 5));
        TarWriter wf(&ms); (void)wf;
        // finish the good archive (the failed entry above is not part of the test archive: rebuild it cleanly)
        MemSink good; TarWriter g(&good); k = 0;
        g.add_dir("dir", 1700000000);
        for (const char* n : ns) { Bytes cc = content(sizes[k], (u32)k); g.add_bytes(n, cc.data(), cc.size(), 1700000000 + k); ++k; }
        g.begin_file("chunked.bin", 300000, 1600000000); for (size_t o = 0; o < c.size(); o += 4099) g.write(c.data() + o, c.size() - o < 4099 ? c.size() - o : 4099); g.end_file();
        CHECK(g.finish());
        Str e; char p[600]; snprintf(p, sizeof p, "%s/our.tar", dir); CHECK(WriteWholeFile(p, good.data.data(), good.data.size(), e));
        snprintf(p, sizeof p, "%s/names.txt", dir); CHECK(WriteWholeFile(p, names.data(), names.size(), e));
    } else if (!strcmp(argv[1], "read")) {      // Python's tars: dir/py_<fmt>.tar with the same files
        const char* fmts[] = {"pax", "gnu", "ustar"};
        for (const char* f : fmts) {
            char p[600]; snprintf(p, sizeof p, "%s/py_%s.tar", dir, f); Bytes t; Str e;
            if (!ReadWholeFile(p, t, e)) { printf("missing %s\n", p); fails++; continue; }
            MemSource src(t.data(), t.size()); TarReader r(&src); TarEntry en; int cnt = 0, rc;
            while ((rc = r.next(en)) == 1) {
                ++cnt; Bytes got; u8 buf[3000]; i64 k; while ((k = r.read(buf, sizeof buf)) > 0) for (i64 i = 0; i < k; ++i) got.push_back(buf[i]);
                CHECK(k == 0); CHECK(got.size() == en.size || en.isDir);
                if (!en.isDir && !en.skipped) { u32 seed = (u32)atoi(en.path.substr(en.path.rfind('_') + 1).c_str()); Bytes want = content(en.size, seed); CHECK(want.size() == got.size() && (want.size() == 0 || memcmp(want.data(), got.data(), want.size()) == 0)); }
            }
            CHECK(rc == 0); if (cnt != (strcmp(f, "ustar") == 0 ? 7 : 9)) { printf("%s: %d entries\n", f, cnt); fails++; }
        }
    } else {                                    // misc: big sizes, safe paths, damaged archives
        MemSink ms; TarWriter w(&ms); CHECK(w.begin_file("huge.img", 9ull << 30, 5));
        MemSource src(ms.data.data(), ms.data.size()); TarReader r(&src); TarEntry en; CHECK(r.next(en) == 1); CHECK(en.size == (9ull << 30) && en.path == "huge.img");
        for (size_t len : {97, 98, 99, 100, 101, 102}) {      // names around the ustar limit survive
            MemSink m2; TarWriter w2(&m2); Str nm; while (nm.size() < len) nm += (char)('a' + nm.size() % 26);
            CHECK(w2.add_bytes(nm, "x", 1, 5)); w2.finish();
            MemSource s2(m2.data.data(), m2.data.size()); TarReader r2(&s2); TarEntry e2; CHECK(r2.next(e2) == 1 && e2.path == nm);
        }
        Str o;
        CHECK(SafeArchivePath("a/b/c.txt", o) && o == "a/b/c.txt"); CHECK(SafeArchivePath("./a//b", o) && o == "a/b"); CHECK(!SafeArchivePath("/etc/passwd", o)); CHECK(!SafeArchivePath("../x", o));
        CHECK(!SafeArchivePath("a/../../x", o)); CHECK(!SafeArchivePath("C:/x", o)); CHECK(!SafeArchivePath("a\\b", o)); CHECK(!SafeArchivePath("", o)); CHECK(!SafeArchivePath("a/b.", o));
        CHECK(!SafeArchivePath("a/\x01", o)); CHECK(!SafeArchivePath("con:", o)); CHECK(SafeArchivePath("d\xc3\xa9j\xc3\xa0/vu", o));
        MemSink g; TarWriter gw(&g); for (int i = 0; i < 20; ++i) { Bytes c = content(rnd() % 5000, (u32)i); gw.add_bytes(Str::fmt("f/%d_%d", i, i), c.data(), c.size(), 1); } gw.finish();
        for (int t = 0; t < 600; ++t) {
            Bytes x = g.data; size_t pos = rnd() % x.size(); x[pos] ^= (u8)(1 << (rnd() % 8)); if (t % 4 == 0) x.resize(rnd() % x.size());
            MemSource s2(x.data(), x.size()); TarReader rr(&s2); TarEntry e2; int rc; int guard = 0;
            while ((rc = rr.next(e2)) == 1 && guard++ < 100) { u8 b[700]; while (rr.read(b, sizeof b) > 0) {} }
        }
    }
    printf(fails ? "tar FAILED %d\n" : "tar ok\n", fails); return fails != 0;
}
