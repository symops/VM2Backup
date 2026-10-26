#include "ipc.h"
#include "winutil.h"
#include <sddl.h>

namespace vm2b {

const char* const kDefaultPipeName = "VM2BackupService";
static const size_t kMaxMessage = 64u << 20;

static Str PipePath(const Str& name) { return Str("\\\\.\\pipe\\") + name; }

static bool ReadMessage(HANDLE h, Bytes& out)
{
    out.clear();
    u8 buf[65536];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &got, nullptr);
        DWORD e = ok ? 0 : GetLastError();
        if (!ok && e != ERROR_MORE_DATA) return false;
        if (out.size() + got > kMaxMessage) return false;
        size_t o = out.size(); out.resize(o + got); if (got) memcpy(out.data() + o, buf, got);
        if (ok) return true;
    }
}

static bool WriteMessage(HANDLE h, const Str& s)
{
    DWORD w = 0;
    // a message-mode write is one message however long it is
    return WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
}

// ---- server ----------------------------------------------------------------------------------------------------
struct ClientArg { IpcServer* srv; HANDLE pipe; };

IpcServer::IpcServer() : m_h(nullptr), m_thread(nullptr), m_stop(0), m_clients(0), m_sd(nullptr), m_nopen(0) { InitializeCriticalSection(&m_cs); }
IpcServer::~IpcServer() { stop(); DeleteCriticalSection(&m_cs); }

bool IpcServer::start(const Str& pipeName, IpcHandler* h, Str& err, const char* sddl)
{
    m_name = pipeName; m_h = h; m_stop = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Wide(sddl ? sddl : "D:(A;;GA;;;BA)(A;;GA;;;SY)"), SDDL_REVISION_1, &m_sd, nullptr)) {
        err = Str::fmt("cannot build the pipe security descriptor (%lu)", (unsigned long)GetLastError()); return false;
    }
    // the first instance is made here so that a second service (or a stale pipe) is noticed at once
    SECURITY_ATTRIBUTES sa = { sizeof(sa), m_sd, FALSE };
    HANDLE first = CreateNamedPipeW(Wide(PipePath(m_name)), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
    if (first == INVALID_HANDLE_VALUE) { err = Str::fmt("cannot create the pipe %s (%lu): is the service already running?", m_name.c_str(), (unsigned long)GetLastError()); LocalFree(m_sd); m_sd = nullptr; return false; }
    ClientArg* a = new ClientArg{ this, first };
    m_thread = CreateThread(nullptr, 0, AcceptThread, a, 0, nullptr);
    return m_thread != nullptr;
}

DWORD WINAPI IpcServer::AcceptThread(LPVOID p)
{
    ClientArg* a = (ClientArg*)p; IpcServer* s = a->srv; HANDLE pipe = a->pipe; delete a;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), s->m_sd, FALSE };
    for (;;) {
        BOOL c = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (s->m_stop) { CloseHandle(pipe); break; }
        if (c) {
            EnterCriticalSection(&s->m_cs); if (s->m_nopen < 64) s->m_open[s->m_nopen++] = pipe; LeaveCriticalSection(&s->m_cs);
            InterlockedIncrement(&s->m_clients);
            ClientArg* ca = new ClientArg{ s, pipe };
            HANDLE t = CreateThread(nullptr, 0, ClientThread, ca, 0, nullptr);
            if (t) CloseHandle(t); else { InterlockedDecrement(&s->m_clients); DisconnectNamedPipe(pipe); CloseHandle(pipe); delete ca; }
        } else CloseHandle(pipe);
        pipe = CreateNamedPipeW(Wide(PipePath(s->m_name)), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
        if (pipe == INVALID_HANDLE_VALUE) break;
    }
    return 0;
}

DWORD WINAPI IpcServer::ClientThread(LPVOID p)
{
    ClientArg* a = (ClientArg*)p; IpcServer* s = a->srv; HANDLE pipe = a->pipe; delete a;
    s->serve(pipe);
    EnterCriticalSection(&s->m_cs);
    for (int i = 0; i < s->m_nopen; ++i) if (s->m_open[i] == pipe) { s->m_open[i] = s->m_open[--s->m_nopen]; break; }
    LeaveCriticalSection(&s->m_cs);
    DisconnectNamedPipe(pipe); CloseHandle(pipe);
    InterlockedDecrement(&s->m_clients);
    return 0;
}

void IpcServer::serve(HANDLE pipe)
{
    Bytes msg;
    while (!m_stop && ReadMessage(pipe, msg)) {
        Json req, answer = Json::object(); Str err;
        if (!Json::parse((const char*)msg.data(), msg.size(), req, err) || !req.is_object()) { answer["ok"] = false; answer["error"] = "bad request: " + err; }
        else {
            Json result; Str e2;
            bool ok = m_h->handle(req.get("cmd").str(), req.get("args"), result, e2);
            answer["ok"] = ok;
            if (ok) answer["result"] = result; else answer["error"] = e2;
        }
        if (!WriteMessage(pipe, answer.dump())) break;
    }
}

void IpcServer::stop()
{
    if (!m_thread) return;
    InterlockedExchange(&m_stop, 1);
    // wake the acceptor up: it is blocked in ConnectNamedPipe
    HANDLE w = CreateFileW(Wide(PipePath(m_name)), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    WaitForSingleObject(m_thread, 5000);
    CloseHandle(m_thread); m_thread = nullptr;
    // connected clients: their reads fail when the pipe is closed under them
    EnterCriticalSection(&m_cs);
    for (int i = 0; i < m_nopen; ++i) CancelIoEx(m_open[i], nullptr);
    LeaveCriticalSection(&m_cs);
    for (int i = 0; i < 100 && m_clients > 0; ++i) { EnterCriticalSection(&m_cs); for (int k = 0; k < m_nopen; ++k) DisconnectNamedPipe(m_open[k]); LeaveCriticalSection(&m_cs); Sleep(50); }
    if (m_sd) { LocalFree(m_sd); m_sd = nullptr; }
}

// ---- client ----------------------------------------------------------------------------------------------------
IpcClient::IpcClient() : m_h(INVALID_HANDLE_VALUE) {}
IpcClient::~IpcClient() { close(); }
void IpcClient::close() { if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; } }

bool IpcClient::connect(const Str& name, int timeoutMs, Str& err)
{
    close();
    Wide path(PipePath(name));
    DWORD t0 = GetTickCount();
    for (;;) {
        m_h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(path, 200);
        else if (e == ERROR_ACCESS_DENIED) { err = "access denied: run the program as administrator"; return false; }
        else Sleep(100);
        if ((int)(GetTickCount() - t0) >= timeoutMs) { err = e == ERROR_FILE_NOT_FOUND ? "the VM2Backup service is not running" : Str::fmt("cannot connect to the service (%lu)", (unsigned long)e); return false; }
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(m_h, &mode, nullptr, nullptr);
    return true;
}

bool IpcClient::call(const char* cmd, const Json& args, Json& result, Str& err, int)
{
    if (m_h == INVALID_HANDLE_VALUE) { err = "not connected to the service"; return false; }
    Json req = Json::object(); req["cmd"] = cmd; req["args"] = args;
    if (!WriteMessage(m_h, req.dump())) { err = "the service has closed the connection"; close(); return false; }
    Bytes msg;
    if (!ReadMessage(m_h, msg)) { err = "the service has closed the connection"; close(); return false; }
    Json ans; Str e;
    if (!Json::parse((const char*)msg.data(), msg.size(), ans, e)) { err = "bad answer of the service: " + e; return false; }
    if (!ans.get("ok").boolean()) { err = ans.get("error").str("error"); return false; }
    result = ans.get("result");
    return true;
}

} // namespace vm2b
