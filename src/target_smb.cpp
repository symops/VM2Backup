// Network folders (SMB / CIFS): a backup target on a UNC path. The connection is made with WNetAddConnection2W (no drive letter, so
// it works for a service in session 0 and does not clash with the drive letters of a user); every file operation is then done by
// the local-folder target on the UNC path. See smb.h.
#include "smb.h"
#include "netbrowse.h"
#include "winutil.h"
#include <winnetwk.h>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace vm2b {

// ---- UNC paths -------------------------------------------------------------------------------------------------
namespace {

const size_t kMaxUncBytes = 2048, kMaxComponentBytes = 255, kMaxShareBytes = 80;

bool IsReserved(const Str& comp)
{
    Str b = comp; int dot = b.find('.'); if (dot >= 0) b = b.substr(0, (size_t)dot);
    while (b.size() && b[b.size() - 1] == ' ') b = b.substr(0, b.size() - 1);
    Str l = b.lower();
    if (l == "con" || l == "prn" || l == "aux" || l == "nul" || l == "conin$" || l == "conout$") return true;
    if (l.size() == 4 && (l.starts_with("com") || l.starts_with("lpt")) && l[3] >= '1' && l[3] <= '9') return true;
    return false;
}

// A folder / share name: no control characters, none of the characters Windows refuses, no trailing dot / space.
bool CheckComponent(const Str& c, bool share, Str& err)
{
    if (c.empty()) { err = "the path has an empty part"; return false; }
    if (c.size() > (share ? kMaxShareBytes : kMaxComponentBytes)) { err = share ? "the share name is too long" : "a folder name in the path is too long"; return false; }
    if (!Utf8Valid(c.data(), c.size())) { err = "the path is not valid text"; return false; }
    for (size_t i = 0; i < c.size(); ++i) {
        unsigned char ch = (unsigned char)c[i];
        if (ch < 0x20 || ch == 0x7f) { err = "the path contains a control character"; return false; }
        if (strchr("<>:\"|?*", ch)) { err = Str::fmt("the path contains a character that Windows does not allow in names: %c", (char)ch); return false; }
    }
    if (c == "." || c == "..") { err = "the path must not contain \".\" or \"..\" parts"; return false; }
    char last = c[c.size() - 1];
    if (last == '.' || last == ' ' || c[0] == ' ') { err = "a name in the path starts with a space or ends with a dot or a space"; return false; }
    if (IsReserved(c)) { err = "a name in the path is a reserved Windows device name: " + c; return false; }
    return true;
}

bool CheckHost(const Str& h, Str& err)
{
    if (h.empty()) { err = "the server name is missing: use \\\\server\\share"; return false; }
    if (h.size() > 255) { err = "the server name is too long"; return false; }
    if (!Utf8Valid(h.data(), h.size())) { err = "the server name is not valid text"; return false; }
    for (size_t i = 0; i < h.size(); ++i) {
        unsigned char ch = (unsigned char)h[i];
        bool ok = ch >= 0x80 || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '.' || ch == '_';
        if (!ok) { err = "the server name contains a character that is not allowed (for an IPv6 address use the form fe80--1.ipv6-literal.net)"; return false; }
    }
    if (h[0] == '.' || h[0] == '-' || h[h.size() - 1] == '.' || h[h.size() - 1] == '-' || h.find("..") >= 0) { err = "the server name is not valid"; return false; }
    return true;
}

Vec<Str> Split(const Str& s, char sep)
{
    Vec<Str> out; size_t from = 0;
    for (size_t i = 0; i <= s.size(); ++i) if (i == s.size() || s[i] == sep) { out.push_back(s.substr(from, i - from)); from = i + 1; }
    return out;
}

} // namespace

bool SmbIsUncPath(const Str& p)
{
    if (p.size() < 2) return false;
    bool s0 = p[0] == '\\' || p[0] == '/', s1 = p[1] == '\\' || p[1] == '/';
    if (!s0 || !s1) return false;
    if (p.size() >= 4 && (p[2] == '?' || p[2] == '.') && (p[3] == '\\' || p[3] == '/')) return false;
    return true;
}

bool SmbHasDriveLetter(const Str& p)
{
    return p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && (p.size() == 2 || p[2] == '\\' || p[2] == '/');
}

bool SmbNormalizeUnc(const Str& in, Str& out, Str& err)
{
    out.clear();
    Str p = in.trim();
    if (p.empty()) { err = "enter the network folder as \\\\server\\share"; return false; }
    if (p.size() > kMaxUncBytes) { err = "the path is too long"; return false; }
    p = p.replace_all("/", "\\");
    if (p.starts_with("\\\\?\\") || p.starts_with("\\\\.\\")) { err = "device paths (\\\\?\\ and \\\\.\\) are not accepted: enter the network folder as \\\\server\\share"; return false; }
    if (!p.starts_with("\\\\")) {
        err = SmbHasDriveLetter(p) ? "this is a drive letter, not a network path: enter \\\\server\\share (for a mapped network drive use \"Network drive...\")"
                                   : "this is not a network path: enter \\\\server\\share[\\folder]";
        return false;
    }
    if (p.size() > 2 && p[2] == '\\') { err = "too many backslashes at the start: enter \\\\server\\share"; return false; }
    Vec<Str> parts = Split(p.substr(2), '\\');
    // an empty part between two backslashes inside the folder part is harmless ("a\\b"); an empty server or share is not
    if (parts.size() < 2 || parts[1].empty()) { err = "the share name is missing: enter \\\\server\\share"; return false; }
    if (!CheckHost(parts[0], err)) return false;
    if (!CheckComponent(parts[1], true, err)) return false;
    Str r = "\\\\" + parts[0] + "\\" + parts[1];
    for (size_t i = 2; i < parts.size(); ++i) {
        if (parts[i].empty()) { if (i + 1 == parts.size()) continue; continue; }       // "\\host\share\" and "a\\b"
        if (!CheckComponent(parts[i], false, err)) return false;
        r += "\\" + parts[i];
    }
    if (r.size() > kMaxUncBytes) { err = "the path is too long"; return false; }
    out = r;
    return true;
}

bool SmbSplitUnc(const Str& unc, Str& root, Str& sub, Str& host)
{
    Str n, e;
    if (!SmbNormalizeUnc(unc, n, e)) return false;
    Vec<Str> parts = Split(n.substr(2), '\\');
    host = parts[0]; root = "\\\\" + parts[0] + "\\" + parts[1]; sub.clear();
    for (size_t i = 2; i < parts.size(); ++i) { if (!sub.empty()) sub += "\\"; sub += parts[i]; }
    return true;
}

bool SmbRelativeDir(const Str& in, Str& out, Str& err)
{
    out.clear();
    Str p = in.trim().replace_all("/", "\\");
    if (p.size() > kMaxUncBytes) { err = "the folder is too long"; return false; }
    if (p.size() >= 2 && p[1] == ':') { err = "the folder inside a network share must not start with a drive letter"; return false; }
    Vec<Str> parts = Split(p, '\\'); Str r;
    for (size_t i = 0; i < parts.size(); ++i) {
        const Str& c = parts[i];
        if (c.empty() || c == ".") continue;
        if (!CheckComponent(c, false, err)) return false;
        if (!r.empty()) r += "\\";
        r += c;
    }
    out = r;
    return true;
}

// ---- errors ----------------------------------------------------------------------------------------------------
bool SmbIsTransientCode(unsigned long c)
{
    switch (c) {
    case 6: case 21: case 51: case 53: case 54: case 55: case 59: case 64: case 88: case 121: case 995:
    case 1222: case 1225: case 1226: case 1231: case 1232: case 1235: case 1236: case 1311: case 1460: case 2250:
        return true;
    }
    return false;
}

bool SmbIsAuthCode(unsigned long c)
{
    return c == 86 || c == 1326 || c == 1909 || c == 1331 || c == 1330 || c == 1907 || c == 1385 || c == 1327 || c == 1328;
}

unsigned long SmbErrorCodeOf(const Str& t)
{
    size_t n = t.size();
    if (n < 3 || t[n - 1] != ')') return 0;
    size_t i = n - 1; unsigned long v = 0, mul = 1; bool any = false;
    while (i > 0 && t[i - 1] >= '0' && t[i - 1] <= '9') { v += (unsigned long)(t[i - 1] - '0') * mul; mul *= 10; any = true; --i; if (mul > 1000000000ul) return 0; }
    return any && i > 0 && t[i - 1] == '(' ? v : 0;
}

Str SmbErrorText(unsigned long code, const Str& host, const Str& share, bool& transient)
{
    transient = SmbIsTransientCode(code);
    Str h = host.empty() ? Str("the server") : host, s = share.empty() ? Str("the share") : Str("\"") + share + "\"";
    switch (code) {
    case 53: return Str::fmt("the network path was not found: cannot reach %s (check the name or address, and that the computer is on) (%lu)", h.c_str(), code);
    case 67: return Str::fmt("the share %s was not found on %s (%lu)", s.c_str(), h.c_str(), code);
    case 86: case 1326: return Str::fmt("the user name or the password is wrong (%lu)", code);
    case 1219: return Str::fmt("%s already has a connection from this computer with other credentials: disconnect it first (net use \\\\%s /delete) or use the same user (%lu)", h.c_str(), h.c_str(), code);
    case 5: return Str::fmt("access denied: the user has no right to %s on %s, or no user name was given (%lu)", s.c_str(), h.c_str(), code);
    case 1203: return Str::fmt("the network path is not valid, no network provider accepted it (%lu)", code);
    case 1331: return Str::fmt("the account is disabled (%lu)", code);
    case 1907: return Str::fmt("the password of this account must be changed before it can log on: change it (for example at a Windows logon), then enter the new one here (%lu)", code);
    case 1330: return Str::fmt("the password of this account has expired: change it, then enter the new one here (%lu)", code);
    case 1909: return Str::fmt("the account is locked out (%lu)", code);
    case 1327: case 1328: case 1385: return Str::fmt("the account is not allowed to log on to %s (%lu)", h.c_str(), code);
    case 1396: return Str::fmt("the account name does not fit %s (wrong domain, or a trust problem) (%lu)", h.c_str(), code);
    case 1222: case 1231: case 1232: return Str::fmt("the network is not reachable: %s cannot be contacted (%lu)", h.c_str(), code);
    case 1225: return Str::fmt("%s refused the connection (%lu)", h.c_str(), code);
    case 1460: case 121: return Str::fmt("the connection to %s timed out (%lu)", h.c_str(), code);
    case 64: case 59: case 1236: case 1235: return Str::fmt("the connection to %s was lost (%lu)", h.c_str(), code);
    case 1311: return Str::fmt("no domain controller can be contacted to check the account (%lu)", code);
    case 2250: return Str::fmt("the connection to %s does not exist (%lu)", h.c_str(), code);
    }
    return WinErrorText(code);
}

// ---- the Windows network API -------------------------------------------------------------------------------------
namespace {

typedef DWORD (WINAPI *AddFn)(LPNETRESOURCEW, LPCWSTR, LPCWSTR, DWORD);
typedef DWORD (WINAPI *CancelFn)(LPCWSTR, DWORD, BOOL);
typedef DWORD (WINAPI *GetConnFn)(LPCWSTR, LPWSTR, LPDWORD);

struct Mpr {                                    // mpr.dll is loaded when needed: the installer and the launcher do not link it
    HMODULE h; AddFn add; CancelFn cancel; GetConnFn getConn;
    Mpr() : h(LoadLibraryW(L"mpr.dll")), add(nullptr), cancel(nullptr), getConn(nullptr) {
        if (h) { add = (AddFn)(void*)GetProcAddress(h, "WNetAddConnection2W"); cancel = (CancelFn)(void*)GetProcAddress(h, "WNetCancelConnection2W"); getConn = (GetConnFn)(void*)GetProcAddress(h, "WNetGetConnectionW"); }
    }
};
Mpr& GetMpr() { static Mpr m; return m; }

unsigned long RealAdd(const wchar_t* remote, const wchar_t* user, const wchar_t* pass)
{
    Mpr& m = GetMpr(); if (!m.add) return ERROR_NO_NETWORK;
    NETRESOURCEW nr; memset(&nr, 0, sizeof(nr)); nr.dwType = RESOURCETYPE_DISK; nr.lpRemoteName = (LPWSTR)remote;   // no local name: a connection without a drive letter
    return m.add(&nr, pass, user, CONNECT_TEMPORARY);
}
unsigned long RealCancel(const wchar_t* remote, int force)
{
    Mpr& m = GetMpr(); if (!m.cancel) return ERROR_NO_NETWORK;
    return m.cancel(remote, 0, force ? TRUE : FALSE);
}
// Does the server answer on TCP 445 within 3 seconds? (a name that does not resolve or a silent address: false)
bool RealProbe(const Str& host)
{
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    bool up = false;
    ADDRINFOA hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    ADDRINFOA* res = nullptr;
    if (getaddrinfo(host.c_str(), "445", &hints, &res) == 0 && res) {
        for (ADDRINFOA* a = res; a && !up; a = a->ai_next) {
            SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol); if (s == INVALID_SOCKET) continue;
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            int rc = connect(s, a->ai_addr, (int)a->ai_addrlen);
            if (rc == 0) up = true;
            else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(s, &w); FD_SET(s, &x); timeval tv = { 3, 0 };
                if (select(0, nullptr, &w, &x, &tv) > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &x)) { int err = 0, el = sizeof(err); getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el); up = err == 0; }
            }
            closesocket(s);
        }
        freeaddrinfo(res);
    }
    WSACleanup();
    return up;
}
const SmbApi kRealApi = { RealAdd, RealCancel, nullptr, RealProbe };
const SmbApi* volatile g_api = &kRealApi;

const SmbApi& Api() { return *g_api; }

Str Mapped(const Str& unc) { return Api().mapPath ? Api().mapPath(unc) : unc; }

// The connections we made: a share is cancelled when the last holder lets go.
struct Reg {
    struct E { Str root; int refs; int gen; };
    CRITICAL_SECTION cs; Vec<E> v; int lastGen;
    Reg() : lastGen(0) { InitializeCriticalSection(&cs); }
    int find(const Str& root) const { Str l = root.lower(); for (size_t i = 0; i < v.size(); ++i) if (v[i].root == l) return (int)i; return -1; }
};
Reg& GetReg() { static Reg r; return r; }

Str ServerOf(const Str& root) { Str r = root.lower(); int c = r.find('\\', 2); return c > 0 ? r.substr(0, (size_t)c) : r; }

} // namespace

Str SmbMapPath(const Str& unc) { return Mapped(unc); }

void SmbSetApi(const SmbApi* api) { g_api = api ? api : &kRealApi; }

bool SmbServerHeld(const Str& host) {
    Reg& r = GetReg(); Str srv = "\\\\" + host.lower(); bool held = false;
    EnterCriticalSection(&r.cs); for (size_t i = 0; i < r.v.size(); ++i) if (ServerOf(r.v[i].root) == srv) held = true; LeaveCriticalSection(&r.cs);
    return held;
}

int SmbOpenConnections() { Reg& r = GetReg(); EnterCriticalSection(&r.cs); int n = (int)r.v.size(); LeaveCriticalSection(&r.cs); return n; }

bool SmbConnection::open(const Str& root, const Str& user, const Str& password, bool reset, Str& err, bool& transient)
{
    transient = false;
    Str host, share; { Str r2, s2; SmbSplitUnc(root, r2, s2, host); int c = r2.rfind('\\'); share = c >= 0 ? r2.substr((size_t)c + 1) : Str(); }
    Wide wr(root), wu(user), wp(password);
    const wchar_t* u = user.empty() ? nullptr : wu.c_str();
    const wchar_t* p = user.empty() ? nullptr : wp.c_str();
    Reg& reg = GetReg();
    if (reset) {                                             // a stale connection (the server was restarted, the cable pulled): start afresh
        Api().cancel(wr.c_str(), 1);                         // the hold (ours and the other holders') stays counted: the connection is re-made below
    }
    unsigned long rc = Api().add(wr.c_str(), u, p);
    if (rc == ERROR_SESSION_CREDENTIAL_CONFLICT || rc == ERROR_ALREADY_ASSIGNED) {
        // Windows allows one set of credentials per server and logon session. When the connection that is in the way is one of ours
        // (an earlier edit of this server, another share of it with other credentials), cancel it and try once more.
        bool ours = false; Str srv = ServerOf(root);
        EnterCriticalSection(&reg.cs);
        for (size_t i = reg.v.size(); i-- > 0;) if (ServerOf(reg.v[i].root) == srv) { Wide wo(reg.v[i].root); Api().cancel(wo.c_str(), 0); reg.v.erase(i); ours = true; }
        LeaveCriticalSection(&reg.cs);
        if (ours) { m_held = false; rc = Api().add(wr.c_str(), u, p); }
        if (rc == ERROR_SESSION_CREDENTIAL_CONFLICT && user.empty()) rc = 0;     // no credentials asked for: the existing connection is used as it is
    }
    // Windows says 53 (Windows 10) or 67 (Windows 11 25H2) both for a missing share and for a server that does not answer at all: the TCP probe of port 445
    // tells them apart. The server answers: it is the share that does not exist (67, permanent); silent: the network (53, transient, retried).
    if ((rc == ERROR_BAD_NETPATH || rc == ERROR_BAD_NET_NAME) && Api().probe) rc = Api().probe(host) ? ERROR_BAD_NET_NAME : ERROR_BAD_NETPATH;
    if (rc != 0) { err = SmbErrorText(rc, host, share, transient); return false; }
    EnterCriticalSection(&reg.cs);
    int i = reg.find(root);
    if (m_held && (i < 0 || reg.v[(size_t)i].gen != m_gen)) m_held = false;      // our registration was cancelled meanwhile (credential conflict of another target): hold again
    if (!m_held) {
        if (i >= 0) { reg.v[(size_t)i].refs++; m_gen = reg.v[(size_t)i].gen; }
        else { Reg::E e; e.root = root.lower(); e.refs = 1; e.gen = ++reg.lastGen; m_gen = e.gen; reg.v.push_back(e); }
        m_held = true; m_root = root;
    }
    LeaveCriticalSection(&reg.cs);
    return true;
}

void SmbConnection::release()
{
    if (!m_held) return;
    m_held = false;
    Reg& reg = GetReg(); bool cancel = false;
    EnterCriticalSection(&reg.cs);
    int i = reg.find(m_root);
    if (i >= 0 && reg.v[(size_t)i].gen == m_gen && --reg.v[(size_t)i].refs <= 0) { reg.v.erase((size_t)i); cancel = true; }
    LeaveCriticalSection(&reg.cs);
    if (cancel) { Wide w(m_root); Api().cancel(w.c_str(), 0); }
}

// ---- network drives (the GUI) ---------------------------------------------------------------------------------
// Test hook (guitest.cpp): a fixed list of "network drives" instead of the real ones ("Z:=\\\\nas\\share;Y:=\\\\srv\\vms"; "" = none; nullptr = real).
static bool g_fakeDrivesOn = false; static Str g_fakeDrives;
void SmbSetFakeDrives(const char* spec) { g_fakeDrivesOn = spec != nullptr; g_fakeDrives = spec ? spec : ""; }
static void FakeDriveList(Vec<Str>& letters, Vec<Str>& uncs)
{
    size_t p = 0; const Str& f = g_fakeDrives;
    while (p < f.size()) {
        int e = f.find(";", p); size_t end = e < 0 ? f.size() : (size_t)e; Str one = f.substr(p, end - p); p = end + 1;
        int eq = one.find("="); if (eq == 2) { letters.push_back(one.substr(0, 2)); uncs.push_back(one.substr(3)); }
    }
}

void SmbListNetworkDrives(Vec<Str>& letters, Vec<Str>& uncs)
{
    letters.clear(); uncs.clear();
    if (g_fakeDrivesOn) { FakeDriveList(letters, uncs); return; }
    DWORD mask = GetLogicalDrives(); Mpr& m = GetMpr();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 }, dev[3] = { (wchar_t)(L'A' + i), L':', 0 };
        if (GetDriveTypeW(root) != DRIVE_REMOTE) continue;
        wchar_t buf[1024]; DWORD n = 1024; buf[0] = 0;
        DWORD rc = m.getConn ? m.getConn(dev, buf, &n) : (DWORD)ERROR_NO_NETWORK;
        if ((rc == NO_ERROR || rc == ERROR_CONNECTION_UNAVAIL) && buf[0]) { letters.push_back(FromWide(dev)); uncs.push_back(FromWide(buf)); }
    }
}

bool SmbResolveDrivePath(const Str& path, Str& unc, Str& err)
{
    if (!SmbHasDriveLetter(path)) { err = "this is not a drive path"; return false; }
    Str rest = path.substr(2).replace_all("/", "\\");
    if (g_fakeDrivesOn) {
        Vec<Str> L, U; FakeDriveList(L, U); char up = (char)toupper((unsigned char)path[0]); int k = -1;
        for (size_t i = 0; i < L.size(); ++i) if (L[i][0] == up) k = (int)i;
        if (k < 0) { err = Str::fmt("%c: is not a network drive", up); return false; }
        Str base = U[(size_t)k]; while (rest.starts_with("\\")) rest = rest.substr(1);
        Str joined = rest.empty() ? base : base + "\\" + rest; return SmbNormalizeUnc(joined, unc, err);
    }
    wchar_t dev[3] = { (wchar_t)toupper((unsigned char)path[0]), L':', 0 }, root[4] = { dev[0], L':', L'\\', 0 };
    if (GetDriveTypeW(root) != DRIVE_REMOTE) { err = Str::fmt("%c: is not a network drive", (char)dev[0]); return false; }
    Mpr& m = GetMpr(); wchar_t buf[1024]; DWORD n = 1024; buf[0] = 0;
    DWORD rc = m.getConn ? m.getConn(dev, buf, &n) : (DWORD)ERROR_NO_NETWORK;
    if ((rc != NO_ERROR && rc != ERROR_CONNECTION_UNAVAIL) || !buf[0]) { err = Str::fmt("cannot find the network path of drive %c: (%s)", (char)dev[0], WinErrorText(rc).c_str()); return false; }
    Str base = FromWide(buf);
    while (base.size() && base[base.size() - 1] == '\\') base = base.substr(0, base.size() - 1);
    while (rest.starts_with("\\")) rest = rest.substr(1);
    Str joined = rest.empty() ? base : base + "\\" + rest;
    return SmbNormalizeUnc(joined, unc, err);
}

Str SmbDriveUnavailableMessage(const Str& path)
{
    if (!SmbHasDriveLetter(path)) return Str();
    wchar_t root[4] = { (wchar_t)toupper((unsigned char)path[0]), L':', L'\\', 0 };
    UINT t = GetDriveTypeW(root);
    if (t != DRIVE_NO_ROOT_DIR && t != DRIVE_UNKNOWN) return Str();
    return Str::fmt("drive %c: is not available to the service: use the UNC path (the program can convert it)", (char)root[0]);
}

// ---- the target ------------------------------------------------------------------------------------------------
namespace {

class SmbTarget;

// Passes the calls on and gives a failure its class: a broken connection is transient, a refusal is not.
class SmbSink : public Sink {
public:
    explicit SmbSink(Sink* in) : m_in(in) {}
    ~SmbSink() { delete m_in; }
    bool write(const void* p, size_t n) override { if (m_in->write(p, n)) return true; return pass(); }
    bool finish() override { if (m_in->finish()) return true; return pass(); }
    void abort() override { m_in->abort(); }
private:
    bool pass() { err = m_in->err; transient = SmbIsTransientCode(SmbErrorCodeOf(err)); return false; }
    Sink* m_in;
};

class SmbSource : public Source {
public:
    explicit SmbSource(Source* in) : m_in(in) {}
    ~SmbSource() { delete m_in; }
    i64 read(void* p, size_t n) override { i64 k = m_in->read(p, n); if (k >= 0) return k; err = m_in->err; transient = SmbIsTransientCode(SmbErrorCodeOf(err)); return -1; }
    void close() override { m_in->close(); }
private:
    Source* m_in;
};

class SmbTarget : public Target {
public:
    SmbTarget(const Str& root, const Str& folder, const Str& user, const Str& pass) : m_root(root), m_folder(folder), m_user(user), m_pass(pass), m_in(nullptr) {}
    ~SmbTarget() { delete m_in; m_conn.release(); }
    const char* kind() const override { return "smb"; }

    bool connect() override { return establish(false); }
    bool reconnect() override { close(); return establish(true); }
    void close() override { delete m_in; m_in = nullptr; }

    Sink* open_write(const Str& name) override {
        if (!m_in && !establish(false)) return nullptr;
        Sink* w = m_in->open_write(name);
        if (!w) { relay(); return nullptr; }
        return new SmbSink(w);
    }
    Source* open_read(const Str& name) override {
        if (!m_in && !establish(false)) return nullptr;
        Source* r = m_in->open_read(name);
        if (!r) { relay(); return nullptr; }
        return new SmbSource(r);
    }
    bool list(Vec<Str>& names) override { if (!m_in && !establish(false)) return false; if (m_in->list(names)) return true; relay(); return false; }
    bool remove(const Str& name) override { if (!m_in && !establish(false)) return false; if (m_in->remove(name)) return true; relay(); return false; }
    bool free_space(u64& bytes) override {
        ULARGE_INTEGER avail, total, freeb;
        Str p = Mapped(m_folder); if (!p.ends_with("\\")) p += "\\";
        if (!GetDiskFreeSpaceExW(FilePath(p), &avail, &total, &freeb)) return false;
        bytes = avail.QuadPart; return true;
    }

private:
    void relay() { err = m_in->err; transient = SmbIsTransientCode(SmbErrorCodeOf(err)); }

    bool establish(bool reset) {
        delete m_in; m_in = nullptr;
        Str e; bool tr = false;
        Str host, sub, root2; SmbSplitUnc(m_root, root2, sub, host);
        if (!m_conn.open(m_root, m_user, m_pass, reset, e, tr)) {
            unsigned long ec = SmbErrorCodeOf(e);
            if (SmbIsAuthCode(ec)) return failAuth(e, (int)ec);
            return fail(e, tr);
        }
        // is the share there for this account? (a name that does not resolve is told here, with the Windows reason)
        Str mroot = Mapped(m_root);
        DWORD a = GetFileAttributesW(FilePath(mroot));
        if (a == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError(); int c = m_root.rfind('\\');
            Str t = SmbErrorText(code, host, c >= 0 ? m_root.substr((size_t)c + 1) : Str(), tr);
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) { t = "the share " + m_root + " cannot be opened: " + WinErrorText(code); tr = false; }
            return fail(t, tr);
        }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return fail(m_root + " is not a folder");
        Json c = Json::object(); c["path"] = Mapped(m_folder);
        Str ce; Target* in = NewLocalTarget(c, ce);
        if (!in) return fail(ce);
        if (!in->connect()) { Str x = in->err; delete in; return fail(x, SmbIsTransientCode(SmbErrorCodeOf(x))); }
        m_in = in;
        err.clear(); transient = false;
        return true;
    }

    Str m_root, m_folder, m_user, m_pass;
    SmbConnection m_conn;
    Target* m_in;
};

} // namespace

Target* NewSmbTarget(const Json& cfg, Str& err)
{
    Str path = cfg.get("path").str(), unc, e;
    if (path.trim().empty()) { err = "network folder: the path is not set"; return nullptr; }
    if (!SmbNormalizeUnc(path, unc, e)) { err = "network folder: " + e; return nullptr; }
    Str root, sub, host; SmbSplitUnc(unc, root, sub, host);
    Str rel;
    if (!SmbRelativeDir(cfg.get("directory").str(), rel, e)) { err = "network folder: " + e; return nullptr; }
    Str folder = root;
    if (!sub.empty()) folder += "\\" + sub;
    if (!rel.empty()) folder += "\\" + rel;
    if (folder.size() > kMaxUncBytes) { err = "network folder: the path is too long"; return nullptr; }
    Str user = cfg.get("user").str().trim();
    if (user.size() > 256 || user.find('\n') >= 0 || user.find('\r') >= 0 || user.find('\0') >= 0) { err = "network folder: the user name is not valid"; return nullptr; }
    return new SmbTarget(root, folder, user, cfg.get("password").str());
}

#include "netbrowse.inc"

} // namespace vm2b
