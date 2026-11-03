#include "codec.h"
#include "xz.h"

namespace vm2b {

const char* CodecName() { return "xz"; }
const char* CodecExt() { return ".xz"; }
bool CodecNameOk(const char* name) { return name && !strcmp(name, "xz"); }

const char* const kCodecLevelError = "the compression level is 0..9";
bool CodecLevelOk(i64 level) { return level >= kCodecMinLevel && level <= kCodecMaxLevel; }
int CodecEffectiveLevel(const CodecOptions& o) { return o.level; }

Sink* CodecMakeSink(const CodecOptions& o, Sink* out) { return MakeXzSink(out, CodecEffectiveLevel(o), o.threads < 0 ? 0 : o.threads); }
Source* CodecMakeSource(Source* in) { return MakeXzSource(in); }

} // namespace vm2b
