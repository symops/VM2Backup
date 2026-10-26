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
    bool start(const Str& pipeName, IpcHandler* h, Str& err, const char* sddl = nullptr);
    void stop();
    int clients() const { return m_clients; }
private:
    static DWORD WINAPI AcceptThread(LPVOID p);
    static DWORD WINAPI ClientThread(LPVOID p);
    void serve(HANDLE pipe);
    Str m_name; IpcHandler* m_h; HANDLE m_thread; volatile LONG m_stop; volatile LONG m_clients; PSECURITY_DESCRIPTOR m_sd;
    CRITICAL_SECTION m_cs; HANDLE m_open[64]; int m_nopen;
    friend struct ClientArg;
};

class IpcClient {
public:
    IpcClient();
    ~IpcClient();
    bool connect(const Str& pipeName, int timeoutMs, Str& err);
    bool call(const char* cmd, const Json& args, Json& result, Str& err, int timeoutMs = 0);   // err holds the service's text on a refusal
    bool connected() const { return m_h != INVALID_HANDLE_VALUE; }
    void close();
private:
    HANDLE m_h;
};

} // namespace vm2b
