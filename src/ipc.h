// VM2Backup — the link between the program (window) and the service: a named pipe in message mode, one JSON message per
// request and per answer.
//   request  {"cmd":"tasks.run","args":{...}}        answer  {"ok":true,"result":...}   or   {"ok":false,"error":"text"}
// By default only Administrators and SYSTEM may open the pipe: the service runs the tasks as SYSTEM, so whoever can talk to it
// can make it read and write files with those rights.
#pragma once
#include "base.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace vm2b {

extern const char* const kDefaultPipeName;                 // "VM2BackupService"

struct IpcHandler {
    virtual ~IpcHandler() {}
    // Returns true with `result` filled, or false with `err`. Called on a thread of its own per connected client.
    virtual bool handle(const Str& cmd, const Json& args, Json& result, Str& err) = 0;
};

class IpcServer {
public:
    IpcServer();
    ~IpcServer();
    // sddl: who may connect; null = Administrators and SYSTEM.
    // keepTrying: when the name is held by another process (a squatter, a stale instance) the start does not fail: the acceptor keeps trying to take the name every
    // half second, `waiting()` tells; without it (the default, the tests) a held name is an error that names the holder.
    bool start(const Str& pipeName, IpcHandler* h, Str& err, const char* sddl = nullptr, bool keepTrying = false);
    bool waiting() const { return m_waiting != 0; }
    void stop();
    int clients() const { return m_clients; }
private:
    static DWORD WINAPI AcceptThread(LPVOID p);
    static DWORD WINAPI ClientThread(LPVOID p);
    void serve(HANDLE pipe);
    Str m_name; IpcHandler* m_h; HANDLE m_thread; volatile LONG m_stop; volatile LONG m_clients; volatile LONG m_waiting; PSECURITY_DESCRIPTOR m_sd;
    CRITICAL_SECTION m_cs; HANDLE m_open[64]; int m_nopen;
    friend struct ClientArg;
};

class IpcClient {
public:
    IpcClient();
    ~IpcClient();
    // On the default pipe name the other end must be the VM2Backup service (security review F9): its process is looked up (GetNamedPipeServerProcessId) and its program
    // file must be the one registered for the service (or, without a registration, VM2BackupService.exe next to this program); otherwise the connection is dropped before
    // anything (passwords of servers, folders) is sent.  A custom name (tests, /pipe) is not checked unless expectServerImage() is called.
    bool connect(const Str& pipeName, int timeoutMs, Str& err);
    void expectServerImage(const Str& path) { m_expect = path; }      // "" = the default rule above
    static Str ServiceImagePath();                                    // the registered program file of the service, else the one next to this program
    bool call(const char* cmd, const Json& args, Json& result, Str& err, int timeoutMs = 0);   // err holds the service's text on a refusal
    bool connected() const { return m_h != INVALID_HANDLE_VALUE; }
    void close();
private:
    HANDLE m_h; Str m_expect;
};

} // namespace vm2b
