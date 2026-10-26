#include "codec.h"
#include "xz.h"

namespace vm2b {

const char* CodecName() { return "xz"; }
const char* CodecExt() { return ".xz"; }
bool CodecNameOk(const char* name) { return name && !strcmp(name, "xz"); }

int CodecClampLevel(i64 level) { return level <= 0 ? kCodecDefaultLevel : level > kCodecMaxLevel ? kCodecMaxLevel : (int)level; }
int CodecEffectiveLevel(const CodecOptions& o) { return CodecClampLevel(o.level); }

Sink* CodecMakeSink(const CodecOptions& o, Sink* out) { return MakeXzSink(out, CodecEffectiveLevel(o), o.threads < 0 ? 0 : o.threads); }
Source* CodecMakeSource(Source* in) { return MakeXzSource(in); }

} // namespace vm2b
