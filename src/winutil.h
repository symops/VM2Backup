// VM2Backup — Windows helpers: UTF-8 <-> UTF-16, long paths, folders, time.
#pragma once
#include "base.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

class Wide {                                   // UTF-8 -> a NUL-terminated wchar_t buffer
public:
    explicit Wide(const char* s);
    explicit Wide(const Str& s) : Wide(s.c_str()) {}
    const wchar_t* c_str() const { return m_v.data(); }
    operator const wchar_t*() const { return m_v.data(); }
private:
    Vec<wchar_t> m_v;
};

Str FromWide(const wchar_t* w);                // UTF-16 -> UTF-8
// A path the file APIs take for any length: '/' becomes '\', long paths get the \\?\ prefix. Relative paths are made absolute.
Wide FilePath(const Str& utf8);
Str  WinErrorText(unsigned long code);         // "Access is denied. (5)"
Str  LastErrorText(const char* what, const Str& path);   // "cannot open C:\x: Access is denied. (5)"

bool MakeDirs(const Str& path, Str& err);      // like mkdir -p
bool FileExists(const Str& path);
bool DirExists(const Str& path);
bool RemoveTree(const Str& path);              // deletes a folder with everything in it (tests, temporary export folders)
Str  JoinPath(const Str& a, const Str& b);     // a + '\' + b without doubling
Str  PathBase(const Str& p);                   // the last component

double UnixNow();                              // seconds since 1970, fractional
Str  LocalStamp(double unixTime);              // "20261024-153045"
Str  LocalIso(double unixTime);                // "2026-10-24T15:30:45"
enum { kMaxNameBytes = 200 };                       // longest VM-name part of a file / folder name we create
Str  TruncateUtf8(const Str& s, size_t maxBytes);
Str  SafeName(const Str& s);                   // a VM name made into a file name: letters, digits, . _ - kept

} // namespace vm2b
