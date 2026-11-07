// VM2Backup — the one compressor of a backup: xz (LZMA2 in .xz, via liblzma), levels 0..9 = the real xz presets, default 7. Blocks are compressed in
// parallel (see xz.h). There is no other codec: gzip and "stored" were removed; a manifest that names another one is refused.
#pragma once
#include "stream.h"

namespace vm2b {

enum { kCodecMinLevel = 0, kCodecMaxLevel = 9, kCodecDefaultLevel = 7 };

struct CodecOptions {
    int level;                     // 0..9 (0 = `xz -0`, the fastest, a real level); the default is 7; anything else is an error of the compressor
    int threads;                   // 0 = automatic (one per processor, bounded by the memory budget in xz.cpp)
    CodecOptions() : level(kCodecDefaultLevel), threads(0) {}
    explicit CodecOptions(int l, int t = 0) : level(l), threads(t) {}
};

const char* CodecName();                           // "xz"
const char* CodecExt();                            // ".xz"
bool CodecNameOk(const char* name);                // true only for "xz"
bool CodecLevelOk(i64 level);                      // 0..9
extern const char* const kCodecLevelError;         // "the compression level is 0..9": the one text of every refusal (settings, tasks, the compressor)
int  CodecEffectiveLevel(const CodecOptions& o);   // the level that goes into the manifest (= o.level)

// The compressor sink in front of `out` (not owned; the returned sink is owned by the caller and must be deleted
// before `out`). finish() on it finishes `out` too.
Sink* CodecMakeSink(const CodecOptions& o, Sink* out);
// The decompressor in front of `in` (not owned).
Source* CodecMakeSource(Source* in);

} // namespace vm2b
