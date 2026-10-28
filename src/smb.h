// VM2Backup — network folders (SMB / CIFS, UNC paths) and network drives.
//   * SmbNormalizeUnc: checks and normalises what the user typed (\\host\share[\folder]).
//   * SmbConnection: the connection to a share made with WNetAddConnection2W (no drive letter), cancelled when it is destroyed.
//   * NewSmbTarget (target_smb.cpp): a backup target = the local-folder target on the UNC path + connect / reconnect / error classes.
//   * network drives: the GUI turns a mapped drive (Z:\x) into its UNC path; a service cannot see the letters of other logon sessions.
// The WNet calls are reached through a table (SmbSetApi) so that the tests can run without a real SMB client.
#pragma once
#include "base.h"
#include "target.h"

namespace vm2b {

// "\\host\share\sub\dir" from "//host/share/sub/dir" etc. Rejects \\?\ and \\.\ paths, drive paths, relative paths, "..", reserved
// names, trailing dots / spaces, control characters and over-long input. On success out has no trailing backslash.
bool SmbNormalizeUnc(const Str& in, Str& out, Str& err);
// Splits a normalised path: root "\\host\share", sub "dir\x" (without the leading backslash, may be empty), host "host".
bool SmbSplitUnc(const Str& unc, Str& root, Str& sub, Str& host);
// A folder inside the share, relative ("a/b", "./x\y"): the normalised "a\b"; ".." and absolute / drive forms are refused.
bool SmbRelativeDir(const Str& in, Str& out, Str& err);
bool SmbIsUncPath(const Str& p);                        // starts with two slashes (either kind) and is not a \\?\ / \\.\ path

// What the program needs from the Windows network API. Replaced in the tests (nullptr = the real one).
struct SmbApi {
    unsigned long (*add)(const wchar_t* remote, const wchar_t* user, const wchar_t* pass);    // 0 = connected, else a Win32 error
    unsigned long (*cancel)(const wchar_t* remote, int force);
    Str (*mapPath)(const Str& unc);                      // the path the file operations use (the fake maps a share to a local folder)
    // Windows answers 53 ("network path not found") both for a server that does not answer and, with some servers (Samba), for a share that does not exist.
    // probe(host) = true when the server answers on the SMB port: then a 53 is reported as 67 (no such share, permanent) instead of "cannot reach the server"
    // (transient, retried for minutes). nullptr = no probe (the fake of the tests).
    bool (*probe)(const Str& host);
};
void SmbSetApi(const SmbApi* api);
Str SmbMapPath(const Str& unc);                          // what the file operations should open for a UNC path (the path itself, except in the tests)
// For the tests: how many connections of ours are open (made and not yet cancelled).
int SmbOpenConnections();
// True when one of our connections (a running backup, another browse) is open to this server: Windows allows one set of credentials per server.
bool SmbServerHeld(const Str& host);

// An open connection to a share. open() may be repeated (a lost connection); the destructor / release() cancels it when we made it.
class SmbConnection {
public:
    SmbConnection() : m_held(false), m_gen(0) {}
    ~SmbConnection() { release(); }
    // user empty = the rights of the account the program runs as. reset = drop an old (stale) connection first.
    bool open(const Str& root, const Str& user, const Str& password, bool reset, Str& err, bool& transient);
    void release();
    bool held() const { return m_held; }
private:
    SmbConnection(const SmbConnection&);
    SmbConnection& operator=(const SmbConnection&);
    bool m_held; Str m_root; int m_gen;       // m_gen: which registration of the share this hold belongs to (a cancelled one is not ours to release)
};

// Explains a Win32 error of a network operation in English (no secrets); transient = a retry may help.
Str SmbErrorText(unsigned long code, const Str& host, const Str& share, bool& transient);
// The Win32 error number at the end of "...: Access is denied. (5)" (0 when there is none).
unsigned long SmbErrorCodeOf(const Str& text);
bool SmbIsTransientCode(unsigned long code);
// The codes that mean "the credentials were refused" (wrong / locked / disabled / expired password, logon not allowed): 86, 1326, 1909, 1331, 1330, 1907, 1385, 1327, 1328.
// 1219 (a conflict with another connection of this computer) and 5 (access denied for a valid account) are NOT among them.
bool SmbIsAuthCode(unsigned long code);
// The code behind ERROR_EXTENDED_ERROR (WNetGetLastError): a Win32 code stays, an NTSTATUS (high bit) becomes its Win32 code (0xC0000071 -> 1330, 0xC0000224 -> 1907, 0xC0000234 -> 1909, 0xC000006D -> 1326)
unsigned long SmbMapProviderError(unsigned long pe);
// A result of WNetAddConnection2W: ERROR_EXTENDED_ERROR (1208) is replaced by the provider's own code (WNetGetLastError, mapped); any other value stays
unsigned long SmbResolveExtendedError(unsigned long rc);

// Network drives of the logged-in user (GUI): letters ("Z:") and the UNC path each one points at.
void SmbListNetworkDrives(Vec<Str>& letters, Vec<Str>& uncs);
// Test hook for the window test: pretend these are the network drives (see target_smb.cpp); nullptr = the real ones.
void SmbSetFakeDrives(const char* spec);
// "Z:\folder" -> "\\host\share\folder" when Z: is a mapped network drive; false (err set) otherwise.
bool SmbResolveDrivePath(const Str& path, Str& unc, Str& err);
bool SmbHasDriveLetter(const Str& p);                  // "Z:" or "Z:\..."
// Empty when the drive of the path is usable; otherwise the clear message for a drive the service cannot see.
Str SmbDriveUnavailableMessage(const Str& path);

// The target: {"type":"smb","path":"\\\\host\\share[\\dir]","user":"DOMAIN\\u","password":"...","directory":"relative\\folder"}
Target* NewSmbTarget(const Json& cfg, Str& err);

} // namespace vm2b
