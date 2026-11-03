// VM2Backup — xz (LZMA2 in the .xz container) on top of liblzma, the library of xz-utils (static, built by tools/deps.sh into deps/out/<arch>).
// The encoder is lzma_stream_encoder_mt: the data is cut into .xz blocks of a FIXED size per level (never derived from the thread count),
// every block is compressed independently by a worker thread, so the output is IDENTICAL for any thread count. Check: CRC64. The decoder
// (lzma_stream_decoder, concatenated streams) reads any ordinary .xz (CRC32/CRC64/SHA-256/none, BCJ/delta filters, several blocks and
// streams). `xz -d`, 7-Zip and Python lzma read what MakeXzSink writes.
//
// Every block chooses its own filter chain from its own bytes: executable code (x86 CALL / JMP) gets the BCJ filter in front of LZMA2
// (3-5% smaller), everything else plain LZMA2; the decision never depends on the thread count or timing, so the output stays identical.
// Levels 0..9 are the REAL xz presets (lzma_lzma_preset without "extreme": `xz -0` .. `xz -9`; dictionaries 0: 256 KiB, 1: 1, 2: 2, 3: 4, 4: 4, 5: 8, 6: 8, 7: 16, 8: 32, 9: 64 MiB).
// Block size = 3 x dictionary, at least 1 MiB (like xz -T); it depends on the level only. Level 5 is the default of the program. Memory per compressing thread: XzLevelInfo().
// threads = 0 picks one per processor but never more than fit in about 2.5 GiB (700 MiB in a 32-bit process, which also shrinks the
// dictionary of presets 8 / 9 until one thread fits in ~900 MiB: the stream stays readable everywhere). The decoder refuses a stream that needs more than 1.5 GiB.
#pragma once
#include "stream.h"

namespace vm2b {

// `out` is not owned; finish() writes the end of the .xz stream and calls out->finish(); abort() calls out->abort().
// The sink is cheap to create (the per-thread encoders are made lazily); it joins its threads in finish() / the destructor.
Sink* MakeXzSink(Sink* out, int level /*0..9, anything else is an error of the first write*/, int threads /*0 = one per processor*/);
// Streaming decoder; `in` is not owned.
Source* MakeXzSource(Source* in);

// The per-block decision for the x86 BCJ filter (see xz.cpp): true = this block is compressed with [x86 BCJ + LZMA2], false = [LZMA2].
// Public only for the tests and the calibration tools; the numbers it fills in are what the decision was based on.
struct XzBcjStats { u64 cand; u64 per10000; u64 plain, bcj; bool trial; bool use; };
bool XzChooseBcj(const u8* p, size_t n, XzBcjStats* st);

// What a level means (the 64-bit process; for the interface table and the tests): the preset's parameters as liblzma sets them, our block size, the memory of one compressing thread.
struct XzLevelParams { u32 dictBytes; u64 blockBytes, memPerThread; u32 lc, lp, pb, niceLen, depth; int mode, mf; };
bool XzLevelInfo(int level, XzLevelParams& p);       // false: level not in 0..9

} // namespace vm2b
