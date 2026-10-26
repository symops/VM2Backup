// VM2Backup — xz (LZMA2 in the .xz container) on top of liblzma, the library of xz-utils (static, built by tools/deps.sh into deps/out/<arch>).
// The encoder is lzma_stream_encoder_mt: the data is cut into .xz blocks of a FIXED size per level (never derived from the thread count),
// every block is compressed independently by a worker thread, so the output is IDENTICAL for any thread count. Check: CRC64. The decoder
// (lzma_stream_decoder, concatenated streams) reads any ordinary .xz (CRC32/CRC64/SHA-256/none, BCJ/delta filters, several blocks and
// streams). `xz -d`, 7-Zip and Python lzma read what MakeXzSink writes.
//
// Every block chooses its own filter chain from its own bytes: executable code (x86 CALL / JMP) gets the BCJ filter in front of LZMA2
// (3-5% smaller), everything else plain LZMA2; the decision never depends on the thread count or timing, so the output stays identical.
// Levels 1..9 (no "extreme"): the liblzma preset of that number with our own dictionary: 1/2/4/8/16/16/32/64/64 MiB. Block size = 3 x
// dictionary (cap 192 MiB). Level 5 is the default of the program. Memory per compressing thread is about 3 x block + ~6 x dictionary;
// threads = 0 picks one per processor but never more than fit in about 2.5 GiB (700 MiB in a 32-bit process, which also shrinks the
// dictionary until one thread fits in ~900 MiB). The decoder refuses a stream that needs more than 1.5 GiB.
#pragma once
#include "stream.h"

namespace vm2b {

// `out` is not owned; finish() writes the end of the .xz stream and calls out->finish(); abort() calls out->abort().
// The sink is cheap to create (the per-thread encoders are made lazily); it joins its threads in finish() / the destructor.
Sink* MakeXzSink(Sink* out, int level /*1..9*/, int threads /*0 = one per processor*/);
// Streaming decoder; `in` is not owned.
Source* MakeXzSource(Source* in);

// The per-block decision for the x86 BCJ filter (see xz.cpp): true = this block is compressed with [x86 BCJ + LZMA2], false = [LZMA2].
// Public only for the tests and the calibration tools; the numbers it fills in are what the decision was based on.
struct XzBcjStats { u64 cand; u64 per10000; u64 plain, bcj; bool trial; bool use; };
bool XzChooseBcj(const u8* p, size_t n, XzBcjStats* st);

} // namespace vm2b
