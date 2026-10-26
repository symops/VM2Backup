// VM2Backup — basic types without the C++ library (the programs link without libstdc++, like Click2Screen):
// Str (UTF-8 string), Vec<T> (array), Json (value, parser, writer), small helpers. Portable: the pure modules
// (base, json, tar, schedule) also build natively on Linux for fast tests.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

// Placement new without <new> (there are no C++ library headers for MinGW here): a tagged form, declared globally.
struct vm2b_place_tag {};
inline void* operator new(size_t, void* p, vm2b_place_tag) noexcept { return p; }
inline void operator delete(void*, void*, vm2b_place_tag) noexcept {}

namespace vm2b {

typedef int64_t i64;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint16_t u16;

// ---- Vec<T> ----------------------------------------------------------------------------------------------------
template <class T> class Vec {
public:
    Vec() : m_p(nullptr), m_n(0), m_cap(0) {}
    Vec(const Vec& o) : m_p(nullptr), m_n(0), m_cap(0) { reserve(o.m_n); for (size_t i = 0; i < o.m_n; ++i) new (&m_p[i], vm2b_place_tag()) T(o.m_p[i]); m_n = o.m_n; }
    Vec(Vec&& o) : m_p(o.m_p), m_n(o.m_n), m_cap(o.m_cap) { o.m_p = nullptr; o.m_n = o.m_cap = 0; }
    ~Vec() { clear(); free(m_p); }
    Vec& operator=(const Vec& o) { if (this != &o) { clear(); reserve(o.m_n); for (size_t i = 0; i < o.m_n; ++i) new (&m_p[i], vm2b_place_tag()) T(o.m_p[i]); m_n = o.m_n; } return *this; }
    Vec& operator=(Vec&& o) { if (this != &o) { clear(); free(m_p); m_p = o.m_p; m_n = o.m_n; m_cap = o.m_cap; o.m_p = nullptr; o.m_n = o.m_cap = 0; } return *this; }
    size_t size() const { return m_n; }
    bool empty() const { return m_n == 0; }
    T& operator[](size_t i) { return m_p[i]; }
    const T& operator[](size_t i) const { return m_p[i]; }
    T* data() { return m_p; }
    const T* data() const { return m_p; }
    T* begin() { return m_p; }
    T* end() { return m_p + m_n; }
    const T* begin() const { return m_p; }
    const T* end() const { return m_p + m_n; }
    T& back() { return m_p[m_n - 1]; }
    const T& back() const { return m_p[m_n - 1]; }
    void reserve(size_t n) {
        if (n <= m_cap) return;
        size_t c = m_cap ? m_cap * 2 : 4; if (c < n) c = n;
        T* p = (T*)malloc(c * sizeof(T));
        if (!p) abort();
        for (size_t i = 0; i < m_n; ++i) { new (&p[i], vm2b_place_tag()) T(static_cast<T&&>(m_p[i])); m_p[i].~T(); }
        free(m_p); m_p = p; m_cap = c;
    }
    void push_back(const T& v) { T tmp(v); reserve(m_n + 1); new (&m_p[m_n], vm2b_place_tag()) T(static_cast<T&&>(tmp)); ++m_n; }
    void push_back(T&& v) { T tmp(static_cast<T&&>(v)); reserve(m_n + 1); new (&m_p[m_n], vm2b_place_tag()) T(static_cast<T&&>(tmp)); ++m_n; }
    void pop_back() { m_p[--m_n].~T(); }
    void clear() { for (size_t i = 0; i < m_n; ++i) m_p[i].~T(); m_n = 0; }
    void resize(size_t n) { if (n < m_n) { for (size_t i = n; i < m_n; ++i) m_p[i].~T(); m_n = n; } else { reserve(n); for (size_t i = m_n; i < n; ++i) new (&m_p[i], vm2b_place_tag()) T(); m_n = n; } }
    void erase(size_t i) { m_p[i].~T(); for (size_t k = i + 1; k < m_n; ++k) { new (&m_p[k - 1], vm2b_place_tag()) T(static_cast<T&&>(m_p[k])); m_p[k].~T(); } --m_n; }
    void insert(size_t i, const T& v) { T tmp(v); reserve(m_n + 1); for (size_t k = m_n; k > i; --k) { new (&m_p[k], vm2b_place_tag()) T(static_cast<T&&>(m_p[k - 1])); m_p[k - 1].~T(); } new (&m_p[i], vm2b_place_tag()) T(static_cast<T&&>(tmp)); ++m_n; }
private:
    T* m_p; size_t m_n, m_cap;
};

// ---- Str: UTF-8 / byte string ----------------------------------------------------------------------------------
class Str {
public:
    Str() : m_p(nullptr), m_n(0), m_cap(0) {}
    Str(const char* s) : m_p(nullptr), m_n(0), m_cap(0) { if (s) append(s, strlen(s)); }
    Str(const char* s, size_t n) : m_p(nullptr), m_n(0), m_cap(0) { append(s, n); }
    Str(const Str& o) : m_p(nullptr), m_n(0), m_cap(0) { append(o.m_p, o.m_n); }
    Str(Str&& o) : m_p(o.m_p), m_n(o.m_n), m_cap(o.m_cap) { o.m_p = nullptr; o.m_n = o.m_cap = 0; }
    ~Str() { free(m_p); }
    Str& operator=(const Str& o) { if (this != &o) { m_n = 0; append(o.m_p, o.m_n); } return *this; }
    Str& operator=(Str&& o) { if (this != &o) { free(m_p); m_p = o.m_p; m_n = o.m_n; m_cap = o.m_cap; o.m_p = nullptr; o.m_n = o.m_cap = 0; } return *this; }
    Str& operator=(const char* s) { m_n = 0; if (s) append(s, strlen(s)); return *this; }
    size_t size() const { return m_n; }
    size_t length() const { return m_n; }
    bool empty() const { return m_n == 0; }
    const char* c_str() const { return m_p ? m_p : ""; }
    const char* data() const { return m_p ? m_p : ""; }
    char* mut() { return m_p; }
    char& operator[](size_t i) { return m_p[i]; }
    char operator[](size_t i) const { return m_p[i]; }
    void reserve(size_t n) {
        if (n + 1 <= m_cap) return;
        size_t c = m_cap ? m_cap * 2 : 16; if (c < n + 1) c = n + 1;
        char* p = (char*)realloc(m_p, c);
        if (!p) abort();
        m_p = p; m_cap = c; m_p[m_n] = 0;
    }
    void resize(size_t n, char fill = 0) { reserve(n); if (n > m_n) memset(m_p + m_n, fill, n - m_n); m_n = n; if (m_p) m_p[n] = 0; }
    void clear() { m_n = 0; if (m_p) m_p[0] = 0; }
    Str& append(const char* s, size_t n) { if (!n) return *this; reserve(m_n + n); memcpy(m_p + m_n, s, n); m_n += n; m_p[m_n] = 0; return *this; }
    Str& append(const char* s) { return s ? append(s, strlen(s)) : *this; }
    Str& append(const Str& s) { return append(s.m_p, s.m_n); }
    Str& operator+=(const Str& s) { return append(s.m_p, s.m_n); }
    Str& operator+=(const char* s) { return append(s); }
    Str& operator+=(char c) { return append(&c, 1); }
    void push_back(char c) { append(&c, 1); }
    Str operator+(const Str& o) const { Str r(*this); r.append(o); return r; }
    Str operator+(const char* o) const { Str r(*this); r.append(o); return r; }
    bool operator==(const Str& o) const { return m_n == o.m_n && (m_n == 0 || memcmp(m_p, o.m_p, m_n) == 0); }
    bool operator==(const char* o) const { size_t n = strlen(o); return m_n == n && (n == 0 || memcmp(m_p, o, n) == 0); }
    bool operator!=(const Str& o) const { return !(*this == o); }
    bool operator!=(const char* o) const { return !(*this == o); }
    bool operator<(const Str& o) const { size_t n = m_n < o.m_n ? m_n : o.m_n; int c = n ? memcmp(m_p, o.m_p, n) : 0; return c < 0 || (c == 0 && m_n < o.m_n); }
    int find(char c, size_t from = 0) const { for (size_t i = from; i < m_n; ++i) if (m_p[i] == c) return (int)i; return -1; }
    int find(const char* s, size_t from = 0) const { size_t n = strlen(s); if (!n) return (int)from; for (size_t i = from; i + n <= m_n; ++i) if (memcmp(m_p + i, s, n) == 0) return (int)i; return -1; }
    int rfind(char c) const { for (size_t i = m_n; i-- > 0;) if (m_p[i] == c) return (int)i; return -1; }
    Str substr(size_t pos, size_t n = (size_t)-1) const { if (pos > m_n) pos = m_n; if (n > m_n - pos) n = m_n - pos; return Str(m_p ? m_p + pos : "", n); }
    bool starts_with(const char* s) const { size_t n = strlen(s); return m_n >= n && memcmp(m_p, s, n) == 0; }
    bool ends_with(const char* s) const { size_t n = strlen(s); return m_n >= n && memcmp(m_p + m_n - n, s, n) == 0; }
    Str lower() const { Str r(*this); for (size_t i = 0; i < r.m_n; ++i) if (r.m_p[i] >= 'A' && r.m_p[i] <= 'Z') r.m_p[i] += 32; return r; }
    Str trim() const { size_t a = 0, b = m_n; while (a < b && (unsigned char)m_p[a] <= ' ') ++a; while (b > a && (unsigned char)m_p[b - 1] <= ' ') --b; return substr(a, b - a); }
    Str replace_all(const char* from, const char* to) const;
    static Str fmt(const char* f, ...);
    static Str vfmt(const char* f, va_list ap);
private:
    char* m_p; size_t m_n, m_cap;
};
inline Str operator+(const char* a, const Str& b) { Str r(a); r.append(b); return r; }

typedef Vec<u8> Bytes;

// ---- Json ------------------------------------------------------------------------------------------------------
class Json {
public:
    enum Type { Null, Bool, Int, Real, String, Array, Object };
    Json() : m_t(Null), m_i(0), m_d(0) {}
    Json(bool b) : m_t(Bool), m_i(b), m_d(0) {}
    Json(int v) : m_t(Int), m_i(v), m_d(0) {}
    Json(unsigned v) : m_t(Int), m_i(v), m_d(0) {}
    Json(i64 v) : m_t(Int), m_i(v), m_d(0) {}
    Json(u64 v) : m_t(Int), m_i((i64)v), m_d(0) {}
    Json(double v) : m_t(Real), m_i(0), m_d(v) {}
    Json(const char* s) : m_t(String), m_i(0), m_d(0), m_s(s) {}
    Json(const Str& s) : m_t(String), m_i(0), m_d(0), m_s(s) {}
    static Json object() { Json j; j.m_t = Object; return j; }
    static Json array() { Json j; j.m_t = Array; return j; }
    Type type() const { return m_t; }
    bool is_null() const { return m_t == Null; }
    bool is_object() const { return m_t == Object; }
    bool is_array() const { return m_t == Array; }
    bool is_string() const { return m_t == String; }
    bool is_number() const { return m_t == Int || m_t == Real; }
    bool is_bool() const { return m_t == Bool; }
    // Typed reads with a default for a missing / mistyped value (the config files may be edited by hand).
    Str str(const char* def = "") const { return m_t == String ? m_s : Str(def); }
    i64 integer(i64 def = 0) const { return m_t == Int ? m_i : m_t == Real ? (i64)m_d : m_t == Bool ? m_i : def; }
    double num(double def = 0) const { return m_t == Real ? m_d : m_t == Int ? (double)m_i : def; }
    bool boolean(bool def = false) const { return m_t == Bool ? m_i != 0 : m_t == Int ? m_i != 0 : def; }
    size_t size() const { return m_t == Array ? m_a.size() : m_t == Object ? m_k.size() : 0; }
    // Array
    const Json& at(size_t i) const { return i < m_a.size() ? m_a[i] : null_value(); }
    Json& item(size_t i) { return m_a[i]; }                    // a mutable array element (the caller keeps i < size())
    Json& push(const Json& v) { if (m_t != Array) { *this = array(); } m_a.push_back(v); return m_a.back(); }
    // Object (insertion order is kept; lookup is linear: the objects are small)
    bool has(const char* k) const { return find(k) >= 0; }
    const Json& get(const char* k) const { int i = find(k); return i >= 0 ? m_v[i] : null_value(); }
    Json& operator[](const char* k) { if (m_t != Object) { *this = object(); } int i = find(k); if (i < 0) { m_k.push_back(Str(k)); m_v.push_back(Json()); i = (int)m_k.size() - 1; } return m_v[i]; }
    void set(const char* k, const Json& v) { (*this)[k] = v; }
    Json& add_member(const Str& k) { if (m_t != Object) *this = object(); m_k.push_back(k); m_v.push_back(Json()); return m_v.back(); }   // parser only: no search for a duplicate key (that is O(n) per member)
    void remove(const char* k) { int i = find(k); if (i >= 0) { m_k.erase(i); m_v.erase(i); } }
    const Str& key_at(size_t i) const { return m_k[i]; }
    const Json& value_at(size_t i) const { return m_v[i]; }
    Json& value_at(size_t i) { return m_v[i]; }
    Str dump(bool pretty = false) const;
    static bool parse(const char* text, size_t n, Json& out, Str& err);
    static bool parse(const Str& s, Json& out, Str& err) { return parse(s.data(), s.size(), out, err); }
    bool operator==(const Json& o) const;
private:
    int find(const char* k) const { for (size_t i = 0; i < m_k.size(); ++i) if (m_k[i] == k) return (int)i; return -1; }
    static const Json& null_value() { static const Json n; return n; }
    void dump_to(Str& out, bool pretty, int depth) const;
    Type m_t; i64 m_i; double m_d; Str m_s; Vec<Json> m_a; Vec<Str> m_k; Vec<Json> m_v;
};

// ---- helpers ---------------------------------------------------------------------------------------------------
Str HexEncode(const void* p, size_t n);
bool HexDecode(const char* s, size_t n, Bytes& out);
Str Base64Encode(const void* p, size_t n);
bool Base64Decode(const char* s, size_t n, Bytes& out);
bool Utf8Valid(const char* s, size_t n);
Str FormatSize(u64 bytes);                    // "1.5 GB"
Str FormatDuration(double seconds);           // "1h 02m 05s"

// Whole-file helpers (UTF-8 paths on every platform).
bool ReadWholeFile(const char* path, Bytes& out, Str& err, size_t maxSize = (size_t)256 << 20);
bool WriteWholeFile(const char* path, const void* p, size_t n, Str& err);   // atomic: temp file + rename

} // namespace vm2b
