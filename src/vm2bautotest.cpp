// vm2bautotest.exe — the randomized end-to-end test of VM2Backup (console), same idea as c2sautotest.exe:
//   vm2bautotest.exe [--seed N] [--only a,b] [--skip a,b] [--keep] [--servers servers.json] [--list] [--minutes M]
// Every test draws its parameters from one seeded generator; the seed is printed first and the same seed repeats the same
// sequence. Failures are printed with the test name, the exit code is the number of failed tests (0 = all passed).
// Tests that need network servers (FTP / FTPS / SCP / ESXi emulators) read their addresses from --servers
// (a JSON file written by tools/run-tests.sh) and are reported as SKIP when it is not given.
#include "autotest.h"

namespace vm2b {
Vec<TestInfo>& Registry() { static Vec<TestInfo> r; return r; }
Reg::Reg(const char* name, const char* group, bool (*fn)(Ctx&)) { TestInfo t = { name, group, fn }; Registry().push_back(t); }
Vec<Teardown>& Teardowns() { static Vec<Teardown> r; return r; }
RegTeardown::RegTeardown(const char* group, void (*fn)(Ctx&)) { Teardown t = { group, fn }; Teardowns().push_back(t); }
}

using namespace vm2b;

static Str g_servers;

static bool Listed(const char* list, const char* name, const char* group)
{
    if (!list || !*list) return false;
    const char* p = list;
    while (*p) {
        const char* e = strchr(p, ','); size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((strlen(name) == n && !strncmp(p, name, n)) || (strlen(group) == n && !strncmp(p, group, n))) return true;
        if (!e) break; p = e + 1;
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    { wchar_t t[4]; if (!GetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", t, 4)) SetEnvironmentVariableW(L"VM2B_WEB_DEFAULT", L"0"); }   // the tests start many services: none of them may take port 8443 (the web site is on by default in the product)
    char buf[1024];
    u64 seed = 0; bool haveSeed = false, keep = false, list = false;
    Str only, skip;
    double minutes = 0;
    for (int i = 1; i < argc; ++i) {
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buf, sizeof(buf), nullptr, nullptr);
        Str a = buf;
        auto next = [&]() -> Str { if (i + 1 < argc) { WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, sizeof(buf), nullptr, nullptr); return Str(buf); } return Str(); };
        if (a == "--seed") { seed = strtoull(next().c_str(), nullptr, 10); haveSeed = true; }
        else if (a == "--only") only = next();
        else if (a == "--skip") skip = next();
        else if (a == "--keep") keep = true;
        else if (a == "--list") list = true;
        else if (a == "--servers") g_servers = next();
        else if (a == "--minutes") minutes = atof(next().c_str());
        else { printf("unknown argument %s\n", a.c_str()); return 2; }
    }
    if (list) { for (const TestInfo& t : Registry()) printf("%-10s %s\n", t.group, t.name); return 0; }
    if (!haveSeed) seed = (u64)GetTickCount64() * 2654435761ull ^ (u64)GetCurrentProcessId();
    printf("vm2bautotest seed %llu\n", (unsigned long long)seed);
    fflush(stdout);
    Ctx ctx(seed, keep);
    if (!g_servers.empty()) {
        Bytes b; Str e;
        if (!ReadWholeFile(g_servers.c_str(), b, e)) { printf("cannot read %s: %s\n", g_servers.c_str(), e.c_str()); return 2; }
        if (!Json::parse((const char*)b.data(), b.size(), ctx.servers, e)) { printf("bad servers file: %s\n", e.c_str()); return 2; }
    }
    int pass = 0, fail = 0, skipped = 0;
    double t0 = (double)GetTickCount64() / 1000.0;
    int round = 0;
    do {
        for (const TestInfo& t : Registry()) {
            if (!only.empty() && !Listed(only.c_str(), t.name, t.group)) continue;
            if (Listed(skip.c_str(), t.name, t.group)) continue;
            ctx.begin(t.name);
            double s = (double)GetTickCount64() / 1000.0;
            bool ok = t.fn(ctx);
            for (const Teardown& td : Teardowns()) if (!strcmp(td.group, t.group)) td.fn(ctx);        // the test VMs of the mock servers are deleted after every test
            double dt = (double)GetTickCount64() / 1000.0 - s;
            if (ctx.skipped) { printf("SKIP  %-9s %-34s %s\n", t.group, t.name, ctx.skipReason.c_str()); ++skipped; }
            else if (ok && !ctx.failed) { printf("PASS  %-9s %-34s %.1fs\n", t.group, t.name, dt); ++pass; }
            else { printf("FAIL  %-9s %-34s %.1fs\n", t.group, t.name, dt); ++fail; }
            fflush(stdout);
        }
        ++round;
    } while (minutes > 0 && (double)GetTickCount64() / 1000.0 - t0 < minutes * 60);
    printf("%d passed, %d failed, %d skipped (%d round%s, seed %llu)\n", pass, fail, skipped, round, round == 1 ? "" : "s", (unsigned long long)seed);
    return fail;
}
