// VM2Backup — the program's side of the link to the service (ipc.h): synchronous calls, and asynchronous ones for what can take
// long (testing a connection, listing the VMs of a remote host) so that the window never freezes.
#pragma once
#include "ipc.h"
#include "ui.h"

#define WM_APP_ASYNC (WM_APP + 41)                 // lParam = AsyncResult* (the receiver deletes it)

struct AsyncResult { int tag; bool ok; vm2b::Json result; ui::Str err; };

class Client {
public:
    Client();
    ~Client();
    bool connect(int timeoutMs);                    // opens the pipe; false with lastError()
    bool call(const char* cmd, const ui::Json& args, ui::Json& result, ui::Str& err);
    // the answer arrives as WM_APP_ASYNC (wParam = tag) on `hwnd`
    void callAsync(HWND hwnd, int tag, const char* cmd, const ui::Json& args);
    bool online() const { return m_online; }
    void close() { EnterCriticalSection(&m_cs); m_cli.close(); LeaveCriticalSection(&m_cs); }       // the next call connects again (tests: to meet a changed rule at once)
    // Why the last connection attempt failed (the panel tells "no service" from "another program holds the pipe name"): IPC_WRONG_SERVER is a program that is not the VM2Backup service
    vm2b::IpcFailure failure() const { return m_fail; }
    unsigned long holderPid() const { return m_holderPid; }
    ui::Str holderImage() const { return m_holderImage; }
private:
    void rememberFailure(bool ok);
    vm2b::IpcClient m_cli; bool m_online; CRITICAL_SECTION m_cs;
    vm2b::IpcFailure m_fail; unsigned long m_holderPid; ui::Str m_holderImage;
};

extern Client g_client;
extern ui::Str g_pipeName;
extern ui::Str g_expectServerImage;                        // tests only: the program file the other end of the pipe must have (the check of the default pipe name for any pipe); "" = the default rule
ui::Str ServiceError(const ui::Str& err);                  // a friendly text for "the service is not running"
