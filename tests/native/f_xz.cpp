// libFuzzer harness (coverage-guided, loop A2). Build and run:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined f_xz.cpp ../../src/xz.cpp ../../src/base.cpp -llzma -o f_xz && ./f_xz corpus_dir -max_total_time=600  (seeds: .xz files from python lzma / xz)
#include "../../src/base.h"
#include "../../src/xz.h"
using namespace vm2b;
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    MemSource src(d, n); Source* g = MakeXzSource(&src); static u8 buf[8192]; u64 tot = 0; size_t chunk = 1 + (n ? d[0] : 0) * 31 % sizeof buf;
    for (;;) { i64 k = g->read(buf, chunk); if (k <= 0) break; tot += k; if (tot > (1u << 27)) break; }
    delete g; return 0;
}
