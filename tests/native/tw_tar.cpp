// Differential helper for tar_diff.py (loop A2): writes random archives with TarWriter and lists archives with TarReader.
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined tw_tar.cpp ../../src/tar.cpp ../../src/base.cpp -lcrypto -o tw   (then: python3 -I tar_diff.py 1 500, in the folder of ./tw)
// tw write <seed> <out.tar> <manifest.tsv>  : random names/sizes/mtimes through TarWriter
// tw read <in.tar> <out.tsv>                : list entries as seen by TarReader (path, size, mtime, isDir, skipped, sha256 of data)
#include <initializer_list>
#include "../../src/tar.h"
#include <openssl/sha.h>
using namespace vm2b;
static u64 rs;
static u32 rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static Str hexsha(const Bytes& b) { u8 h[32]; SHA256(b.data(), b.size(), h); return HexEncode(h, 32); }
static bool readAll(const char* p, Bytes& b) { FILE* f = fopen(p, "rb"); if (!f) return false; u8 buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) for (size_t i = 0; i < n; ++i) b.push_back(buf[i]); fclose(f); return true; }
int main(int argc, char** argv) {
    Str mode = argv[1];
    if (mode == "write") {
        rs = strtoull(argv[2], nullptr, 10) * 0x9E3779B97F4A7C15ull | 1; for (int i = 0; i < 8; ++i) rnd();
        MemSink ms; TarWriter w(&ms); FILE* mf = fopen(argv[4], "wb");
        static const char* alpha[] = {"a","b","Z","0","9"," ","-","_",".","\xd0\xb4","\xd0\xb8","\xe6\x97\xa5","\xf0\x9f\x94\x91","\xc3\xa9","(",")","~","+","=",",","#","%","@","!","$","&"};
        int n = 1 + rnd() % 40;
        for (int e = 0; e < n; ++e) {
            Str name; int parts = 1 + rnd() % 6;
            int target = (rnd() % 3 == 0) ? 90 + (int)(rnd() % 30) : (rnd() % 4 == 0 ? 140 + (int)(rnd() % 80) : 1 + (int)(rnd() % 40));
            for (int p = 0; p < parts; ++p) {
                if (p) name += '/';
                int len = 1 + (int)(rnd() % (unsigned)(target / parts + 1));
                for (int i = 0; i < len; ++i) { const char* c = alpha[rnd() % (sizeof alpha / sizeof *alpha)]; name += c; }
                if (name.size() && (name[name.size() - 1] == ' ' || name[name.size() - 1] == '.')) name += 'x';
            }
            name = Str("vm/") + name + Str::fmt("-%d", e);
            bool dir = rnd() % 6 == 0; i64 mt = (rnd() % 5 == 0) ? (i64)(rnd() % 0x7fffffff) * 3 + 1ll : (i64)(1000000000 + rnd() % 900000000);
            if (dir) { w.add_dir(name, mt); fprintf(mf, "%s\t0\t%lld\t1\t-\n", name.c_str(), (long long)mt); }
            else {
                size_t sz = rnd() % 5 == 0 ? 0 : rnd() % 4 == 0 ? 500 + rnd() % 30 : rnd() % 2000; Bytes b; b.resize(sz); for (size_t i = 0; i < sz; ++i) b[i] = (u8)rnd();
                if (!w.add_bytes(name, b.data(), b.size(), mt)) { printf("add_bytes failed %s\n", name.c_str()); return 1; }
                fprintf(mf, "%s\t%zu\t%lld\t0\t%s\n", name.c_str(), sz, (long long)mt, hexsha(b).c_str());
            }
        }
        w.finish(); fclose(mf);
        FILE* f = fopen(argv[3], "wb"); fwrite(ms.data.data(), 1, ms.data.size(), f); fclose(f);
        return 0;
    }
    if (mode == "read") {
        Bytes b; if (!readAll(argv[2], b)) return 2;
        MemSource src(b.data(), b.size()); TarReader r(&src); TarEntry e; FILE* o = fopen(argv[3], "wb"); int rc;
        while ((rc = r.next(e)) == 1) {
            Bytes d; u8 buf[777]; for (;;) { i64 k = r.read(buf, sizeof buf); if (k <= 0) break; for (i64 i = 0; i < k; ++i) d.push_back(buf[i]); }
            fprintf(o, "%s\t%llu\t%lld\t%d\t%s\t%d\n", e.path.c_str(), (unsigned long long)e.size, (long long)e.mtime, e.isDir ? 1 : 0, e.isDir || e.skipped ? "-" : hexsha(d).c_str(), e.skipped ? 1 : 0);
        }
        fprintf(o, "END\t%d\t%s\n", rc, r.error().c_str()); fclose(o); return 0;
    }
}
