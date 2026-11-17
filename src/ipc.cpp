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

IpcServer::IpcServer() : m_h(nullptr), m_thread(nullptr), m_stop(0), m_clients(0), m_waiting(0), m_sd(nullptr), m_nopen(0) { InitializeCriticalSection(&m_cs); }
IpcServer::~IpcServer() { stop(); DeleteCriticalSection(&m_cs); }

// Who holds a pipe name: "process 1234 (C:\\x\\y.exe)" - found through a client connection (when the pipe's security lets this process in).
static Str PipeHolder(const Str& name)
{
    HANDLE h = CreateFileW(Wide(PipePath(name)), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Str();
    ULONG pid = 0; Str out;
    if (GetNamedPipeServerProcessId(h, &pid) && pid) {
        out = Str::fmt("process %lu", (unsigned long)pid);
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (p) { wchar_t img[MAX_PATH * 2]; DWORD n = MAX_PATH * 2; if (QueryFullProcessImageNameW(p, 0, img, &n)) out += " (" + FromWide(img) + ")"; CloseHandle(p); }
    }
    CloseHandle(h); return out;
}

// PIPE_REJECT_REMOTE_CLIENTS: the pipe is not offered to other computers over SMB (only the ACL kept them out before).
static const DWORD kPipeMode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | 0x00000008 /* PIPE_REJECT_REMOTE_CLIENTS */;

bool IpcServer::start(const Str& pipeName, IpcHandler* h, Str& err, const char* sddl, bool keepTrying)
{
    m_name = pipeName; m_h = h; m_stop = 0; m_waiting = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Wide(sddl ? sddl : "D:(A;;GA;;;BA)(A;;GA;;;SY)"), SDDL_REVISION_1, &m_sd, nullptr)) {
        err = Str::fmt("cannot build the pipe security descriptor (%lu)", (unsigned long)GetLastError()); return false;
    }
    // the first instance is made here so that a second service (or a stale pipe) is noticed at once; FILE_FLAG_FIRST_PIPE_INSTANCE stays: it is what keeps a
    // program that was there first (and made its own pipe with an access list of its own) from being taken for the service's pipe
    SECURITY_ATTRIBUTES sa = { sizeof(sa), m_sd, FALSE };
    HANDLE first = CreateNamedPipeW(Wide(PipePath(m_name)), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, kPipeMode, PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
    if (first == INVALID_HANDLE_VALUE) {
        DWORD le = GetLastError();
        bool held = le == ERROR_ACCESS_DENIED || le == ERROR_PIPE_BUSY || le == ERROR_ALREADY_EXISTS;
        Str who = held ? PipeHolder(m_name) : Str();
        if (held && keepTrying) { m_waiting = 1; first = INVALID_HANDLE_VALUE; }              // the acceptor takes the name when it is free
        else {
            err = Str::fmt("cannot create the pipe %s (%lu): is the service already running?", m_name.c_str(), (unsigned long)le);
            if (!who.empty()) err += " The name is held by " + who + ".";
            LocalFree(m_sd); m_sd = nullptr; return false;
        }
    }
    ClientArg* a = new ClientArg{ this, first };
    m_thread = CreateThread(nullptr, 0, AcceptThread, a, 0, nullptr);
    return m_thread != nullptr;
}

DWORD WINAPI IpcServer::AcceptThread(LPVOID p)
{
    ClientArg* a = (ClientArg*)p; IpcServer* s = a->srv; HANDLE pipe = a->pipe; delete a;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), s->m_sd, FALSE };
    while (pipe == INVALID_HANDLE_VALUE && !s->m_stop) {                          // the name was held when the service started: take it as soon as it is free
        pipe = CreateNamedPipeW(Wide(PipePath(s->m_name)), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, kPipeMode, PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
        if (pipe != INVALID_HANDLE_VALUE) { InterlockedExchange(&s->m_waiting, 0); break; }
        Sleep(500);
    }
    if (pipe == INVALID_HANDLE_VALUE) return 0;
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
        pipe = CreateNamedPipeW(Wide(PipePath(s->m_name)), PIPE_ACCESS_DUPLEX, kPipeMode, PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
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
const char kWrongServerPrefix[] = "wrong server: ";

IpcClient::IpcClient() : m_h(INVALID_HANDLE_VALUE), m_fail(IPC_OK), m_srvPid(0) {}
IpcClient::~IpcClient() { close(); }
void IpcClient::close() { if (m_h != INVALID_HANDLE_VALUE) { CloseHandle(m_h); m_h = INVALID_HANDLE_VALUE; } }

// The program file registered for the service (the SCM's image path without quotes and arguments); without a registration VM2BackupService.exe next to this program.
Str IpcClient::ServiceImagePath()
{
    Str img;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm) {
        SC_HANDLE svc = OpenServiceW(scm, L"VM2BackupService", SERVICE_QUERY_CONFIG);
        if (svc) {
            DWORD need = 0; QueryServiceConfigW(svc, nullptr, 0, &need);
            if (need && need < 65536) {
                Bytes b; b.resize(need); QUERY_SERVICE_CONFIGW* qc = (QUERY_SERVICE_CONFIGW*)b.data();
                if (QueryServiceConfigW(svc, qc, need, &need) && qc->lpBinaryPathName) {
                    Str p = FromWide(qc->lpBinaryPathName).trim();
                    if (!p.empty() && p[0] == '"') { int e = p.find('"', 1); img = e > 0 ? p.substr(1, (size_t)e - 1) : p.substr(1); }
                    else { int e = p.lower().find(".exe"); img = e > 0 ? p.substr(0, (size_t)e + 4) : p; }
                }
            }
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    if (img.empty()) { wchar_t me[MAX_PATH * 2]; DWORD n = GetModuleFileNameW(nullptr, me, MAX_PATH * 2); if (n && n < MAX_PATH * 2) { Str m = FromWide(me); int cut = m.rfind('\\'); if (cut > 0) img = m.substr(0, (size_t)cut + 1) + "VM2BackupService.exe"; } }
    return img;
}

static Str NormImage(const Str& p) { wchar_t full[MAX_PATH * 2]; DWORD n = GetFullPathNameW(Wide(p), MAX_PATH * 2, full, nullptr); return (n && n < MAX_PATH * 2 ? FromWide(full) : p).lower(); }

// Is the process at the other end of the pipe the expected program?  "" = yes; else the reason (it starts with kWrongServerPrefix); pid / img = what is known about it.
static Str CheckPipeServer(HANDLE h, const Str& expect, ULONG& pid, Str& img)
{
    pid = 0; img.clear(); Str pre = kWrongServerPrefix;
    if (!GetNamedPipeServerProcessId(h, &pid) || !pid) { pid = 0; return pre + Str::fmt("cannot tell which program is at the other end of the pipe (%lu)", (unsigned long)GetLastError()); }
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return pre + Str::fmt("cannot look at the program at the other end of the pipe (process %lu, error %lu)", (unsigned long)pid, (unsigned long)GetLastError());
    wchar_t w[MAX_PATH * 2]; DWORD n = MAX_PATH * 2; BOOL ok = QueryFullProcessImageNameW(p, 0, w, &n); CloseHandle(p);
    if (!ok) return pre + Str::fmt("cannot read the program file of the process at the other end of the pipe (process %lu)", (unsigned long)pid);
    img = FromWide(w);
    if (NormImage(img) != NormImage(expect)) return pre + "the program at the other end of the pipe is not the VM2Backup service (process " + Str::fmt("%lu", (unsigned long)pid) + ", " + img + "): nothing was sent to it";
    return Str();
}

bool IpcClient::connect(const Str& name, int timeoutMs, Str& err)
{
    close(); m_fail = IPC_OK; m_srvPid = 0; m_srvImage.clear();
    Wide path(PipePath(name));
    Str expect = m_expect; if (expect.empty() && name == kDefaultPipeName) expect = ServiceImagePath();
    DWORD t0 = GetTickCount();
    for (;;) {
        // SECURITY_IDENTIFICATION: whatever is at the other end can learn who connects, not act as him
        m_h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (m_h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(path, 200);
        else if (e == ERROR_ACCESS_DENIED) { err = "access denied: run the program as administrator"; m_fail = IPC_DENIED; return false; }
        else Sleep(100);
        if ((int)(GetTickCount() - t0) >= timeoutMs) { m_fail = e == ERROR_FILE_NOT_FOUND ? IPC_NOT_RUNNING : IPC_OTHER; err = e == ERROR_FILE_NOT_FOUND ? "the VM2Backup service is not running" : Str::fmt("cannot connect to the service (%lu)", (unsigned long)e); return false; }
    }
    if (!expect.empty()) {
        ULONG pid = 0; Str img; Str why = CheckPipeServer(m_h, expect, pid, img);
        if (!why.empty()) { close(); err = why; m_fail = IPC_WRONG_SERVER; m_srvPid = pid; m_srvImage = img; return false; }
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
