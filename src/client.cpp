#include "client.h"
using ui::Str; using ui::Json;

Client g_client;
Str g_pipeName = "VM2BackupService";                       // the pipe the program talks to (/pipe <name> overrides it: tests)

ui::Str g_expectServerImage;

Client::Client() : m_online(false), m_fail(vm2b::IPC_OK), m_holderPid(0) { InitializeCriticalSection(&m_cs); }
Client::~Client() { DeleteCriticalSection(&m_cs); }

void Client::rememberFailure(bool ok)
{
    m_fail = m_cli.failure(); m_holderPid = ok ? 0 : m_cli.serverPid(); m_holderImage = ok ? Str() : m_cli.serverImage();
}

bool Client::connect(int timeoutMs)
{
    EnterCriticalSection(&m_cs);
    Str err; m_cli.expectServerImage(g_expectServerImage); m_online = m_cli.connect(g_pipeName, timeoutMs, err); rememberFailure(m_online);
    LeaveCriticalSection(&m_cs);
    return m_online;
}

bool Client::call(const char* cmd, const Json& args, Json& result, Str& err)
{
    EnterCriticalSection(&m_cs);
    bool ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // while the service is known to be down a call gives up quickly: every call and the 1 s poll of the window run on the UI thread, and
        // connect() keeps trying for the whole timeout when there is no pipe (a service that is just starting)
        if (!m_cli.connected()) {
            m_cli.expectServerImage(g_expectServerImage);
            bool c = m_cli.connect(g_pipeName, !m_online ? 120 : attempt ? 1500 : 1000, err); rememberFailure(c);
            if (!c) { m_online = false; break; }
        }
        m_online = true;
        ok = m_cli.call(cmd, args, result, err);
        if (ok || m_cli.connected()) break;                 // a refusal by the service is final; a broken pipe gets one more try
    }
    LeaveCriticalSection(&m_cs);
    return ok;
}

struct AsyncJob { Client* c; HWND h; int tag; Str cmd; Json args; };

// The result `r` is handed to the receiving window (WM_APP_ASYNC handler frees it); the analyzer does not follow ownership through PostMessage.
[[clang::suppress]] static DWORD WINAPI AsyncThread(LPVOID p)
{
    AsyncJob* j = (AsyncJob*)p;
    AsyncResult* r = new AsyncResult; r->tag = j->tag;
    // a call of its own connection: a long call must not hold up the polling of the main window
    vm2b::IpcClient own; Str err; own.expectServerImage(g_expectServerImage);
    r->ok = false;
    if (own.connect(g_pipeName, 3000, err)) r->ok = own.call(j->cmd.c_str(), j->args, r->result, r->err); else r->err = err;
    if (!PostMessageW(j->h, WM_APP_ASYNC, (WPARAM)j->tag, (LPARAM)r)) delete r;
    delete j;
    return 0;
}

void Client::callAsync(HWND hwnd, int tag, const char* cmd, const Json& args)
{
    AsyncJob* j = new AsyncJob{ this, hwnd, tag, Str(cmd), args };
    HANDLE t = CreateThread(nullptr, 0, AsyncThread, j, 0, nullptr);
    if (t) CloseHandle(t); else delete j;
}

Str ServiceError(const Str& err)
{
    if (err.starts_with(vm2b::kWrongServerPrefix)) return Str(ui::U8(TR("Another program holds the control channel of the VM2Backup service, so this program does not talk to it. Close that program, then restart the service.")));
    if (err.find("not running") >= 0 || err.find("cannot connect") >= 0) return Str(ui::U8(TR("The VM2Backup service is not running. Start it with the \"Start service\" button.")));
    return err;
}
