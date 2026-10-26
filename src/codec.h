// VM2Backup — the one compressor of a backup: xz (LZMA2 in .xz, via liblzma), levels 1..9, default 5. Blocks are compressed in
// parallel (see xz.h). There is no other codec: gzip and "stored" were removed; a manifest that names another one is refused.
#pragma once
#include "stream.h"

namespace vm2b {

enum { kCodecMinLevel = 1, kCodecMaxLevel = 9, kCodecDefaultLevel = 5 };

struct CodecOptions {
    int level;                     // 1..9; 0 = the default level (5); out of range is clamped
    int threads;                   // 0 = automatic (one per processor, bounded by the memory budget in xz.cpp)
    CodecOptions() : level(0), threads(0) {}
    explicit CodecOptions(int l, int t = 0) : level(l), threads(t) {}
};

const char* CodecName();                           // "xz"
const char* CodecExt();                            // ".xz"
bool CodecNameOk(const char* name);                // true only for "xz"
int  CodecClampLevel(i64 level);                   // 0 / negative -> 5, above 9 -> 9
int  CodecEffectiveLevel(const CodecOptions& o);   // the level actually used (default applied, clamped)

// The compressor sink in front of `out` (not owned; the returned sink is owned by the caller and must be deleted
// before `out`). finish() on it finishes `out` too.
Sink* CodecMakeSink(const CodecOptions& o, Sink* out);
// The decompressor in front of `in` (not owned).
Source* CodecMakeSource(Source* in);

} // namespace vm2b
