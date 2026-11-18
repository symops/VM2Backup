// Ревью безопасности F9: клиент канала проверяет, какая программа на другом конце (GetNamedPipeServerProcessId + файл программы), и не посылает ей ничего.
// Под Wine проверяется клиентская часть и то, что сервер работает с PIPE_REJECT_REMOTE_CLIENTS; занятое имя канала (FILE_FLAG_FIRST_PIPE_INSTANCE) Wine не
// принуждает, это проверяет только гостевая фаза на Windows (.winvm/guest/pipe.ps1, docs/WINDOWS-TESTS.md).
#include "autotest.h"
#include "ipc.h"
#include "winutil.h"
#include <sddl.h>

namespace vm2b {

namespace {

struct Counting : IpcHandler {
    volatile LONG calls; Counting() : calls(0) {}
    bool handle(const Str& cmd, const Json& args, Json& result, Str& err) override {
        InterlockedIncrement(&calls);
        if (cmd == "echo") { result = args; return true; }
        err = "unknown command"; return false;
    }
};

Str MyExe() { wchar_t me[MAX_PATH * 2]; GetModuleFileNameW(nullptr, me, MAX_PATH * 2); return FromWide(me); }

} // namespace

VM2B_TEST("service", pipe_client_checks_the_program_at_the_other_end)
{
    Str pipe = Str::fmt("VM2B-test-pipe-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(c_.next() % 1000000));
    Counting h; IpcServer srv; Str err;
    T_CHECK(srv.start(pipe, &h, err, "D:(A;;GA;;;WD)")); if (c_.failed) return false;
    // the right program (this very process is the "service"): works
    { IpcClient cli; cli.expectServerImage(MyExe()); T_CHECK(cli.connect(pipe, 3000, err)); Json a = Json::object(); a["x"] = 5; Json r; T_CHECK(cli.call("echo", a, r, err)); T_CHECK(r.get("x").integer() == 5); }
    // a wrong program (the same pipe, another expected file): refused before any request is sent
    LONG before = h.calls;
    { IpcClient cli; cli.expectServerImage("C:\\Windows\\System32\\notepad.exe"); Str e2; bool ok = cli.connect(pipe, 3000, e2);
      if (e2.find("cannot tell") >= 0 || e2.find("cannot look") >= 0 || e2.find("cannot read") >= 0) { c_.skip("this system cannot tell the process behind a pipe"); srv.stop(); return true; }
      T_CHECK(!ok); T_CHECK(!cli.connected()); T_CHECK(e2.find("not the VM2Backup service") >= 0); c_.note("refused: %s", e2.c_str());
      // the refusal is a failure kind of its own ("wrong server"), not "no service": the panel shows "Blocked" with the program, not "Stopped"
      T_CHECK(cli.failure() == IPC_WRONG_SERVER); T_CHECK(e2.starts_with(kWrongServerPrefix)); T_CHECK(cli.serverPid() == GetCurrentProcessId()); T_CHECK(cli.serverImage().lower() == MyExe().lower());
      IpcClient none; Str e4; T_CHECK(!none.connect("vm2b-nobody-serves-this-pipe", 150, e4)); T_CHECK(none.failure() == IPC_NOT_RUNNING); T_CHECK(!e4.starts_with(kWrongServerPrefix)); }
    T_CHECK(h.calls == before);                                                          // nothing reached the server
    // no expectation on a custom pipe name: connects as before (the tests and /pipe rely on it)
    { IpcClient cli; T_CHECK(cli.connect(pipe, 3000, err)); Json r; T_CHECK(cli.call("echo", Json::object(), r, err)); }
    // the path is compared as a full path, case does not matter
    { IpcClient cli; Str up = MyExe(); Str odd = up.lower(); cli.expectServerImage(odd); T_CHECK(cli.connect(pipe, 3000, err)); }
    // the registered image of the service is a file name, never empty
    T_CHECK(IpcClient::ServiceImagePath().size() > 4);
    srv.stop();
    return true;
}

// Real Windows only (Wine does not enforce FILE_FLAG_FIRST_PIPE_INSTANCE): a program that holds the pipe name must neither stop the service for good nor be taken for it.
VM2B_TEST("winreal", pipe_name_held_by_another_program_is_waited_out)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"); if (nt && GetProcAddress(nt, "wine_get_version")) { c_.skip("Wine does not enforce FILE_FLAG_FIRST_PIPE_INSTANCE"); return true; }
    Str pipe = Str::fmt("VM2B-test-squat-%lu-%llu", (unsigned long)GetCurrentProcessId(), (unsigned long long)(c_.next() % 1000000));
    PSECURITY_DESCRIPTOR sd = nullptr; T_CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr));
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    HANDLE squat = CreateNamedPipeW(Wide("\\\\.\\pipe\\" + pipe), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa);
    T_CHECK(squat != INVALID_HANDLE_VALUE); if (squat == INVALID_HANDLE_VALUE) { LocalFree(sd); return false; }
    Counting h; Str err;
    { IpcServer s1; bool ok = s1.start(pipe, &h, err, "D:(A;;GA;;;WD)"); T_CHECK(!ok); T_CHECK(err.find("already running") >= 0); c_.note("a held name, no waiting: %s", err.c_str()); }
    IpcServer s2; T_CHECK(s2.start(pipe, &h, err, "D:(A;;GA;;;WD)", true)); T_CHECK(s2.waiting());
    { IpcClient cli; cli.expectServerImage("C:\\Windows\\System32\\notepad.exe"); Str e2; T_CHECK(!cli.connect(pipe, 2000, e2)); T_CHECK(e2.find("not the VM2Backup service") >= 0 || e2.find("cannot") >= 0); T_CHECK(e2.find("not the VM2Backup service") < 0 || cli.failure() == IPC_WRONG_SERVER); }      // this squatter does not listen (PIPE_BUSY: the server is never reached); IPC_WRONG_SERVER for a listening one is checked by pipe_default_name_client_checks_the_service
    T_CHECK(h.calls == 0);
    CloseHandle(squat);                                                                  // the other program lets go
    bool took = false; for (int i = 0; i < 40 && !took; ++i) { if (!s2.waiting()) took = true; else Sleep(250); }
    T_CHECK(took);
    { IpcClient cli; Str e3; T_CHECK(cli.connect(pipe, 5000, e3)); Json r; T_CHECK(cli.call("echo", Json::object(), r, e3)); }
    s2.stop(); LocalFree(sd);
    return true;
}

// Real Windows, run by .winvm/guest/lowsec.ps1 (VM2B_SQUAT=1 while a program holds the default pipe name; without it: against the real service, when it runs).
VM2B_TEST("winreal", pipe_default_name_client_checks_the_service)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"); if (nt && GetProcAddress(nt, "wine_get_version")) { c_.skip("Wine: no service of its own"); return true; }
    const char* sq = getenv("VM2B_SQUAT"); bool squat = sq && *sq == '1';
    IpcClient cli; Str err; bool ok = cli.connect(kDefaultPipeName, 4000, err);
    c_.note("default pipe, squatter %s: %s %s", squat ? "present" : "absent", ok ? "connected" : "refused", ok ? "" : err.c_str());
    if (squat) { T_CHECK(!ok); T_CHECK(err.find("not the VM2Backup service") >= 0); T_CHECK(cli.failure() == IPC_WRONG_SERVER); T_CHECK(err.starts_with(kWrongServerPrefix)); return true; }      // (the panel shows this as "Blocked")
    if (!ok) { c_.skip(("no service on the default pipe: " + err).c_str()); return true; }
    Json st; T_CHECK(cli.call("state", Json::object(), st, err)); T_CHECK(st.get("version").str().size() > 3);          // the real service is accepted
    return true;
}

} // namespace vm2b
