// The administration web site: listeners, HTTP/1.1, routing, the admin commands.  See webserver.h and docs/WEB.md.
#include "webserver.h"
#include "webauth.h"
#include "webtls.h"
#include "svccore.h"
#include "crypto.h"
#include "winutil.h"
#include "version.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#endif
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

namespace vm2b {

#include "webui.inc"

namespace {

struct Guard { CRITICAL_SECTION* c; explicit Guard(CRITICAL_SECTION* x) : c(x) { EnterCriticalSection(c); } ~Guard() { LeaveCriticalSection(c); } };

const size_t kMaxHead = 16384;
const size_t kMaxBody = 8u << 20;
const int kMaxHeaders = 64;
const int kWorkers = 24;
const int kMaxQueue = 128;
const int kMaxPerIp = 32;
const int kMaxRequestsPerConn = 200;
const int kIdleKeepAliveMs = 10000;
const int kHeadMs = 15000;
const int kBodyMs = 60000;
const int kHandshakeMs = 10000;

bool IsWine()
{
    static int w = -1;
    if (w < 0) { HMODULE h = GetModuleHandleW(L"ntdll.dll"); w = (h && GetProcAddress(h, "wine_get_version")) ? 1 : 0; }
    return w == 1;
}

void WsInit() { static bool done = false; if (!done) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); done = true; } }

// ---- adapters (iphlpapi is loaded dynamically: no extra link library) -------------------------------------------------------------
typedef ULONG (WINAPI* GetAdaptersAddressesFn)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);

Json RealInterfaces()
{
    Json out = Json::array();
    HMODULE h = LoadLibraryW(L"iphlpapi.dll"); if (!h) return out;
    GetAdaptersAddressesFn fn = (GetAdaptersAddressesFn)GetProcAddress(h, "GetAdaptersAddresses");
    if (!fn) return out;
    Bytes buf; ULONG size = 32768; ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 4 && r == ERROR_BUFFER_OVERFLOW; ++i) { buf.resize(size); r = fn(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (PIP_ADAPTER_ADDRESSES)buf.data(), &size); }
    if (r != NO_ERROR) return out;
    for (PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf.data(); a; a = a->Next) {
        Json o = Json::object();
        o["name"] = Str(a->AdapterName ? a->AdapterName : ""); o["friendly"] = a->FriendlyName ? FromWide(a->FriendlyName) : Str(); o["description"] = a->Description ? FromWide(a->Description) : Str();
        o["up"] = a->OperStatus == IfOperStatusUp; o["loopback"] = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        Json addrs = Json::array();
        for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;
            char t[80] = ""; Json e = Json::object();
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                const sockaddr_in* s = (const sockaddr_in*)u->Address.lpSockaddr; inet_ntop(AF_INET, (void*)&s->sin_addr, t, sizeof(t));
                e["family"] = "ipv4"; e["link_local"] = (ntohl(s->sin_addr.s_addr) >> 16) == 0xA9FE;
            } else if (u->Address.lpSockaddr->sa_family == AF_INET6) {
                const sockaddr_in6* s = (const sockaddr_in6*)u->Address.lpSockaddr; inet_ntop(AF_INET6, (void*)&s->sin6_addr, t, sizeof(t));
                bool ll = s->sin6_addr.s6_addr[0] == 0xfe && (s->sin6_addr.s6_addr[1] & 0xc0) == 0x80;
                Str ts = t; if (ll) ts += Str::fmt("%%%lu", (unsigned long)(s->sin6_scope_id ? s->sin6_scope_id : a->Ipv6IfIndex));
                e["family"] = "ipv6"; e["link_local"] = ll; e["address"] = ts; addrs.push(e); continue;
            } else continue;
            e["address"] = Str(t); addrs.push(e);
        }
        o["addresses"] = addrs; out.push(o);
    }
    return out;
}

typedef DWORD (WINAPI* NotifyFn)(ADDRESS_FAMILY, void*, PVOID, BOOLEAN, HANDLE*);
typedef DWORD (WINAPI* CancelFn)(HANDLE);
void WINAPI OnNetChange(PVOID ctx, void*, int) { SetEvent((HANDLE)ctx); }

struct Want { Str entry, addr; };

} // namespace

// =================================================================================================================================
struct WebServer::Impl {
    struct Target { Str entry, addr; size_t sock; Str err; };
    struct EntryNote { Str entry, note; };
    struct QConn { size_t sock; Str ip; DWORD tick; };
    struct IpCount { Str ip; int n; };

    ServiceCore* core; WebAuth auth; Str dataDir;
    CRITICAL_SECTION adminCs, tcs, qcs, ipcs;
    Vec<Target> targets; Vec<EntryNote> notes; Vec<Str> entries; int portNum;
    WebTlsServer* tls; Str tlsError; Bytes certDer; bool selfSigned;
    volatile LONG stopFlag; HANDLE mgr; Vec<HANDLE> workers; HANDLE sem; HANDLE evt;
    Vec<QConn> queue; Vec<IpCount> ipCounts;
    Json (*ifProvider)(); int retrySec; bool running, enabled; volatile LONG trustProxy;
    HANDLE notify1, notify2; HMODULE iphlp; i64 lastReconcile;
    i64 (*clockFn)();
    WebServer* owner; volatile LONG applyPending, closed;
    struct WebCtx { bool viaWeb, applyLater; Str actor, ip, session; WebCtx() : viaWeb(false), applyLater(false) {} } ctx;     // set by handleAdminFromWeb under adminCs

    Impl() : core(nullptr), portNum(8443), tls(nullptr), selfSigned(false), stopFlag(0), mgr(nullptr), sem(nullptr), evt(nullptr), ifProvider(nullptr), retrySec(30),
             running(false), enabled(false), trustProxy(0), notify1(nullptr), notify2(nullptr), iphlp(nullptr), lastReconcile(0), clockFn(nullptr), owner(nullptr), applyPending(0), closed(0)
    { InitializeCriticalSection(&adminCs); InitializeCriticalSection(&tcs); InitializeCriticalSection(&qcs); InitializeCriticalSection(&ipcs); }
    ~Impl() { DeleteCriticalSection(&adminCs); DeleteCriticalSection(&tcs); DeleteCriticalSection(&qcs); DeleteCriticalSection(&ipcs); }

    Json interfaces() { return ifProvider ? ifProvider() : RealInterfaces(); }

    // is every address this bind entry can resolve to a loopback address?  (adapter entries: the adapter is a loopback one)
    bool entryIsLoopback(const Str& e)
    {
        if (e == "localhost") return true;
        if (e == "all") return false;
        if (e.starts_with("adapter:")) {
            Json ifs = interfaces();
            for (size_t k = 0; k < ifs.size(); ++k) if (ifs.at(k).get("name").str().lower() == e.substr(8).lower() && ifs.at(k).get("loopback").boolean()) return true;
            return false;
        }
        return WebAuth::IsLoopbackAddress(e);
    }

    // ---- resolving the bind entries into concrete addresses ----
    void resolve(Vec<Want>& want, Vec<EntryNote>& nt) { resolveEntries(entries, want, nt); }
    void resolveEntries(const Vec<Str>& list, Vec<Want>& want, Vec<EntryNote>& nt)
    {
        Json ifs; bool haveIfs = false;
        for (const Str& e : list) {
            if (e == "localhost") { want.push_back(Want{ e, "127.0.0.1" }); want.push_back(Want{ e, "::1" }); }
            else if (e == "all") { want.push_back(Want{ e, "0.0.0.0" }); want.push_back(Want{ e, "::" }); }
            else if (e.starts_with("adapter:")) {
                if (!haveIfs) { ifs = interfaces(); haveIfs = true; }
                Str g = e.substr(8).lower(); bool found = false, any = false;
                for (size_t i = 0; i < ifs.size(); ++i) {
                    const Json& a = ifs.at(i); if (a.get("name").str().lower() != g) continue;
                    found = true;
                    if (!a.get("up").boolean(true)) { nt.push_back(EntryNote{ e, "the adapter is down" }); break; }
                    const Json& ad = a.get("addresses");
                    for (size_t k = 0; k < ad.size(); ++k) { want.push_back(Want{ e, ad.at(k).get("address").str() }); any = true; }
                    if (!any) nt.push_back(EntryNote{ e, "the adapter has no addresses" });
                    break;
                }
                if (!found) nt.push_back(EntryNote{ e, "the adapter was not found" });
            } else want.push_back(Want{ e, e });
        }
    }

    bool tryBind(Target& t, int portToUse = 0)
    {
        char port[16]; snprintf(port, sizeof(port), "%d", portToUse ? portToUse : portNum);
        addrinfo hints; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
        addrinfo* res = nullptr;
        if (getaddrinfo(t.addr.c_str(), port, &hints, &res) != 0 || !res) { t.err = "not a valid address"; return false; }
        SOCKET s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { t.err = "cannot create a socket: " + WinErrorText((unsigned long)WSAGetLastError()); freeaddrinfo(res); return false; }
        int one = 1;
        if (res->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&one, sizeof(one));
        if (IsWine()) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));      // Linux keeps TIME_WAIT ports busy
        else setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));       // nobody else may share our port
        if (bind(s, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(s, 128) != 0) {
            int e = WSAGetLastError(); closesocket(s); freeaddrinfo(res);
            t.err = (e == WSAEADDRINUSE || e == WSAEACCES) ? Str("the port is already in use on this address (or access is denied)") : e == WSAEADDRNOTAVAIL ? Str("the address does not exist on this computer") : e == WSAEAFNOSUPPORT ? Str("this address family is not available") : WinErrorText((unsigned long)e);
            return false;
        }
        freeaddrinfo(res);
        t.sock = (size_t)s; t.err.clear();
        return true;
    }

    // A trial of a new bind list / port before it is stored: every entry must be able to listen on at least one of its addresses
    // (an address we already hold on the same port counts).  The OS error of the last failure is returned.
    bool trialBind(const Vec<Str>& list, int port, Str& err)
    {
        for (const Str& e : list) {
            Vec<Want> want; Vec<EntryNote> nt; Vec<Str> one; one.push_back(e); resolveEntries(one, want, nt);
            if (want.empty()) { err = "cannot listen on " + e + ": " + (nt.size() ? nt[0].note : Str("no address")); return false; }
            bool okAny = false; Str lastErr;
            for (const Want& w : want) {
                bool held = false;
                if (port == portNum) { Guard g(&tcs); for (const Target& t : targets) if (t.addr == w.addr && t.sock != (size_t)INVALID_SOCKET) held = true; }
                if (held) { okAny = true; continue; }
                Target t; t.entry = w.entry; t.addr = w.addr; t.sock = (size_t)INVALID_SOCKET;
                if (tryBind(t, port)) { closesocket((SOCKET)t.sock); okAny = true; } else lastErr = t.err;
            }
            if (!okAny) { err = Str::fmt("cannot listen on %s port %d: ", e.c_str(), port) + lastErr; return false; }
        }
        return true;
    }

    static DWORD WINAPI ApplyThread(LPVOID p)
    {
        Impl* m = (Impl*)p; Sleep(250);                       // the answer of the request that asked for it is on its way
        { Guard g(&m->adminCs); if (!m->closed) { Str e; m->apply(e); } }
        InterlockedDecrement(&m->applyPending);
        return 0;
    }
    void scheduleApply()
    {
        InterlockedIncrement(&applyPending);
        HANDLE h = CreateThread(nullptr, 0, ApplyThread, this, 0, nullptr);
        if (h) CloseHandle(h); else InterlockedDecrement(&applyPending);
    }

    // Brings the sockets in line with the entries: closes what is no longer wanted, binds what is missing.
    void reconcile()
    {
        Vec<Want> want; Vec<EntryNote> nt; resolve(want, nt);
        Guard g(&tcs);
        for (size_t i = targets.size(); i-- > 0;) {
            bool keep = false;
            for (const Want& w : want) if (w.entry == targets[i].entry && w.addr == targets[i].addr) keep = true;
            if (!keep) { if (targets[i].sock != (size_t)INVALID_SOCKET) closesocket((SOCKET)targets[i].sock); targets.erase(i); }
        }
        for (const Want& w : want) {
            bool have = false; for (const Target& t : targets) if (t.entry == w.entry && t.addr == w.addr) have = true;
            if (!have && targets.size() < 60) { Target t; t.entry = w.entry; t.addr = w.addr; t.sock = (size_t)INVALID_SOCKET; targets.push_back(t); }
        }
        for (Target& t : targets) if (t.sock == (size_t)INVALID_SOCKET) tryBind(t);
        notes = nt;
        lastReconcile = (i64)GetTickCount64() / 1000;
    }

    // ---- life cycle ----
    void loadSettings()
    {
        Json s = auth.settings();
        enabled = s.get("enabled").boolean(); portNum = (int)s.get("port").integer(8443);
        entries.clear(); const Json& b = s.get("bind"); for (size_t i = 0; i < b.size(); ++i) entries.push_back(b.at(i).str());
        InterlockedExchange(&trustProxy, s.get("trust_proxy").boolean() ? 1 : 0);
    }

    bool ensureIdentity(Str& err)
    {
        WebTlsIdentity id; bool ss = false;
        if (auth.identity(id, &ss)) { certDer = id.certDer; selfSigned = ss; return true; }
        Vec<Want> want; Vec<EntryNote> nt; resolve(want, nt);          // the concrete addresses chosen now go into the certificate
        Vec<Str> extra;
        for (const Want& w : want) { if (w.addr == "0.0.0.0" || w.addr == "::" || w.addr.find('%') >= 0) continue; extra.push_back(w.addr); }
        WebTlsIdentity gen;
        if (!WebTlsGenerate(extra, gen, err)) return false;
        if (!auth.setIdentity(gen, true, err)) return false;
        certDer = gen.certDer; selfSigned = true;
        return true;
    }

    void startNotify()
    {
        iphlp = LoadLibraryW(L"iphlpapi.dll"); if (!iphlp) return;
        NotifyFn n1 = (NotifyFn)GetProcAddress(iphlp, "NotifyIpInterfaceChange");
        NotifyFn n2 = (NotifyFn)GetProcAddress(iphlp, "NotifyUnicastIpAddressChange");
        if (n1) { HANDLE h = nullptr; if (n1(AF_UNSPEC, (void*)OnNetChange, evt, FALSE, &h) == NO_ERROR) notify1 = h; }
        if (n2) { HANDLE h = nullptr; if (n2(AF_UNSPEC, (void*)OnNetChange, evt, FALSE, &h) == NO_ERROR) notify2 = h; }
    }
    void stopNotify()
    {
        if (iphlp) {
            CancelFn c = (CancelFn)GetProcAddress(iphlp, "CancelMibChangeNotify2");
            if (c) { if (notify1) c(notify1); if (notify2) c(notify2); }
        }
        notify1 = notify2 = nullptr;
    }

    bool startListening(Str& err)
    {
        WsInit();
        loadSettings();
        tlsError.clear();
        if (!enabled) return true;
        if (!ensureIdentity(err)) { tlsError = err; return false; }
        WebTlsIdentity id; auth.identity(id);
        tls = new WebTlsServer();
        if (!tls->init(id, err)) { tlsError = err; delete tls; tls = nullptr; return false; }
        InterlockedExchange(&stopFlag, 0);
        sem = CreateSemaphoreW(nullptr, 0, 100000, nullptr); evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        reconcile();
        startNotify();
        running = true;
        mgr = CreateThread(nullptr, 0, ManagerThread, this, 0, nullptr);
        for (int i = 0; i < kWorkers; ++i) workers.push_back(CreateThread(nullptr, 0, WorkerThread, this, 0, nullptr));
        return true;
    }

    void stopListening()
    {
        if (running) {
            InterlockedExchange(&stopFlag, 1);
            SetEvent(evt);
            if (mgr) { WaitForSingleObject(mgr, 20000); CloseHandle(mgr); mgr = nullptr; }
            ReleaseSemaphore(sem, kWorkers, nullptr);
            for (HANDLE h : workers) { WaitForSingleObject(h, 20000); CloseHandle(h); }
            workers.clear();
            stopNotify();
            running = false;
        }
        { Guard g(&tcs); for (Target& t : targets) if (t.sock != (size_t)INVALID_SOCKET) closesocket((SOCKET)t.sock); targets.clear(); notes.clear(); }
        { Guard g(&qcs); for (QConn& q : queue) closesocket((SOCKET)q.sock); queue.clear(); }
        if (sem) { CloseHandle(sem); sem = nullptr; }
        if (evt) { CloseHandle(evt); evt = nullptr; }
        delete tls; tls = nullptr;
    }

    bool apply(Str& err) { stopListening(); return startListening(err); }

    // ---- threads ----
    static DWORD WINAPI ManagerThread(LPVOID p) { ((Impl*)p)->managerLoop(); return 0; }
    static DWORD WINAPI WorkerThread(LPVOID p) { ((Impl*)p)->workerLoop(); return 0; }

    void managerLoop()
    {
        while (!stopFlag) {
            fd_set fs; FD_ZERO(&fs); int n = 0;
            { Guard g(&tcs); for (Target& t : targets) if (t.sock != (size_t)INVALID_SOCKET && n < 60) { FD_SET((SOCKET)t.sock, &fs); ++n; } }
            if (n == 0) WaitForSingleObject(evt, 500);
            else {
                timeval tv; tv.tv_sec = 0; tv.tv_usec = 250000;
                int r = select(0, &fs, nullptr, nullptr, &tv);
                if (r > 0) {
                    Vec<SOCKET> ready;
                    { Guard g(&tcs); for (Target& t : targets) if (t.sock != (size_t)INVALID_SOCKET && FD_ISSET((SOCKET)t.sock, &fs)) ready.push_back((SOCKET)t.sock); }
                    for (SOCKET ls : ready) acceptOne(ls);
                }
            }
            if (stopFlag) break;
            bool sig = WaitForSingleObject(evt, 0) == WAIT_OBJECT_0;
            if (sig) Sleep(300);                                  // a burst of change notifications: let the configuration settle
            if (sig || (i64)GetTickCount64() / 1000 - lastReconcile >= retrySec) reconcile();
        }
    }

    void acceptOne(SOCKET ls)
    {
        sockaddr_storage ss; int sl = sizeof(ss);
        SOCKET s = accept(ls, (sockaddr*)&ss, &sl);
        if (s == INVALID_SOCKET) return;
        char buf[80] = "";
        if (ss.ss_family == AF_INET) inet_ntop(AF_INET, &((sockaddr_in*)&ss)->sin_addr, buf, sizeof(buf));
        else if (ss.ss_family == AF_INET6) inet_ntop(AF_INET6, &((sockaddr_in6*)&ss)->sin6_addr, buf, sizeof(buf));
        Str ip = buf;
        { Guard g(&ipcs); int* c = nullptr; for (IpCount& x : ipCounts) if (x.ip == ip) c = &x.n;
          if (c && *c >= kMaxPerIp) { if (getenv("VM2B_WEB_DEBUG")) fprintf(stderr, "web: per-IP cap for %s\n", ip.c_str()); closesocket(s); return; }
          if (c) ++*c; else { IpCount x; x.ip = ip; x.n = 1; ipCounts.push_back(x); } }
        { Guard g(&qcs); if ((int)queue.size() >= kMaxQueue) {      // full: drop the OLDEST waiting connection (silent ones pile up first), not the newcomer
              if (getenv("VM2B_WEB_DEBUG")) fprintf(stderr, "web: queue full\n");
              QConn old = queue[0]; queue.erase(0); closesocket((SOCKET)old.sock); releaseIp(old.ip); }
          QConn q; q.tick = GetTickCount(); q.sock = (size_t)s; q.ip = ip; queue.push_back(q); }
        ReleaseSemaphore(sem, 1, nullptr);
    }
    void releaseIp(const Str& ip)
    {
        Guard g(&ipcs);
        for (size_t i = 0; i < ipCounts.size(); ++i) if (ipCounts[i].ip == ip) { if (--ipCounts[i].n <= 0) ipCounts.erase(i); return; }
    }

    void workerLoop()
    {
        while (!stopFlag) {
            if (WaitForSingleObject(sem, 500) != WAIT_OBJECT_0) continue;
            QConn q; bool got = false;
            { Guard g(&qcs); if (queue.size()) { q = queue[0]; queue.erase(0); got = true; } }
            if (!got) continue;
            // a connection that has sent nothing yet must not hold a worker (24 silent sockets would stall the whole site for the
            // handshake timeout): put it back at the end of the queue until its first bytes arrive or the handshake time is over
            { fd_set rs; FD_ZERO(&rs); FD_SET((SOCKET)q.sock, &rs); timeval z; z.tv_sec = 0; z.tv_usec = 0;
              if (select(0, &rs, nullptr, nullptr, &z) <= 0) {
                  if (GetTickCount() - q.tick < (DWORD)kHandshakeMs && !stopFlag) { { Guard g(&qcs); queue.push_back(q); } ReleaseSemaphore(sem, 1, nullptr); Sleep(5); continue; }
                  closesocket((SOCKET)q.sock); releaseIp(q.ip); continue;
              } }
            serve(q);
            closesocket((SOCKET)q.sock);
            releaseIp(q.ip);
        }
    }

    size_t queued() { Guard g(&qcs); return queue.size(); }

    // ---- HTTP ----
    struct Header { Str name, value; };
    struct Request {
        Str method, path, query, version; Vec<Header> headers; Bytes body; bool keepAlive;
        Str header(const char* n) const { for (const Header& h : headers) if (h.name == n) return h.value; return Str(); }
        bool has(const char* n) const { for (const Header& h : headers) if (h.name == n) return true; return false; }
    };
    struct Response {
        int status; Str type; Str body; Vec<Str> extra; bool close, applyAfter;
        Response() : status(200), close(false), applyAfter(false) {}
    };
    struct Conn {
        WebTlsConn tls; Bytes buf; size_t len; Str ip;
        Conn() : len(0) {}
    };

    // reads more bytes: >0 ok, 0 closed, -1 error, -2 deadline / stop
    i64 fill(Conn& c, DWORD deadlineTick, bool idleWait = false)
    {
        if (c.buf.size() < c.len + 4096) c.buf.resize(c.len + 8192);
        for (;;) {
            if (stopFlag) return -2;
            if (idleWait && queued() > 0) return -2;                  // an idle keep-alive connection yields its worker to a waiting one
            int left = (int)(deadlineTick - GetTickCount());
            if (left <= 0) return -2;
            i64 k = c.tls.recv(c.buf.data() + c.len, c.buf.size() - c.len, left < 250 ? left : 250);
            if (k == kTlsTimeout) continue;
            if (k > 0) c.len += (size_t)k;
            return k;
        }
    }

    static const char* Reason(int s)
    {
        switch (s) { case 100: return "Continue"; case 200: return "OK"; case 202: return "Accepted"; case 204: return "No Content"; case 400: return "Bad Request"; case 401: return "Unauthorized"; case 403: return "Forbidden";
                     case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 408: return "Request Timeout"; case 411: return "Length Required"; case 413: return "Payload Too Large";
                     case 414: return "URI Too Long"; case 415: return "Unsupported Media Type"; case 417: return "Expectation Failed"; case 429: return "Too Many Requests"; case 431: return "Request Header Fields Too Large";
                     case 500: return "Internal Server Error"; case 501: return "Not Implemented"; case 505: return "HTTP Version Not Supported"; default: return "Status"; }
    }

    bool sendResponse(Conn& c, Response& r, bool head)
    {
        Str h = Str::fmt("HTTP/1.1 %d %s\r\n", r.status, Reason(r.status));
        h += "Content-Type: " + (r.type.empty() ? Str("text/plain; charset=utf-8") : r.type) + "\r\n";
        h += Str::fmt("Content-Length: %zu\r\n", r.body.size());
        h += "Content-Security-Policy: default-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n";
        h += "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
        h += "Strict-Transport-Security: max-age=31536000\r\nCross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Resource-Policy: same-origin\r\n";
        h += "Permissions-Policy: camera=(), microphone=(), geolocation=()\r\n";
        for (const Str& x : r.extra) h += x + "\r\n";
        h += r.close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\nKeep-Alive: timeout=10\r\n\r\n";
        if (!head) h.append(r.body.data(), r.body.size());
        return c.tls.send(h.data(), h.size(), 15000);
    }

    static Response Plain(int status, const char* text) { Response r; r.status = status; r.body = text; r.type = "text/plain; charset=utf-8"; r.extra.push_back("Cache-Control: no-store"); return r; }
    static Response JsonResp(int status, const Json& j) { Response r; r.status = status; r.body = j.dump(); r.type = "application/json; charset=utf-8"; r.extra.push_back("Cache-Control: no-store"); return r; }
    static Response Err(int status, const Str& msg) { Json j = Json::object(); j["ok"] = false; j["error"] = msg; return JsonResp(status, j); }

    // parses the head in c.buf[0..headEnd); returns 0 or the HTTP status to answer with
    int parseHead(Conn& c, size_t headEnd, Request& rq)
    {
        const char* p = (const char*)c.buf.data(); size_t n = headEnd;     // up to and excluding the final CRLFCRLF
        for (size_t i = 0; i < n; ++i) {
            unsigned char ch = (unsigned char)p[i];
            if (ch == 0) return 400;
            if (ch == '\r') { if (!(i + 1 < n && p[i + 1] == '\n')) return 400; }
            else if (ch == '\n') { if (!(i > 0 && p[i - 1] == '\r')) return 400; }
            else if (ch < 0x20 && ch != '\t') return 400;
        }
        size_t pos = 0; int lineNo = 0;
        while (pos <= n) {
            size_t e = pos; while (e < n && !(p[e] == '\r' && e + 1 < n && p[e + 1] == '\n')) ++e;
            Str line(p + pos, e - pos);
            if (lineNo == 0) {
                int s1 = line.find(' '); if (s1 <= 0) return 400;
                int s2 = line.find(' ', (size_t)s1 + 1); if (s2 < 0) return 400;
                rq.method = line.substr(0, (size_t)s1); Str target = line.substr((size_t)s1 + 1, (size_t)(s2 - s1 - 1)); rq.version = line.substr((size_t)s2 + 1);
                if (rq.version.find(' ') >= 0) return 400;
                if (rq.version != "HTTP/1.1" && rq.version != "HTTP/1.0") return rq.version.starts_with("HTTP/") ? 505 : 400;
                for (size_t i = 0; i < rq.method.size(); ++i) if (!((rq.method[i] >= 'A' && rq.method[i] <= 'Z'))) return 400;
                if (target.empty() || target.size() > 2048) return target.empty() ? 400 : 414;
                if (target[0] != '/') return 400;
                for (size_t i = 0; i < target.size(); ++i) { unsigned char ch = (unsigned char)target[i]; if (ch <= 0x20 || ch >= 0x7f || ch == '\\') return 400; }
                int q = target.find('?');
                rq.path = q >= 0 ? target.substr(0, (size_t)q) : target; rq.query = q >= 0 ? target.substr((size_t)q + 1) : Str();
            } else {
                if (line.empty()) return 400;
                if (line[0] == ' ' || line[0] == '\t') return 400;                 // obsolete line folding: a smuggling vector
                int col = line.find(':'); if (col <= 0) return 400;
                Header h; h.name = line.substr(0, (size_t)col).lower(); h.value = line.substr((size_t)col + 1).trim();
                for (size_t i = 0; i < h.name.size(); ++i) { char ch = h.name[i]; bool tok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' || ch == '*' || ch == '+' || ch == '^' || ch == '`' || ch == '|' || ch == '~'; if (!tok) return 400; }
                if ((int)rq.headers.size() >= kMaxHeaders) return 431;
                rq.headers.push_back(h);
            }
            ++lineNo;
            if (e >= n) break;
            pos = e + 2;
        }
        if (lineNo == 0) return 400;
        return 0;
    }

    static bool ParseLen(const Str& v, size_t& out)
    {
        if (v.empty() || v.size() > 10) return false;
        size_t n = 0; for (size_t i = 0; i < v.size(); ++i) { if (v[i] < '0' || v[i] > '9') return false; n = n * 10 + (size_t)(v[i] - '0'); }
        out = n; return true;
    }

    void serve(QConn& q)
    {
        Conn c; c.ip = q.ip;
        { int one = 1; setsockopt((SOCKET)q.sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)); }
        Str e;
        if (!tls || !c.tls.accept(q.sock, *tls, kHandshakeMs, &stopFlag, e)) { if (getenv("VM2B_WEB_DEBUG")) fprintf(stderr, "web: handshake from %s failed: %s\n", q.ip.c_str(), e.c_str()); return; }
        for (int nreq = 0; nreq < kMaxRequestsPerConn && !stopFlag; ++nreq) {
            DWORD idleEnd = GetTickCount() + (nreq == 0 ? kHeadMs : kIdleKeepAliveMs);
            size_t headEnd = 0; bool have = false; DWORD headDeadline = 0; bool started = c.len > 0;
            if (started) headDeadline = GetTickCount() + kHeadMs;
            for (;;) {
                for (size_t i = 0; i + 3 < c.len; ++i) if (c.buf[i] == '\r' && c.buf[i + 1] == '\n' && c.buf[i + 2] == '\r' && c.buf[i + 3] == '\n') { headEnd = i; have = true; break; }
                if (have && headEnd + 4 > kMaxHead) { Response r = Plain(431, "header too large"); r.close = true; sendResponse(c, r, false); goto done; }
                if (have) break;
                if (c.len > kMaxHead) { Response r = Plain(431, "header too large"); r.close = true; sendResponse(c, r, false); goto done; }
                i64 k = fill(c, started ? headDeadline : idleEnd, !started && nreq > 0);    // only an IDLE keep-alive yields its worker: a fresh connection gets to send its first request
                if (k == 0 || k == -1) goto done;
                if (k == -2) { if (started) { Response r = Plain(408, "request timeout"); r.close = true; sendResponse(c, r, false); } goto done; }
                if (!started) { started = true; headDeadline = GetTickCount() + kHeadMs; }
            }
            {
                Request rq;
                int bad = parseHead(c, headEnd, rq);
                if (bad) { Response r = Plain(bad, Reason(bad)); r.close = true; sendResponse(c, r, false); goto done; }
                size_t consumed = headEnd + 4;
                if (rq.has("transfer-encoding")) { Response r = Plain(501, "chunked / transfer-coded bodies are not supported"); r.close = true; sendResponse(c, r, false); goto done; }
                size_t clen = 0; int clCount = 0; for (const Header& h : rq.headers) if (h.name == "content-length") ++clCount;
                if (clCount > 1) { Response r = Plain(400, "duplicate Content-Length"); r.close = true; sendResponse(c, r, false); goto done; }
                if (clCount == 1 && !ParseLen(rq.header("content-length"), clen)) { Response r = Plain(400, "bad Content-Length"); r.close = true; sendResponse(c, r, false); goto done; }
                if (rq.version == "HTTP/1.1" && (!rq.has("host") || rq.header("host").empty())) { Response r = Plain(400, "Host header required"); r.close = true; sendResponse(c, r, false); goto done; }
                { int hc = 0; for (const Header& h : rq.headers) if (h.name == "host") ++hc; if (hc > 1) { Response r = Plain(400, "duplicate Host"); r.close = true; sendResponse(c, r, false); goto done; } }
                if (clen > kMaxBody) { Response r = Plain(413, "body too large (8 MiB at most)"); r.close = true; sendResponse(c, r, false); goto done; }
                Str conn = rq.header("connection").lower();
                rq.keepAlive = rq.version == "HTTP/1.1" ? conn.find("close") < 0 : conn.find("keep-alive") >= 0;
                if (rq.header("expect").lower() == "100-continue" && clen > 0 && c.len - consumed < clen) { const char* cont = "HTTP/1.1 100 Continue\r\n\r\n"; c.tls.send(cont, strlen(cont), 5000); }
                else if (rq.has("expect") && rq.header("expect").lower() != "100-continue") { Response r = Plain(417, "unsupported Expect"); r.close = true; sendResponse(c, r, false); goto done; }
                DWORD bodyDeadline = GetTickCount() + kBodyMs;
                while (c.len - consumed < clen) {
                    i64 k = fill(c, bodyDeadline);
                    if (k <= 0) { if (k == -2) { Response r = Plain(408, "request timeout"); r.close = true; sendResponse(c, r, false); } goto done; }
                }
                rq.body.resize(clen); if (clen) memcpy(rq.body.data(), c.buf.data() + consumed, clen);
                consumed += clen;
                memmove(c.buf.data(), c.buf.data() + consumed, c.len - consumed); c.len -= consumed;     // pipelined bytes stay

                Response r;
                route(c, rq, r);
                bool keep = rq.keepAlive && !r.close && queued() == 0 && nreq + 1 < kMaxRequestsPerConn;
                r.close = !keep;
                bool sent = sendResponse(c, r, rq.method == "HEAD");
                if (r.applyAfter) scheduleApply();              // a listener restart asked for by this very request: only after its answer left
                if (!sent) goto done;
                if (!keep) goto done;
            }
        }
    done:
        c.tls.shutdown();
    }

    // ---- routing ----
    Str clientIp(const Conn& c, const Request& rq)
    {
        if (trustProxy && (c.ip == "127.0.0.1" || c.ip == "::1")) {
            Str x = rq.header("x-forwarded-for");
            if (!x.empty()) { int cm = -1; for (size_t i = x.size(); i-- > 0;) if (x[i] == ',') { cm = (int)i; break; } Str last = (cm >= 0 ? x.substr((size_t)cm + 1) : x).trim(); in_addr a4; in6_addr a6; if (inet_pton(AF_INET, last.c_str(), &a4) == 1 || inet_pton(AF_INET6, last.c_str(), &a6) == 1) return last; }
        }
        return c.ip;
    }

    static Str CookieValue(const Request& rq, const char* name)
    {
        Str want = Str(name) + "=";
        for (const Header& h : rq.headers) {
            if (h.name != "cookie") continue;
            const Str& v = h.value; size_t pos = 0;
            while (pos < v.size()) {
                size_t e = pos; while (e < v.size() && v[e] != ';') ++e;
                Str part = v.substr(pos, e - pos).trim();
                if (part.starts_with(want.c_str())) return part.substr(want.size());
                pos = e + 1;
            }
        }
        return Str();
    }

    static Str SessionCookie(const Str& id, int maxAge)
    {
        if (id.empty()) return "Set-Cookie: __Host-vm2b=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0";
        return Str::fmt("Set-Cookie: __Host-vm2b=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d", id.c_str(), maxAge);
    }

    static bool SameOrigin(const Request& rq)
    {
        Str host = rq.header("host");
        Str origin = rq.header("origin");
        if (rq.has("origin") && origin != "https://" + host) return false;
        Str sfs = rq.header("sec-fetch-site");
        if (!sfs.empty() && sfs != "same-origin" && sfs != "none") return false;
        return true;
    }

    static bool CsrfEq(const Str& a, const Str& b)
    {
        if (a.size() != b.size() || a.empty()) return false;
        u8 d = 0; for (size_t i = 0; i < a.size(); ++i) d |= (u8)(a[i] ^ b[i]); return d == 0;
    }

    void serveStatic(const Request& rq, Response& r)
    {
        Str path = rq.path == "/" ? Str("/index.html") : rq.path;
        for (int i = 0; i < kWebUiCount; ++i) {
            if (path != kWebUi[i].path) continue;
            bool gz = rq.header("accept-encoding").lower().find("gzip") >= 0;
            r.status = 200; r.type = kWebUi[i].type;
            if (gz) { r.body = Str((const char*)kWebUi[i].gz, kWebUi[i].gzLen); r.extra.push_back("Content-Encoding: gzip"); r.extra.push_back("Vary: Accept-Encoding"); }
            else r.body = Str((const char*)kWebUi[i].raw, kWebUi[i].rawLen);
            r.extra.push_back("Cache-Control: no-cache");
            return;
        }
        r = Plain(404, "not found");
    }

    void route(Conn& c, Request& rq, Response& r)
    {
        const Str& m = rq.method;
        bool isApi = rq.path.starts_with("/api/");
        if (!isApi) {
            if (m != "GET" && m != "HEAD") { r = Plain(405, "method not allowed"); r.extra.push_back("Allow: GET, HEAD"); return; }
            serveStatic(rq, r); return;
        }
        Str ip = clientIp(c, rq);
        Str name = rq.path.substr(5);
        if (name.empty() || name.size() > 40) { r = Err(404, "not found"); return; }
        for (size_t i = 0; i < name.size(); ++i) { char ch = name[i]; if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_')) { r = Err(404, "not found"); return; } }
        if (name == "me") {
            if (m != "GET" && m != "HEAD") { r = Err(405, "method not allowed"); return; }
            Json j = Json::object(); WebSession s;
            if (auth.session(CookieValue(rq, "__Host-vm2b"), s)) { j["authenticated"] = true; j["user"] = s.user; j["role"] = s.role; j["csrf"] = s.csrf; j["session_seconds"] = auth.sessionSeconds(); }
            else { j["authenticated"] = false; j["no_users"] = auth.userCount() == 0; }
            j["version"] = Str(VM2B_VER_STR);
            r = JsonResp(200, j); return;
        }
        if (m != "POST") { r = Err(405, "use POST"); r.extra.push_back("Allow: POST"); return; }
        if (!SameOrigin(rq)) { r = Err(403, "cross-origin request refused"); auth.audit("csrf_origin_refused", "", ip, name); return; }
        Str ct = rq.header("content-type").lower();
        if (!ct.starts_with("application/json")) { r = Err(415, "Content-Type must be application/json"); return; }
        Json args = Json::object();
        if (rq.body.size()) {
            // invalid UTF-8 would end up in config.json and in answers; a body with a flood of members would make the (quadratic) object parser burn CPU before any login check
            if (!Utf8Valid((const char*)rq.body.data(), rq.body.size())) { r = Err(400, "the body must be valid UTF-8 text"); return; }
            size_t commas = 0; for (size_t i = 0; i < rq.body.size(); ++i) commas += rq.body[i] == ',';
            if (commas > 10000) { r = Err(413, "the request has too many elements"); return; }
        }
        if (rq.body.size()) { Str e; Json parsed; if (!Json::parse((const char*)rq.body.data(), rq.body.size(), parsed, e) || !parsed.is_object()) { r = Err(400, "the body must be a JSON object"); return; } args = parsed; }
        if (name == "login") {
            Str code = args.get("code").str(), rec = args.get("recovery").str();
            WebAuth::LoginResult lr = auth.login(ip, args.get("user").str(), args.get("password").str(), code, rec);
            Json j = Json::object();
            if (lr.status == 200) {
                j["ok"] = true; j["user"] = lr.user; j["role"] = lr.role; j["csrf"] = lr.csrf; j["session_seconds"] = auth.sessionSeconds();
                r = JsonResp(200, j); r.extra.push_back(SessionCookie(lr.sessionId, 12 * 3600)); return;
            }
            j["ok"] = false; j["error"] = lr.message; if (lr.status == 202) j["need_2fa"] = true; if (lr.status == 429) j["retry_after"] = lr.retryAfter;
            if (lr.message.starts_with("No users")) j["no_users"] = true;
            r = JsonResp(lr.status, j); if (lr.status == 429) r.extra.push_back(Str::fmt("Retry-After: %d", lr.retryAfter));
            return;
        }
        WebSession s;
        if (!auth.session(CookieValue(rq, "__Host-vm2b"), s)) { r = Err(401, "not signed in"); return; }
        if (!CsrfEq(rq.header("x-csrf-token"), s.csrf)) { r = Err(403, "missing or wrong CSRF token"); auth.audit("csrf_refused", s.user, ip, name); return; }
        if (name == "logout") { auth.logout(s.id); Json j = Json::object(); j["ok"] = true; r = JsonResp(200, j); r.extra.push_back(SessionCookie(Str(), 0)); return; }
        if (name == "password") {
            Str e; Json j = Json::object();
            if (!auth.changePassword(s.user, args.get("current").str(), args.get("next").str(), ip, e)) { j["ok"] = false; j["error"] = e; r = JsonResp(400, j); return; }
            j["ok"] = true; r = JsonResp(200, j); r.extra.push_back(SessionCookie(Str(), 0)); return;
        }
        if (!WebAuth::RoleAllows(s.role, name)) {
            auth.audit("forbidden", s.user, ip, name);
            r = Err(403, name == "service.shutdown" ? "this command is not available over the web" : "your role does not allow this"); return;
        }
        Json result; Str err; bool ok, later = false;
        if (name.starts_with("web.")) ok = owner->handleAdminFromWeb(s.user, s.id, ip, name, args, result, err, later);      // the site's own administration (admin role): with lock-out protections
        else ok = core->handle(name, args, result, err);
        if (!WebAuth::IsReadOnlyCmd(name)) {
            Str d = name; Str id = args.get("id").str(); if (id.empty()) id = args.get("task").get("name").str(); if (id.empty()) id = args.get("server").get("name").str(); if (id.empty()) id = args.get("name").str();
            if (!id.empty()) d += " " + id;
            auth.audit(ok ? "api" : "api_failed", s.user, ip, d);
        }
        Json j = Json::object(); j["ok"] = ok;
        if (ok) j["result"] = result; else j["error"] = err;
        r = JsonResp(200, j); r.applyAfter = later;
    }

    // ---- admin ----
    Json certJson()
    {
        Json cert = Json::object();
        WebTlsIdentity id; bool ss = false;
        if (auth.identity(id, &ss)) {
            cert["fingerprint"] = WebTlsFingerprint(id.certDer); cert["self_signed"] = ss;
            Json d = WebTlsDescribe(id.certDer); for (size_t i = 0; i < d.size(); ++i) cert[d.key_at(i).c_str()] = d.value_at(i);
        }
        return cert;
    }

    Json statusJson()
    {
        Json st = Json::object(); Json s = auth.settings();
        st["enabled"] = s.get("enabled").boolean(); st["port"] = s.get("port"); st["bind"] = s.get("bind");
        bool listening = false; Str first;
        Json ents = Json::array();
        { Guard g(&tcs);
          for (const Str& e : entries) {
              Json eo = Json::object(); eo["entry"] = e; Json addrs = Json::array(); bool any = false; Str firstErr;
              for (const Target& t : targets) if (t.entry == e) {
                  Json a = Json::object(); a["address"] = t.addr; bool l = t.sock != (size_t)INVALID_SOCKET; a["listening"] = l; if (!l) { a["error"] = t.err; if (firstErr.empty()) firstErr = t.err; }
                  if (l) { any = true; if (first.empty()) first = t.addr; } addrs.push(a);
              }
              eo["addresses"] = addrs; eo["listening"] = any;
              Str note; for (const EntryNote& n : notes) if (n.entry == e) note = n.note;
              if (!any) eo["error"] = !note.empty() ? note : (!firstErr.empty() ? firstErr : Str(enabled && running ? "no address" : "not listening"));
              if (any) listening = true;
              ents.push(eo);
          } }
        st["entries"] = ents; st["listening"] = listening; st["address"] = first;
        if (!tlsError.empty()) st["error"] = tlsError;
        Json cert = certJson(); st["cert"] = cert; st["cert_fingerprint"] = cert.get("fingerprint");
        st["users"] = auth.userCount(); st["sessions"] = auth.sessionCount();
        return st;
    }

    Json warnings()
    {
        Json w = Json::array(); Json s = auth.settings();
        bool exposed = false;
        const Json& b = s.get("bind");
        for (size_t i = 0; i < b.size(); ++i) {
            Str e = b.at(i).str();
            if (entryIsLoopback(e)) continue;
            exposed = true;
        }
        if (!s.get("enabled").boolean() || !exposed) return w;
        WebTlsIdentity id; bool ss = true; auth.identity(id, &ss);
        if (ss) w.push("The web site is reachable from the network with the self-signed certificate. Browsers will warn; import a certificate from your own CA (web.cert.import) or put the site behind a reverse proxy with a trusted certificate.");
        Json us = auth.usersList(); int tf = 0; for (size_t i = 0; i < us.size(); ++i) if (us.at(i).get("totp_enabled").boolean()) ++tf;
        if (tf == 0) w.push("The web site is reachable from the network, but no user has two-factor authentication enabled. Enable it for every user (web.users.totp_enroll).");
        return w;
    }
};

// =================================================================================================================================
WebServer::WebServer() : m(new Impl()) { m->owner = this; }
WebServer::~WebServer() { stop(); for (int i = 0; i < 500 && m->applyPending > 0; ++i) Sleep(10); delete m; }

void WebServer::setClock(i64 (*clock)()) { m->clockFn = clock; m->auth.setClock(clock); }
void WebServer::setRetrySeconds(int s) { m->retrySec = s < 1 ? 1 : s; }
void WebServer::setInterfaceProvider(Json (*p)()) { m->ifProvider = p; if (m->evt) SetEvent(m->evt); }
void WebServer::setKdfIterationsForTests(u32 n) { m->auth.setKdfIterationsForTests(n); }
int WebServer::port() const { return m->portNum; }

bool WebServer::start(ServiceCore* core, const Str& dataDir, Str& err, bool enabledDefault)
{
    Guard g(&m->adminCs);
    InterlockedExchange(&m->closed, 0);
    m->core = core; m->dataDir = dataDir;
    if (!m->auth.open(dataDir, err, enabledDefault)) return false;
    m->auth.audit("service_start", "", "", "");
    Str e; m->startListening(e);                // a failure shows in web.status
    return true;
}

void WebServer::stop()
{
    InterlockedExchange(&m->closed, 1);                  // a listener restart that was asked for must not bring the site back
    Guard g(&m->adminCs); m->stopListening();
}

namespace {
Str B64DecodeToFile(const Str& b64, Bytes& out) { return Base64Decode(b64.data(), b64.size(), out) && !out.empty() ? Str() : Str("the file is not valid base64"); }
bool LoopbackEntry(const Str& e) { return e == "localhost" || WebAuth::IsLoopbackAddress(e); }
}

// The site administers itself (admin role, over HTTPS).  Same commands as the local channel, plus the protections that keep the administrator
// from locking everybody out; a listener restart is done after the answer was sent.
bool WebServer::handleAdminFromWeb(const Str& actor, const Str& session, const Str& ip, const Str& cmd, const Json& a, Json& r, Str& err, bool& applyLater)
{
    applyLater = false;
    for (;;) {                                           // never wait on the lock while the listener is being stopped: that thread joins this worker
        if (TryEnterCriticalSection(&m->adminCs)) break;
        if (m->stopFlag || m->closed) { err = "the web site is restarting; try again in a moment"; return false; }
        Sleep(15);
    }
    struct Rel { CRITICAL_SECTION* c; ~Rel() { LeaveCriticalSection(c); } } rel = { &m->adminCs };
    m->ctx = Impl::WebCtx(); m->ctx.viaWeb = true; m->ctx.actor = actor; m->ctx.ip = ip; m->ctx.session = session;
    bool ok = handleAdmin(cmd, a, r, err);
    applyLater = m->ctx.applyLater; m->ctx = Impl::WebCtx();
    return ok;
}

bool WebServer::handleAdmin(const Str& cmd, const Json& a, Json& r, Str& err)
{
    Guard g(&m->adminCs);
    r = Json::object();
    WebAuth& au = m->auth;
    const bool web = m->ctx.viaWeb;
    Str who = web ? m->ctx.actor : Str("local-admin"), from = web ? m->ctx.ip : Str("pipe");
    au.setActor(web ? m->ctx.actor : Str(), web ? m->ctx.ip : Str());
    struct Reset { WebAuth& a; ~Reset() { a.setActor(Str(), Str()); } } reset = { au };
    auto restart = [&](bool needed) -> Str { if (!needed) return Str(); if (web) { m->ctx.applyLater = true; return Str(); } Str e; m->apply(e); return e; };
    if (cmd == "web.status") { r = m->statusJson(); return true; }
    if (cmd == "web.settings.get") { r["settings"] = au.settings(); r["warnings"] = m->warnings(); return true; }
    if (cmd == "web.settings.set") {
        if (web) {
            if (a.has("enabled") && a.get("enabled").is_bool() && !a.get("enabled").boolean()) { err = "the site cannot be switched off from itself; stop the VM2Backup service or use the local administration channel"; return false; }
            Vec<Str> list; bool haveBind = false;
            if (a.has("bind")) {
                const Json& b = a.get("bind"); haveBind = true;
                if (b.is_string()) list.push_back(b.str().trim());
                else if (b.is_array()) for (size_t i = 0; i < b.size(); ++i) { if (!b.at(i).is_string()) { err = "bind entries must be text"; return false; } list.push_back(b.at(i).str().trim()); }
                else { err = "bind must be a list of addresses"; return false; }
                bool lo = false; for (const Str& e : list) if (LoopbackEntry(e)) lo = true;
                if (!lo) { err = "the list of addresses must keep a loopback entry (localhost or 127.0.0.1): otherwise nobody could reach the site again to repair a mistake"; return false; }
                for (const Str& e : list) if (!WebAuth::BindEntryOk(e)) { err = "bad bind entry \"" + e + "\": use an IP address, adapter:<GUID>, localhost or all"; return false; }
            }
            if (a.has("require_2fa_admins") && a.get("require_2fa_admins").is_bool() && a.get("require_2fa_admins").boolean() && !au.settings().get("require_2fa_admins").boolean()) {
                Str role; bool tf = false; if (!au.userInfo(m->ctx.actor, role, tf) || !tf) { err = "set up two-factor authentication for your own account first, otherwise requiring it would lock you out"; return false; }
            }
            if (haveBind || a.has("port")) {
                if (!haveBind) { const Json& cb = au.settings().get("bind"); for (size_t i = 0; i < cb.size(); ++i) list.push_back(cb.at(i).str()); }
                i64 port = a.has("port") ? a.get("port").integer(-1) : au.settings().get("port").integer(8443);
                if (port < 1 || port > 65535) { err = "port must be 1..65535"; return false; }
                if (!m->trialBind(list, (int)port, err)) return false;       // the old settings stay
            }
        }
        Json before = au.settings();
        if (!au.setSettings(a, err)) return false;
        Json after = au.settings();
        bool listenerChanged = !web || before.get("bind").dump() != after.get("bind").dump() || before.get("port").integer() != after.get("port").integer() || before.get("enabled").boolean() != after.get("enabled").boolean();
        if (!listenerChanged) InterlockedExchange(&m->trustProxy, after.get("trust_proxy").boolean() ? 1 : 0);     // the other settings are read live: no restart, no dropped connections
        Str e = restart(listenerChanged);
        r["settings"] = au.settings(); r["warnings"] = m->warnings();
        if (!e.empty()) r["error"] = e;
        au.audit("settings_changed", who, from, au.settings().dump());
        return true;
    }
    if (cmd == "web.interfaces.list") { r["interfaces"] = m->interfaces(); return true; }
    if (cmd == "web.cert.generate") {
        Vec<Str> names; const Json& n = a.get("names"); for (size_t i = 0; i < n.size(); ++i) names.push_back(n.at(i).str().trim());
        bool wasRunning = m->running;
        WebTlsIdentity id;
        if (!WebTlsGenerate(names, id, err)) return false;
        if (!au.setIdentity(id, true, err)) return false;
        au.audit("cert_generated", who, from, WebTlsFingerprint(id.certDer));
        m->certDer = id.certDer; m->selfSigned = true;
        Str e = restart(wasRunning); if (!e.empty()) r["error"] = e;
        r["fingerprint"] = WebTlsFingerprint(id.certDer); return true;
    }
    if (cmd == "web.cert.import") {
        WebTlsIdentity id; Str path = a.get("pfx_path").str(), tmp; size_t upSize = 0;
        if (a.has("pfx_base64")) {                       // an uploaded file (the web UI): written to a private temporary file of the data folder, wiped afterwards
            Bytes data; Str b64 = a.get("pfx_base64").str();
            if (b64.size() > (1u << 21)) { err = "the certificate file is too large (1 MB at most)"; return false; }
            Str de = B64DecodeToFile(b64, data); if (!de.empty()) { err = de; return false; }
            if (data.size() > (1u << 20)) { err = "the certificate file is too large (1 MB at most)"; return false; }
            tmp = JoinPath(m->dataDir, "web-upload.pfx.tmp");
            if (!WriteWholeFile(tmp.c_str(), data.data(), data.size(), err)) return false;
            upSize = data.size(); SecureZeroMemory(data.data(), data.size()); path = tmp;
        } else if (web) { err = "upload the certificate file (pfx_base64); the web site does not read files from the server by path"; return false; }
        bool good = WebTlsImportPfx(path, a.get("password").str(), id, err);
        if (!tmp.empty()) { Bytes z; z.resize(upSize); Str we; WriteWholeFile(tmp.c_str(), z.data(), z.size(), we); DeleteFileW(FilePath(tmp)); }
        if (!good) return false;
        if (!au.setIdentity(id, false, err)) return false;
        au.audit("cert_imported", who, from, WebTlsFingerprint(id.certDer));
        bool wasRunning = m->running; m->certDer = id.certDer; m->selfSigned = false;
        Str e = restart(wasRunning); if (!e.empty()) r["error"] = e;
        r["fingerprint"] = WebTlsFingerprint(id.certDer); return true;
    }
    if (cmd == "web.users.list") { r["users"] = au.usersList(); return true; }
    if (cmd == "web.users.save") {
        if (web) {
            Str name = a.get("name").str().trim(), role = a.get("role").str(), twofa = a.get("twofa").str(""), curRole; bool tf = false;
            bool exists = au.userInfo(name, curRole, tf), self = m->ctx.actor.lower() == name.lower();
            if (exists && !role.empty() && role != curRole && self) { err = "you cannot change your own role"; return false; }
            if (exists && !role.empty() && role != "admin" && curRole == "admin" && au.adminCount() <= 1) { err = "the last administrator cannot be demoted"; return false; }
            if (self && twofa == "required" && !tf) { err = "set up two-factor authentication for your own account first, otherwise you would lock yourself out"; return false; }
        }
        if (!au.usersSave(a, err)) return false;
        r["users"] = au.usersList(); return true;
    }
    if (cmd == "web.users.delete") {
        if (web) {
            Str name = a.get("name").str().trim(), curRole; bool tf = false;
            if (au.userInfo(name, curRole, tf)) {
                if (m->ctx.actor.lower() == name.lower()) { err = "you cannot delete your own account"; return false; }
                if (curRole == "admin" && au.adminCount() <= 1) { err = "the last administrator cannot be deleted"; return false; }
            }
        }
        return au.usersDelete(a.get("name").str(), err);
    }
    if (cmd == "web.users.totp_enroll") return au.totpEnroll(a.get("name").str(), r, err);
    if (cmd == "web.users.totp_confirm") {
        Str name = a.get("name").str(); bool self = web && m->ctx.actor.lower() == name.trim().lower();
        return au.totpConfirm(name, a.get("code").str(), r, err, self ? m->ctx.session : Str());
    }
    if (cmd == "web.users.totp_disable") {
        if (web && m->ctx.actor.lower() == a.get("name").str().trim().lower()) {
            Str role; bool tf = false; au.userInfo(m->ctx.actor, role, tf);
            if (role == "admin" && au.settings().get("require_2fa_admins").boolean(true)) { err = "you cannot switch off your own two-factor authentication while it is required for administrators"; return false; }
        }
        return au.totpDisable(a.get("name").str(), err);
    }
    if (cmd == "web.audit.tail") { r["text"] = au.auditTail((int)a.get("lines").integer(100)); return true; }
    err = "unknown command: " + cmd;
    return false;
}

} // namespace vm2b
