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
private:
    vm2b::IpcClient m_cli; bool m_online; CRITICAL_SECTION m_cs;
};

extern Client g_client;
extern ui::Str g_pipeName;
ui::Str ServiceError(const ui::Str& err);                  // a friendly text for "the service is not running"
