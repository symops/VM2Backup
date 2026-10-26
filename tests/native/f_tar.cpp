// libFuzzer harness (coverage-guided, loop A2). Build and run:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined f_tar.cpp ../../src/tar.cpp ../../src/base.cpp -o f_tar && ./f_tar corpus_dir -max_total_time=600
#include "../../src/base.h"
#include "../../src/tar.h"
using namespace vm2b;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    MemSource src(d, n); TarReader r(&src); TarEntry e; int rc; int k = 0; static u8 buf[4096];
    while ((rc = r.next(e)) == 1 && k++ < 200) {
        Str o; SafeArchivePath(e.path, o);
        u64 tot = 0; for (;;) { i64 q = r.read(buf, sizeof buf); if (q <= 0) break; tot += q; if (tot > (1u << 22)) break; }
    }
    return 0;
}
