#include "base.h"
#include <math.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace vm2b {

Str Str::vfmt(const char* f, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), f, ap);
    Str r;
    if (n < 0) { va_end(ap2); return r; }
    if ((size_t)n < sizeof(buf)) r.append(buf, (size_t)n);
    else { r.resize((size_t)n); vsnprintf(r.mut(), (size_t)n + 1, f, ap2); }
    va_end(ap2);
    return r;
}

Str Str::fmt(const char* f, ...)
{
    va_list ap; va_start(ap, f);
    Str r = vfmt(f, ap);
    va_end(ap);
    return r;
}

Str Str::replace_all(const char* from, const char* to) const
{
    size_t fl = strlen(from);
    if (!fl) return *this;
    Str r;
    size_t pos = 0;
    for (;;) {
        int i = find(from, pos);
        if (i < 0) { r.append(c_str() + pos, m_n - pos); break; }
        r.append(c_str() + pos, (size_t)i - pos);
        r.append(to);
        pos = (size_t)i + fl;
    }
    return r;
}

// ---- Json writer -----------------------------------------------------------------------------------------------
static void EscapeTo(Str& out, const Str& s)
{
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += (char)c;                        // UTF-8 passes through unchanged
        }
    }
    out += '"';
}

void Json::dump_to(Str& out, bool pretty, int depth) const
{
    auto nl = [&](int d) { if (pretty) { out += '\n'; for (int i = 0; i < d; ++i) out += "  "; } };
    switch (m_t) {
    case Null: out += "null"; break;
    case Bool: out += m_i ? "true" : "false"; break;
    case Int: out += Str::fmt("%lld", (long long)m_i); break;
    case Real:
        if (isfinite(m_d)) out += Str::fmt("%.17g", m_d); else out += "null";
        break;
    case String: EscapeTo(out, m_s); break;
    case Array:
        if (m_a.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < m_a.size(); ++i) { if (i) out += ','; nl(depth + 1); m_a[i].dump_to(out, pretty, depth + 1); }
        nl(depth); out += ']';
        break;
    case Object:
        if (m_k.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < m_k.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            EscapeTo(out, m_k[i]);
            out += pretty ? ": " : ":";
            m_v[i].dump_to(out, pretty, depth + 1);
        }
        nl(depth); out += '}';
        break;
    }
}

Str Json::dump(bool pretty) const { Str s; dump_to(s, pretty, 0); return s; }

bool Json::operator==(const Json& o) const
{
    if (m_t != o.m_t) return (is_number() && o.is_number()) ? num() == o.num() : false;
    switch (m_t) {
    case Null: return true;
    case Bool: case Int: return m_i == o.m_i;
    case Real: return m_d == o.m_d;
    case String: return m_s == o.m_s;
    case Array:
        if (m_a.size() != o.m_a.size()) return false;
        for (size_t i = 0; i < m_a.size(); ++i) if (!(m_a[i] == o.m_a[i])) return false;
        return true;
    case Object:
        if (m_k.size() != o.m_k.size()) return false;
        for (size_t i = 0; i < m_k.size(); ++i) if (!(m_v[i] == o.get(m_k[i].c_str())) || !o.has(m_k[i].c_str())) return false;
        return true;
    }
    return false;
}

// ---- Json parser (strict RFC 8259; depth-limited; a BOM is skipped) --------------------------------------------
namespace {
struct Parser {
    const char* p; const char* e; Str* err; int depth;
    bool fail(const char* msg) { if (err->empty()) *err = Str::fmt("%s (offset %d)", msg, 0); return false; }
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    static int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
    static void utf8(Str& s, u32 cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(u32& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) { int h = hex(p[i]); if (h < 0) return false; v = v * 16 + (u32)h; }
        p += 4;
        return true;
    }
    bool string(Str& out) {
        ++p;                                              // opening quote
        for (;;) {
            if (p >= e) return fail("unterminated string");
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return true;
            if (c < 0x20) return fail("control character in string");
            if (c != '\\') { out += (char)c; continue; }
            if (p >= e) return fail("bad escape");
            char x = *p++;
            switch (x) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                u32 v;
                if (!hex4(v)) return fail("bad \\u escape");
                if (v >= 0xD800 && v < 0xDC00) {
                    u32 lo;
                    if (e - p < 2 || p[0] != '\\' || p[1] != 'u') return fail("lone surrogate");
                    p += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return fail("bad surrogate pair");
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                } else if (v >= 0xDC00 && v <= 0xDFFF) return fail("lone surrogate");
                utf8(out, v);
                break; }
            default: return fail("bad escape");
            }
        }
    }
    bool value(Json& out) {
        ws();
        if (p >= e) return fail("unexpected end");
        if (++depth > 200) return fail("nesting too deep");
        bool ok = value1(out);
        --depth;
        return ok;
    }
    bool value1(Json& out) {
        char c = *p;
        if (c == '{') {
            ++p; out = Json::object(); ws();
            if (p < e && *p == '}') { ++p; return true; }
            Vec<u32> tab;                                  // from 32 members on: a hash table of the keys (index + 1), so a huge object parses in linear time
            for (;;) {
                ws();
                if (p >= e || *p != '"') return fail("object key expected");
                Str k; if (!string(k)) return false;
                ws();
                if (p >= e || *p != ':') return fail("':' expected");
                ++p;
                Json v; if (!value(v)) return false;
                if (out.size() < 32) out[k.c_str()] = v;
                else {
                    auto hash = [](const Str& s) { u32 h = 2166136261u; for (size_t i = 0; i < s.size(); ++i) { h ^= (u8)s[i]; h *= 16777619u; } return h; };
                    if (tab.empty() || (out.size() + 1) * 2 > tab.size()) {
                        size_t cap = 64; while (cap < (out.size() + 1) * 4) cap *= 2;
                        tab.clear(); tab.resize(cap);
                        for (size_t i = 0; i < out.size(); ++i) { size_t at = hash(out.key_at(i)) & (cap - 1); while (tab[at]) at = (at + 1) & (cap - 1); tab[at] = (u32)i + 1; }
                    }
                    size_t mask = tab.size() - 1, at = hash(k) & mask; bool dup = false;
                    while (tab[at]) { if (out.key_at(tab[at] - 1) == k) { out.value_at(tab[at] - 1) = v; dup = true; break; } at = (at + 1) & mask; }
                    if (!dup) { out.add_member(k) = v; tab[at] = (u32)out.size(); }
                }
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            ++p; out = Json::array(); ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                Json v; if (!value(v)) return false;
                out.push(v);
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { Str s; if (!string(s)) return false; out = Json(s); return true; }
        if (e - p >= 4 && memcmp(p, "true", 4) == 0) { p += 4; out = Json(true); return true; }
        if (e - p >= 5 && memcmp(p, "false", 5) == 0) { p += 5; out = Json(false); return true; }
        if (e - p >= 4 && memcmp(p, "null", 4) == 0) { p += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p; bool real = false;
            if (*p == '-') ++p;
            if (p >= e || *p < '0' || *p > '9') return fail("bad number");
            if (*p == '0' && p + 1 < e && p[1] >= '0' && p[1] <= '9') return fail("leading zero");
            while (p < e && *p >= '0' && *p <= '9') ++p;
            if (p < e && *p == '.') { real = true; ++p; if (p >= e || *p < '0' || *p > '9') return fail("bad number"); while (p < e && *p >= '0' && *p <= '9') ++p; }
            if (p < e && (*p == 'e' || *p == 'E')) { real = true; ++p; if (p < e && (*p == '+' || *p == '-')) ++p; if (p >= e || *p < '0' || *p > '9') return fail("bad number"); while (p < e && *p >= '0' && *p <= '9') ++p; }
            Str t(s, (size_t)(p - s));
            if (!real) {
                errno = 0; char* end; long long v = strtoll(t.c_str(), &end, 10);
                if (errno == 0) { out = Json((i64)v); return true; }
            }
            out = Json(strtod(t.c_str(), nullptr));
            return true;
        }
        return fail("unexpected character");
    }
};
} // namespace

bool Json::parse(const char* text, size_t n, Json& out, Str& err)
{
    err.clear();
    if (n >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) { text += 3; n -= 3; }
    Parser ps{text, text + n, &err, 0};
    Json v;
    if (!ps.value(v)) { err = Str::fmt("JSON: %s at offset %d", err.c_str(), (int)(ps.p - text)); return false; }
    ps.ws();
    if (ps.p != ps.e) { err = Str::fmt("JSON: extra data at offset %d", (int)(ps.p - text)); return false; }
    out = static_cast<Json&&>(v);
    return true;
}

// ---- hex / base64 ----------------------------------------------------------------------------------------------
Str HexEncode(const void* p, size_t n)
{
    static const char d[] = "0123456789abcdef";
    Str s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { u8 b = ((const u8*)p)[i]; s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

bool HexDecode(const char* s, size_t n, Bytes& out)
{
    if (n & 1) return false;
    out.clear();
    for (size_t i = 0; i < n; i += 2) {
        int a = Parser::hex(s[i]), b = Parser::hex(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((u8)(a * 16 + b));
    }
    return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

Str Base64Encode(const void* p, size_t n)
{
    const u8* d = (const u8*)p;
    Str s; s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)d[i] << 16 | (i + 1 < n ? (u32)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        s += kB64[v >> 18]; s += kB64[(v >> 12) & 63];
        s += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        s += i + 2 < n ? kB64[v & 63] : '=';
    }
    return s;
}

bool Base64Decode(const char* s, size_t n, Bytes& out)
{
    out.clear();
    u32 acc = 0; int bits = 0; size_t pad = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '=') { ++pad; continue; }
        if (c == '\r' || c == '\n' || c == ' ') continue;
        if (pad) return false;
        const char* q = (c ? (const char*)memchr(kB64, c, 64) : nullptr);
        if (!q) return false;
        acc = acc << 6 | (u32)(q - kB64); bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((u8)(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    return pad < 3;
}

bool Utf8Valid(const char* s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        u8 c = (u8)s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return false;
        u32 cp = len == 1 ? c : len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (size_t k = 1; k < len; ++k) { if (((u8)s[i + k] & 0xC0) != 0x80) return false; cp = cp << 6 | ((u8)s[i + k] & 0x3F); }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

Str FormatSize(u64 b)
{
    const char* u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = (double)b; int k = 0;
    while (v >= 1024 && k < 5) { v /= 1024; ++k; }
    return k == 0 ? Str::fmt("%llu B", (unsigned long long)b) : Str::fmt("%.1f %s", v, u[k]);
}

Str FormatDuration(double s)
{
    long long t = (long long)(s + 0.5);
    if (t < 60) return Str::fmt("%llds", t);
    if (t < 3600) return Str::fmt("%lldm %02llds", t / 60, t % 60);
    return Str::fmt("%lldh %02lldm %02llds", t / 3600, (t / 60) % 60, t % 60);
}

// ---- files -----------------------------------------------------------------------------------------------------
#ifdef _WIN32
// UTF-8 -> UTF-16 for the file calls below. A path of 240+ characters gets the long-path prefix (\\?\ or \\?\UNC\) like FilePath() of winutil.cpp does (this module
// cannot call it): real Windows refuses longer paths (MAX_PATH 260) without it, Wine does not care. The prefix switches the normalisation off, so "/" becomes "\\" here.
static bool Widen(const char* s, wchar_t* out, int cap)
{
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) <= 0) return false;
    size_t n = wcslen(out);
    for (size_t i = 0; i < n; ++i) if (out[i] == L'/') out[i] = L'\\';
    bool prefixed = n >= 4 && !wcsncmp(out, L"\\\\?\\", 4);
    bool unc = n >= 2 && out[0] == L'\\' && out[1] == L'\\' && !prefixed;
    bool drive = n >= 3 && out[1] == L':' && out[2] == L'\\';
    if (n >= 240 && !prefixed && (unc || drive)) {
        const wchar_t* pre = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
        size_t pn = wcslen(pre), skip = unc ? 2 : 0;
        if ((int)(n - skip + pn + 1) > cap) return false;
        memmove(out + pn, out + skip, (n - skip + 1) * sizeof(wchar_t));
        memcpy(out, pre, pn * sizeof(wchar_t));
    }
    return true;
}
static Str WinErr(const char* what, const char* path)
{
    DWORD e = GetLastError();
    return Str::fmt("%s %s: error %lu", what, path, (unsigned long)e);
}
#endif

bool ReadWholeFile(const char* path, Bytes& out, Str& err, size_t maxSize)
{
    out.clear();
#ifdef _WIN32
    wchar_t w[4096];
    if (!Widen(path, w, 4096)) { err = "bad path"; return false; }
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = WinErr("cannot open", path); return false; }
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    if ((u64)sz.QuadPart > maxSize) { CloseHandle(f); err = Str::fmt("%s is too big", path); return false; }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD n = 0;
        if (!ReadFile(f, out.data() + done, (DWORD)(out.size() - done > (1u << 24) ? (1u << 24) : out.size() - done), &n, nullptr) || n == 0) break;
        done += n;
    }
    CloseHandle(f);
    out.resize(done);
    return true;
#else
    FILE* f = fopen(path, "rb");
    if (!f) { err = Str::fmt("cannot open %s", path); return false; }
    u8 buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out.size() + n > maxSize) { fclose(f); err = Str::fmt("%s is too big", path); return false; }
        for (size_t i = 0; i < n; ++i) out.push_back(buf[i]);
    }
    fclose(f);
    return true;
#endif
}

bool WriteWholeFile(const char* path, const void* p, size_t n, Str& err)
{
    Str tmp = Str(path) + ".tmp";
#ifdef _WIN32
    wchar_t w[4096], wt[4106];
    if (!Widen(path, w, 4096) || !Widen(tmp.c_str(), wt, 4106)) { err = "bad path"; return false; }
    HANDLE f = CreateFileW(wt, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = WinErr("cannot write", tmp.c_str()); return false; }
    size_t done = 0; bool ok = true;
    while (done < n) {
        DWORD k = 0;
        if (!WriteFile(f, (const u8*)p + done, (DWORD)(n - done > (1u << 24) ? (1u << 24) : n - done), &k, nullptr)) { ok = false; break; }
        done += k;
    }
    if (ok) ok = FlushFileBuffers(f) != 0;
    CloseHandle(f);
    if (!ok) { err = WinErr("cannot write", tmp.c_str()); DeleteFileW(wt); return false; }
    for (int attempt = 0;; ++attempt) {                                  // a scanner / indexer holding the old file for a moment: wait, do not lose the save
        if (MoveFileExW(wt, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        DWORD le = GetLastError();
        if (attempt >= 15 || (le != ERROR_SHARING_VIOLATION && le != ERROR_ACCESS_DENIED && le != ERROR_LOCK_VIOLATION)) { SetLastError(le); err = WinErr("cannot replace", path); DeleteFileW(wt); return false; }
        Sleep(200);
    }
    return true;
#else
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = Str::fmt("cannot write %s", tmp.c_str()); return false; }
    bool ok = n == 0 || fwrite(p, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    fclose(f);
    if (!ok || rename(tmp.c_str(), path) != 0) { err = Str::fmt("cannot write %s", path); remove(tmp.c_str()); return false; }
    return true;
#endif
}

} // namespace vm2b
