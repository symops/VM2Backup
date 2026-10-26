// VM2Backup — the SSH-2 client of the SCP target (target_ssh.cpp): a thin layer over libssh2 (OpenSSL backend, static, built by
// tools/deps.sh into deps/out/<arch>). libssh2 does the protocol and the cryptography; this layer adds what the program needs on top:
// the socket (non-blocking, with an abort flag polled every 100 ms, a connect timeout, an idle timeout and keepalives), the host key
// policy (pinned SHA-256 fingerprints), authentication in one place (publickey, password, keyboard-interactive), and error messages
// with a `transient` flag (a retry may help) that never contain a secret.
#pragma once
#include "stream.h"
#include "target.h"

namespace vm2b {

struct SshConfig {
    Str host; int port; Str user, password, keyFile, keyPassphrase;
    Str keyText;                         // the private key itself (PEM / OpenSSH text), used instead of keyFile when set
    Str hostKey;                         // "SHA256:..." as ssh-keygen prints it
    Vec<Str> knownFingerprints;          // more accepted fingerprints
    bool insecureAcceptHostKey;
    int connectTimeoutMs, ioTimeoutMs, keepaliveMs;
    u64 rekeyBytes;                      // ignored: libssh2 does not start a key exchange by itself (the server's rekey requests are answered)
    // comma lists that replace the default preference (tests; also lets an admin force an algorithm)
    Str kexList, hostKeyList, cipherList, macList;
    SshConfig() : port(22), insecureAcceptHostKey(false), connectTimeoutMs(15000), ioTimeoutMs(60000), keepaliveMs(15000), rekeyBytes(0) {}
};

class SshSession;

class SshChannel {
public:
    ~SshChannel();
    bool exec(const Str& command);
    // stdout: >0 bytes, 0 = end of data (EOF / closed), -1 = error
    i64  read(void* p, size_t n);
    bool read_exact(void* p, size_t n);          // false on error or if the data ends first
    bool write(const void* p, size_t n);
    bool send_eof();
    // Waits until the server closes the channel (it does so after it has sent the exit status), at most graceMs (-1 = the io timeout).
    bool wait_close(int graceMs = -1);
    void close();                                // sends CLOSE; the object stays valid for reading the leftovers
    int  exit_status() const { return m_exitStatus; }          // -1 = the channel is not closed yet
    const Bytes& stderr_data();                  // what the command wrote to stderr (collected when asked, never in between: libssh2 wants a pending call repeated unchanged)
    Str err; bool transient;
private:
    friend class SshSession;
    SshChannel(SshSession* s, void* ch);
    void drain_stderr();
    bool at_eof();
    bool fail(const Str& e, bool temp) { err = e; transient = temp; return false; }
    bool fail_rc(int rc, const char* what);
    SshSession* m_s; void* m_ch; Bytes m_errBuf; int m_exitStatus; bool m_closeSent;
};

class SshSession {
public:
    SshSession();
    ~SshSession();
    bool connect(const SshConfig& cfg);          // TCP, handshake, host key check, authentication
    void disconnect();
    bool connected() const { return m_state == St_Ready; }
    bool idle_tick();                            // call now and then while the caller is busy elsewhere (an scp upload spooling to disk): sends the keepalive when it is due
    SshChannel* open_session();                  // owned by the caller; delete it before the session
    // Runs a command to completion: stdout / stderr / exit status. maxOut bounds the output (error when exceeded).
    bool exec(const Str& command, Bytes& out, Bytes& errOut, int& status, size_t maxOut = (size_t)16 << 20);
    void set_abort_flag(volatile long* f);
    Str err; bool transient;
    bool hostKeyUntrusted;                       // connect() failed because the host key is not known (fingerprint is in err and in fingerprint)
    Str fingerprint, serverVersion, kexName, cipherName, macName, hostKeyName;
    u64 rekeys;                                  // always 0 (kept for the tests)
    void test_rekey() {}
    struct Impl;                                 // internal (ssh.cpp)
private:
    friend class SshChannel;
    enum State { St_Closed, St_Ready, St_Dead };
    bool fail(const Str& e, bool temp) { err = e; transient = temp; return false; }
    bool fail_rc(int rc, const char* what);      // maps a libssh2 error code to err / transient
    bool connect_socket(const SshConfig& cfg);
    bool authenticate(const SshConfig& cfg);
    Impl* m_i;
    State m_state; volatile long* m_abort;
};

// Called by the targets: pointer to a flag that aborts blocking calls when set (see SshSession::set_abort_flag).
void SshTargetSetAbortFlag(Target* t, volatile long* flag);

} // namespace vm2b
