// libFuzzer harness (coverage-guided, loop A2). Build and run:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined f_json.cpp ../../src/base.cpp -o f_json && ./f_json corpus_dir -max_len=3000 -max_total_time=600
#include "../../src/base.h"
using namespace vm2b;
static bool eq(const Json& a, const Json& b);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    Json j; Str e; if (!Json::parse((const char*)d, n, j, e)) return 0;
    Str o = j.dump(n & 1); Json k; Str e2;
    if (!Json::parse(o, k, e2)) { printf("reparse failed: %s\n", e2.c_str()); abort(); }
    Str o2 = k.dump(n & 1); Json k2; Str e3; if (!Json::parse(o2, k2, e3)) abort(); if (k2.dump(n & 1) != o2 && o2.find("-0") < 0 && o2.find("e") < 0) { printf("unstable %s | %s\n", o2.c_str(), k2.dump(n&1).c_str()); abort(); }
    return 0;
}
