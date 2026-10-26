#include "tar.h"

namespace vm2b {

static void Octal(char* dst, int width, u64 v)       // width includes the terminating NUL
{
    for (int i = width - 2; i >= 0; --i) { dst[i] = (char)('0' + (v & 7)); v >>= 3; }
    dst[width - 1] = 0;
}

static u64 ParseOctal(const char* p, int n)
{
    // GNU base-256 for huge values (bit 7 of the first byte)
    if ((u8)p[0] & 0x80) { u64 v = (u8)p[0] & 0x7F; for (int i = 1; i < n; ++i) v = (v << 8) | (u8)p[i]; return v; }
    u64 v = 0; int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = (v << 3) | (u64)(p[i] - '0');
    return v;
}

static bool Ascii(const Str& s) { for (size_t i = 0; i < s.size(); ++i) if ((u8)s[i] >= 0x80 || (u8)s[i] < 0x20) return false; return true; }

bool TarWriter::header(const Str& path, u64 size, i64 mtime, u32 mode, char type)
{
    Str name = path;
    bool needPax = name.size() > 99 || !Ascii(name) || size > 077777777777ull || (mtime < 0);
    if (needPax) {
        // PAX extended header: "<len> key=value\n" records
        Str rec;
        auto add = [&](const char* k, const Str& v) {
            Str body = Str(k) + "=" + v + "\n";
            size_t base = body.size() + 1;                              // the body and the space; the length counts its own digits
            size_t len = base + 1;
            for (;;) { size_t d = Str::fmt("%zu", len).size(); if (base + d == len) break; len = base + d; }
            rec += Str::fmt("%zu ", len); rec += body;
        };
        add("path", name);
        if (size > 077777777777ull) add("size", Str::fmt("%llu", (unsigned long long)size));
        if (mtime < 0) add("mtime", Str::fmt("%lld", (long long)mtime));
        char h[512]; memset(h, 0, 512);
        snprintf(h, 100, "PaxHeaders/%.80s", "vm2b");
        Octal(h + 100, 8, 0644); Octal(h + 108, 8, 0); Octal(h + 116, 8, 0);
        Octal(h + 124, 12, rec.size()); Octal(h + 136, 12, mtime < 0 ? 0 : (u64)mtime);
        h[156] = 'x'; memcpy(h + 257, "ustar", 5); memcpy(h + 263, "00", 2);
        memset(h + 148, ' ', 8);
        u32 sum = 0; for (int i = 0; i < 512; ++i) sum += (u8)h[i];
        snprintf(h + 148, 8, "%06o", sum); h[154] = 0; h[155] = ' ';
        if (!put(h, 512)) return false;
        size_t padded = (rec.size() + 511) & ~(size_t)511;
        if (!put(rec.data(), rec.size())) return false;
        if (padded > rec.size()) { char z[512] = {0}; if (!put(z, padded - rec.size())) return false; }
    }
    char h[512]; memset(h, 0, 512);
    // the visible name: the head of the path when it fits, otherwise a stub (the PAX record carries the real one)
    size_t n = name.size() > 99 ? 99 : name.size();
    if (!Ascii(name)) { memcpy(h, "vm2b-name", 9); } else memcpy(h, name.data(), n);
    Octal(h + 100, 8, mode); Octal(h + 108, 8, 0); Octal(h + 116, 8, 0);
    Octal(h + 124, 12, size > 077777777777ull ? 0 : size);
    Octal(h + 136, 12, mtime < 0 ? 0 : (u64)mtime);
    h[156] = type; memcpy(h + 257, "ustar", 5); memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    u32 sum = 0; for (int i = 0; i < 512; ++i) sum += (u8)h[i];
    snprintf(h + 148, 8, "%06o", sum); h[154] = 0; h[155] = ' ';
    return put(h, 512);
}

bool TarWriter::put(const void* p, size_t n)
{
    u64 end = m_pos + n;
    if (end <= m_skip) { m_pos = end; return true; }               // all of it lies in the part that is already stored
    size_t off = m_pos < m_skip ? (size_t)(m_skip - m_pos) : 0;
    m_pos = end;
    if (!m_out->write((const u8*)p + off, n - off)) { m_err = m_out->err; return false; }
    return true;
}

bool TarWriter::skip_data(u64 n)
{
    if (n > skippable()) { m_err = "tar: skipping more than allowed"; return false; }
    m_left -= n; m_pos += n;
    return true;
}

bool TarWriter::begin_file(const Str& path, u64 size, i64 mtime, u32 mode)
{
    if (m_left || m_pad) { m_err = "tar: previous entry not finished"; return false; }
    if (!header(path, size, mtime, mode, '0')) return false;
    m_left = size; m_pad = (size_t)((512 - (size & 511)) & 511);
    return true;
}

bool TarWriter::write(const void* p, size_t n)
{
    if (n > m_left) { m_err = "tar: more data than announced"; return false; }
    m_left -= n;
    return put(p, n);
}

bool TarWriter::end_file()
{
    if (m_left) { m_err = Str::fmt("tar: the file ended %llu bytes short", (unsigned long long)m_left); return false; }
    if (m_pad) { char z[512] = {0}; bool ok = put(z, m_pad); m_pad = 0; return ok; }
    return true;
}

bool TarWriter::add_bytes(const Str& path, const void* p, size_t n, i64 mtime)
{
    return begin_file(path, n, mtime) && write(p, n) && end_file();
}

bool TarWriter::add_dir(const Str& path, i64 mtime)
{
    Str p = path; if (!p.ends_with("/")) p += '/';
    return header(p, 0, mtime, 0755, '5');
}

bool TarWriter::finish()
{
    char z[1024] = {0};
    return put(z, 1024);
}

// ---- reader ----------------------------------------------------------------------------------------------------
bool TarReader::skip(u64 n)
{
    u8 buf[8192];
    while (n) {
        size_t k = n < sizeof(buf) ? (size_t)n : sizeof(buf);
        i64 r = ReadFull(*m_in, buf, k);
        if (r < 0) { m_err = m_in->err; return false; }
        if ((size_t)r != k) { m_err = "tar: unexpected end of archive"; return false; }
        n -= k;
    }
    return true;
}

bool TarReader::fillBlock(u8* b, bool& eof)
{
    eof = false;
    i64 r = ReadFull(*m_in, b, 512);
    if (r < 0) { m_err = m_in->err; return false; }
    if (r == 0) { eof = true; return true; }
    if (r != 512) { m_err = "tar: truncated header"; return false; }
    return true;
}

static bool AllZero(const u8* b) { for (int i = 0; i < 512; ++i) if (b[i]) return false; return true; }

int TarReader::next(TarEntry& e)
{
    if (m_end) return 0;
    if (m_left + m_pad && !skip(m_left + m_pad)) return -1;
    m_left = m_pad = 0; m_inEntry = false;
    for (;;) {
        u8 h[512]; bool eof;
        if (!fillBlock(h, eof)) return -1;
        if (eof) { m_end = true; return 0; }                       // tolerated: an archive without the end blocks
        if (AllZero(h)) { m_end = true; return 0; }                // the end marker
        u32 sum = 0; for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (sum != ParseOctal((const char*)h + 148, 8)) { m_err = "tar: bad header checksum"; return -1; }
        char type = (char)h[156];
        u64 size = ParseOctal((const char*)h + 124, 12);
        if (type == 'x' || type == 'g' || type == 'L') {
            if (size > (1u << 20)) { m_err = "tar: extended header too large"; return -1; }
            Bytes buf; buf.resize((size_t)size);
            if (size) { i64 r = ReadFull(*m_in, buf.data(), (size_t)size); if (r < 0) { m_err = m_in->err; return -1; } if ((u64)r != size) { m_err = "tar: truncated"; return -1; } }
            if (!skip((512 - (size & 511)) & 511)) return -1;
            if (type == 'L') { size_t n = buf.size(); while (n && buf[n - 1] == 0) --n; m_gnuName = Str((const char*)buf.data(), n); continue; }
            if (type == 'g') continue;
            size_t pos = 0;
            while (pos < buf.size()) {
                size_t sp = pos; while (sp < buf.size() && buf[sp] != ' ') ++sp;
                if (sp >= buf.size()) break;
                u64 len = 0; for (size_t i = pos; i < sp; ++i) { if (buf[i] < '0' || buf[i] > '9') { len = 0; break; } len = len * 10 + (buf[i] - '0'); }
                if (len < (sp - pos) + 3 || pos + len > buf.size()) { m_err = "tar: bad PAX record"; return -1; }
                Str rec((const char*)buf.data() + sp + 1, (size_t)(len - (sp - pos) - 2));      // without the trailing newline
                int eq = rec.find('=');
                if (eq > 0) {
                    Str k = rec.substr(0, (size_t)eq), v = rec.substr((size_t)eq + 1);
                    if (k == "path") m_paxPath = v;
                    else if (k == "size") { m_havePaxSize = true; m_paxSize = strtoull(v.c_str(), nullptr, 10); }
                    else if (k == "mtime") { m_havePaxMtime = true; m_paxMtime = (i64)strtod(v.c_str(), nullptr); }
                }
                pos += (size_t)len;
            }
            continue;
        }
        if (m_havePaxSize) size = m_paxSize;
        Str name;
        if (!m_paxPath.empty()) name = m_paxPath;
        else if (!m_gnuName.empty()) name = m_gnuName;
        else {
            size_t n = 0; while (n < 100 && h[n]) ++n;
            Str base((const char*)h, n);
            size_t pn = 0; while (pn < 155 && h[345 + pn]) ++pn;
            name = pn ? Str((const char*)h + 345, pn) + "/" + base : base;
        }
        e.path = name; e.size = size; e.mode = (u32)ParseOctal((const char*)h + 100, 8);
        e.mtime = m_havePaxMtime ? m_paxMtime : (i64)ParseOctal((const char*)h + 136, 12);
        e.isDir = type == '5';
        e.skipped = !(type == '0' || type == 0 || type == '7' || type == '5');
        if (e.isDir) { while (e.path.size() > 1 && e.path.ends_with("/")) e.path = e.path.substr(0, e.path.size() - 1); }
        m_paxPath.clear(); m_gnuName.clear(); m_havePaxSize = m_havePaxMtime = false;
        m_left = e.isDir ? 0 : size;
        m_pad = e.isDir ? 0 : (size_t)((512 - (size & 511)) & 511);
        if (e.skipped) { if (!skip(m_left + m_pad)) return -1; m_left = m_pad = 0; }
        m_cur = e; m_inEntry = true;
        return 1;
    }
}

Json TarReader::saveState() const
{
    Json j = Json::object();
    j["left"] = m_left; j["pad"] = (u64)m_pad; j["end"] = m_end; j["in_entry"] = m_inEntry;
    j["pax_path"] = m_paxPath; j["gnu_name"] = m_gnuName; j["have_pax_size"] = m_havePaxSize; j["pax_size"] = m_paxSize;
    j["have_pax_mtime"] = m_havePaxMtime; j["pax_mtime"] = m_paxMtime;
    Json c = Json::object(); c["path"] = m_cur.path; c["size"] = m_cur.size; c["mtime"] = m_cur.mtime; c["mode"] = m_cur.mode; c["dir"] = m_cur.isDir; c["skipped"] = m_cur.skipped;
    j["cur"] = c;
    return j;
}

bool TarReader::loadState(const Json& j)
{
    if (!j.is_object()) return false;
    m_left = (u64)j.get("left").integer(); m_pad = (size_t)j.get("pad").integer(); m_end = j.get("end").boolean(); m_inEntry = j.get("in_entry").boolean();
    if (m_pad > 511) return false;
    m_paxPath = j.get("pax_path").str(); m_gnuName = j.get("gnu_name").str(); m_havePaxSize = j.get("have_pax_size").boolean(); m_paxSize = (u64)j.get("pax_size").integer();
    m_havePaxMtime = j.get("have_pax_mtime").boolean(); m_paxMtime = j.get("pax_mtime").integer();
    const Json& c = j.get("cur");
    m_cur.path = c.get("path").str(); m_cur.size = (u64)c.get("size").integer(); m_cur.mtime = c.get("mtime").integer(); m_cur.mode = (u32)c.get("mode").integer();
    m_cur.isDir = c.get("dir").boolean(); m_cur.skipped = c.get("skipped").boolean();
    return true;
}

i64 TarReader::read(void* p, size_t n)
{
    if (!m_left) return 0;
    size_t k = (u64)n < m_left ? n : (size_t)m_left;
    i64 r = m_in->read(p, k);
    if (r < 0) { m_err = m_in->err; return -1; }
    if (r == 0) { m_err = "tar: unexpected end of archive"; return -1; }
    m_left -= (u64)r;
    return r;
}

// Windows treats "CON", "PRN", "AUX", "NUL", "COM1-9", "LPT1-9" (also with an extension: "nul.txt") as devices, in any case.
static bool IsReservedDeviceName(const Str& comp)
{
    size_t n = 0; while (n < comp.size() && comp[n] != '.') ++n;
    while (n > 0 && comp[n - 1] == ' ') --n;
    if (n != 3 && n != 4) return false;
    char b[5]; for (size_t i = 0; i < n; ++i) { char c = comp[i]; b[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; } b[n] = 0;
    if (n == 3) return !strcmp(b, "CON") || !strcmp(b, "PRN") || !strcmp(b, "AUX") || !strcmp(b, "NUL");
    return (!strncmp(b, "COM", 3) || !strncmp(b, "LPT", 3)) && b[3] >= '0' && b[3] <= '9';
}

bool SafeArchivePath(const Str& path, Str& out)
{
    out.clear();
    if (path.empty() || path.size() > 1000 || !Utf8Valid(path.data(), path.size())) return false;
    size_t i = 0;
    int parts = 0;
    while (i <= path.size()) {
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        Str comp = path.substr(i, j - i);
        if (comp.empty() && i == 0) return false;                        // absolute
        if (!comp.empty() && comp != ".") {
            if (comp == "..") return false;
            for (size_t k = 0; k < comp.size(); ++k) { u8 c = (u8)comp[k]; if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false; }
            if (comp[comp.size() - 1] == '.' || comp[comp.size() - 1] == ' ') return false;   // Windows strips these: a different file
            if (IsReservedDeviceName(comp)) return false;                                     // CON, NUL, COM1 ...: the file would be a device, the data lost
            if (!out.empty()) out += '/';
            out += comp; ++parts;
        }
        i = j + 1;
    }
    return parts > 0;
}

} // namespace vm2b
