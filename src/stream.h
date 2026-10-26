// VM2Backup — byte streams. A backup is a chain of sinks (tar -> xz -> encryption -> hash -> target file); a restore is the
// chain of sources the other way round. Errors are returned as false / negative and described in err.
#pragma once
#include "base.h"

namespace vm2b {

struct Sink {
    Str err;
    bool transient;               // the error may pass if tried again (connection lost, timeout): see target.h
    Sink() : transient(false) {}
    virtual ~Sink() {}
    virtual bool write(const void* p, size_t n) = 0;
    virtual bool finish() = 0;            // flushes trailers and finishes the downstream sink
    virtual void abort() {}               // the chain is dropped: remove half-written files
protected:
    bool fail(const Str& e, bool temp = false) { err = e; transient = temp; return false; }
    bool failFrom(const Sink* o) { err = o->err; transient = o->transient; return false; }   // passes the error of the next sink on
};

struct Source {
    Str err;
    bool transient;
    Source() : transient(false) {}
    virtual ~Source() {}
    // Returns the number of bytes read (0 = end of data), or -1 on an error (see err).
    virtual i64 read(void* p, size_t n) = 0;
    virtual void close() {}
protected:
    i64 fail(const Str& e, bool temp = false) { err = e; transient = temp; return -1; }
    i64 failFrom(const Source* o) { err = o->err; transient = o->transient; return -1; }
};

// Reads exactly n bytes unless the data ends first; returns the count, or -1 on error.
inline i64 ReadFull(Source& s, void* p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        i64 k = s.read((u8*)p + got, n - got);
        if (k < 0) return -1;
        if (k == 0) break;
        got += (size_t)k;
    }
    return (i64)got;
}

struct MemSink : Sink {
    Bytes data;
    bool write(const void* p, size_t n) override { size_t o = data.size(); data.resize(o + n); if (n) memcpy(data.data() + o, p, n); return true; }
    bool finish() override { return true; }
};

struct MemSource : Source {
    const u8* p; size_t n, pos;
    MemSource(const void* d, size_t len) : p((const u8*)d), n(len), pos(0) {}
    i64 read(void* out, size_t k) override { size_t r = n - pos < k ? n - pos : k; if (r) memcpy(out, p + pos, r); pos += r; return (i64)r; }
};

// Counts and forwards (progress, sizes).
struct CountSink : Sink {
    Sink* out; u64 bytes;
    explicit CountSink(Sink* o) : out(o), bytes(0) {}
    bool write(const void* p, size_t n) override { bytes += n; if (!out->write(p, n)) return failFrom(out); return true; }
    bool finish() override { if (!out->finish()) return failFrom(out); return true; }
    void abort() override { out->abort(); }
};

} // namespace vm2b
