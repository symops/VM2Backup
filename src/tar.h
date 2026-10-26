// VM2Backup — tar archives (POSIX ustar + PAX extended headers for long / non-ASCII names and files over 8 GB).
// Python's tarfile and GNU tar read what TarWriter writes, and the reader understands what they write.
#pragma once
#include "stream.h"

namespace vm2b {

class TarWriter {
public:
    explicit TarWriter(Sink* out) : m_out(out), m_left(0), m_pad(0), m_pos(0), m_skip(0) {}
    // Resuming: the first `n` bytes of the tar stream already live in finished volumes and are not forwarded. The writer still
    // produces them (headers are cheap), but a source can avoid READING the data that falls into the skipped part:
    // after begin_file() it asks skippable() — the number of leading data bytes of this file that need not be supplied —
    // seeks past them and calls skip_data(). Supplying them anyway through write() is correct too (just slower).
    void set_skip(u64 n) { m_skip = n; }
    u64  position() const { return m_pos; }                     // bytes of the tar stream produced so far (skipped ones included)
    u64  skippable() const { return m_skip > m_pos ? (m_skip - m_pos < m_left ? m_skip - m_pos : m_left) : 0; }
    bool skip_data(u64 n);
    bool begin_file(const Str& path, u64 size, i64 mtime, u32 mode = 0644);
    bool write(const void* p, size_t n);                        // exactly `size` bytes in total between begin_file / end_file
    bool end_file();                                            // pads to 512; fails when fewer bytes were written
    bool add_bytes(const Str& path, const void* p, size_t n, i64 mtime);
    bool add_dir(const Str& path, i64 mtime);
    bool finish();                                              // two zero blocks (does not finish the sink)
    const Str& error() const { return m_err; }
private:
    bool header(const Str& path, u64 size, i64 mtime, u32 mode, char type);
    bool put(const void* p, size_t n);
    Sink* m_out; u64 m_left; size_t m_pad; u64 m_pos, m_skip; Str m_err;
};

struct TarEntry {
    Str path;                // UTF-8, '/' separated
    u64 size;
    i64 mtime;
    u32 mode;
    bool isDir;
    bool skipped;            // a kind of entry we do not restore (link, device, ...); data is skipped
};

class TarReader {
public:
    explicit TarReader(Source* in) : m_in(in), m_left(0), m_pad(0), m_end(false), m_inEntry(false), m_havePaxSize(false), m_paxSize(0), m_havePaxMtime(false), m_paxMtime(0) {}
    // 1 = an entry, 0 = end of archive, -1 = error. The data of the previous entry that was not read is skipped.
    int next(TarEntry& e);
    i64 read(void* p, size_t n);                                // data of the current entry; 0 at its end
    const Str& error() const { return m_err; }
    // Position in the stream, for a restore that continues after a pause: at a volume boundary (a multiple of 512 bytes) the
    // reader's state is saved; a new reader that is given the state and the stream from that very byte carries on.
    Json saveState() const;
    bool loadState(const Json& j);
    const TarEntry& current() const { return m_cur; }           // the entry being read (after next() or loadState())
    u64 remaining() const { return m_left; }                    // data bytes of it still to come
    bool inEntry() const { return m_inEntry; }
private:
    bool skip(u64 n);
    bool fillBlock(u8* b, bool& eof);
    Source* m_in; u64 m_left; size_t m_pad; bool m_end; Str m_err;
    TarEntry m_cur; bool m_inEntry;
    Str m_paxPath, m_gnuName; bool m_havePaxSize; u64 m_paxSize; bool m_havePaxMtime; i64 m_paxMtime;
};

// A path from an archive that may be written below a folder: relative, no "..", no drive letters / colons / backslashes /
// control characters, not empty. Returns the cleaned path in `out`.
bool SafeArchivePath(const Str& path, Str& out);

} // namespace vm2b
