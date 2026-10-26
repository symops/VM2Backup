// Property test of schedule.cpp against a naive minute-by-minute oracle, plus hostile values (ASan/UBSan must stay quiet, no hang).
//   t_sched_prop <seed> [cases]
#include <time.h>
#include "../../src/schedule.h"
using namespace vm2b;
static u64 rs;
static u32 rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)(rs >> 11); }
static int fails = 0;
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#define CHECK(c) do { if (!(c)) { if (fails++ < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static i64 FloorDiv(i64 a, i64 b) { i64 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static void Civil(i64 day, int& y, int& m, int& d)    // independent implementation: via gmtime
{
    time_t t = (time_t)(day * 86400); struct tm g; gmtime_r(&t, &g); y = g.tm_year + 1900; m = g.tm_mon + 1; d = g.tm_mday;
}
static int Dim(int y, int m) { int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : dm[m - 1]; }
static int Wd(i64 day) { time_t t = (time_t)(day * 86400); struct tm g; gmtime_r(&t, &g); return (g.tm_wday + 6) % 7; }

static bool Fires(const Json& sp, i64 m)
{
    Str mode = sp.get("mode").str(); i64 day = FloorDiv(m, 1440); int mod = (int)(m - day * 1440);
    if (mode == "every") {
        i64 first; ParseLocalTime(sp.get("first_time").str().c_str(), first);
        i64 step = sp.get("every").get("value").integer() * (sp.get("every").get("unit").str() == "minutes" ? 1 : sp.get("every").get("unit").str() == "hours" ? 60 : 1440);
        return m >= first && (m - first) % step == 0;
    }
    int h, mi; sscanf(sp.get("start_time").str().c_str(), "%d:%d", &h, &mi);
    if (mod != h * 60 + mi) return false;
    if (mode == "weekly") {
        i64 interval = sp.has("week_interval") ? sp.get("week_interval").integer() : 1;
        i64 anchorDay; if (sp.has("anchor")) { i64 am; ParseLocalTime(sp.get("anchor").str().c_str(), am); anchorDay = FloorDiv(am, 1440); } else { i64 am; ParseLocalTime("2000-01-03", am); anchorDay = am / 1440; }
        anchorDay -= Wd(anchorDay);
        bool listed = false; for (size_t i = 0; i < sp.get("weekdays").size(); ++i) if (sp.get("weekdays").at(i).integer() == Wd(day)) listed = true;
        i64 wk = FloorDiv(day - anchorDay, 7);
        return listed && ((wk % interval) + interval) % interval == 0;
    }
    int y, mo, d; Civil(day, y, mo, d);
    i64 interval = sp.has("month_interval") ? sp.get("month_interval").integer() : 1;
    i64 idx = (i64)y * 12 + mo - 1 - 2000 * 12;
    if (((idx % interval) + interval) % interval != 0) return false;
    bool last = sp.get("day").is_string(); int want = last ? Dim(y, mo) : (int)sp.get("day").integer(); if (want > Dim(y, mo)) want = Dim(y, mo);
    return d == want;
}

static Json RandomSchedule()
{
    Json sp = Json::object(); sp["enabled"] = true;
    int k = (int)(rnd() % 3);
    char buf[64];
    if (k == 0) {
        sp["mode"] = "every"; Json ev = Json::object(); const char* u[] = { "minutes", "hours", "days" }; int ui = (int)(rnd() % 3);
        ev["value"] = (i64)(1 + rnd() % (ui == 0 ? 500 : ui == 1 ? 50 : 40)); ev["unit"] = u[ui]; sp["every"] = ev;
        snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d", 2024 + (int)(rnd() % 5), 1 + (int)(rnd() % 12), 1 + (int)(rnd() % 28), (int)(rnd() % 24), (int)(rnd() % 60)); sp["first_time"] = buf;
    } else if (k == 1) {
        sp["mode"] = "weekly"; snprintf(buf, sizeof buf, "%02d:%02d", (int)(rnd() % 24), (int)(rnd() % 60)); sp["start_time"] = buf;
        Json wd = Json::array(); int n = 1 + (int)(rnd() % 7); for (int i = 0; i < n; ++i) wd.push((i64)(rnd() % 7)); sp["weekdays"] = wd;
        if (rnd() % 2) sp["week_interval"] = (i64)(1 + rnd() % 6);
        if (rnd() % 2) { snprintf(buf, sizeof buf, "%04d-%02d-%02d", 2024 + (int)(rnd() % 4), 1 + (int)(rnd() % 12), 1 + (int)(rnd() % 28)); sp["anchor"] = buf; }
    } else {
        sp["mode"] = "monthly"; snprintf(buf, sizeof buf, "%02d:%02d", (int)(rnd() % 24), (int)(rnd() % 60)); sp["start_time"] = buf;
        if (rnd() % 4 == 0) sp["day"] = "last"; else sp["day"] = (i64)(1 + rnd() % 31);
        if (rnd() % 2) sp["month_interval"] = (i64)(1 + rnd() % 14);
    }
    return sp;
}

int main(int argc, char** argv)
{
    u64 seed = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1; int cases = argc > 2 ? atoi(argv[2]) : 300;
    rs = seed * 0x9E3779B97F4A7C15ull | 1; for (int i = 0; i < 8; ++i) rnd();
    printf("t_sched_prop seed %llu, %d cases\n", (unsigned long long)seed, cases);
    for (int c = 0; c < cases; ++c) {
        Json sp = RandomSchedule(); Str e; CHECK(ScheduleValidate(sp, e));
        i64 after = CivilToMin(2024 + (int)(rnd() % 5), 1 + (int)(rnd() % 12), 1 + (int)(rnd() % 28), (int)(rnd() % 24), (int)(rnd() % 60));
        i64 got = 0; bool has = ScheduleNextMin(sp, after, got);
        // oracle: the first firing minute within 3 years
        i64 want = -1; i64 limit = after + 3 * 366 * 1440LL;
        for (i64 m = after + 1; m <= limit; ++m) if (Fires(sp, m)) { want = m; break; }
        if (want < 0) { if (has && got <= limit) { CHECK(false); printf("  got %s but oracle sees none: %s\n", FormatLocalMin(got).c_str(), sp.dump().c_str()); } }
        else if (!has || got != want) { CHECK(false); printf("  after %s: got %s want %s: %s\n", FormatLocalMin(after).c_str(), has ? FormatLocalMin(got).c_str() : "none", FormatLocalMin(want).c_str(), sp.dump().c_str()); }
        // the chain is strictly increasing
        i64 cur = after; for (int k = 0; k < 5; ++k) { i64 n; if (!ScheduleNextMin(sp, cur, n)) break; CHECK(n > cur); cur = n; }
        CHECK(ScheduleDescribe(sp).size() > 3);
    }
    // hostile values: no hang, no UB
    const i64 hostile[] = { 0, -1, 1, 2, 1000000, 1000000000ll, 4611686018427387904ll, 9223372036854775807ll, -9223372036854775807ll - 1 };
    double t0 = now();
    for (i64 v : hostile) {
        for (int mode = 0; mode < 3; ++mode) {
            Json sp = Json::object(); sp["enabled"] = true;
            if (mode == 0) { sp["mode"] = "every"; Json ev = Json::object(); ev["value"] = v; ev["unit"] = rnd() % 2 ? "days" : "minutes"; sp["every"] = ev; sp["first_time"] = "2026-01-01T00:00"; }
            else if (mode == 1) { sp["mode"] = "weekly"; sp["start_time"] = "01:00"; Json wd = Json::array(); wd.push((i64)3); sp["weekdays"] = wd; sp["week_interval"] = v; }
            else { sp["mode"] = "monthly"; sp["start_time"] = "01:00"; sp["day"] = 31; sp["month_interval"] = v; }
            Str ve; bool valid = ScheduleValidate(sp, ve);
            i64 n = 0; double s0 = now(); bool has = ScheduleNextMin(sp, CivilToMin(2026, 10, 24, 8, 0), n); (void)has;
            double dt = now() - s0;
            if (dt > 2.0) { CHECK(false); printf("  SLOW %.1fs: %s (valid=%d)\n", dt, sp.dump().c_str(), (int)valid); }
            i64 un = 0; ScheduleNext(sp, (i64)1792000000, un);
        }
    }
    // hostile dates and clocks
    const char* texts[] = { "2026-10-24T08:00", "99999-01-01T00:00", "-5-01-01", "2026-13-01", "2026-02-30", "2026-10-24T24:00", "2026-10-24T08:60", "2026-10-24X08:00", "", "2026", "2026-10-24T08", "0000-01-01", "2147483647-12-31T23:59", "2026-10-24T08:00garbage", "2026-1-1T1:1" };
    for (const char* t : texts) { i64 o = 0; if (ParseLocalTime(t, o)) { (void)FormatLocalMin(o); i64 u = LocalMinToUnix(o); (void)UnixToLocalMin(u); } }
    Json win = Json::array(); { Json w = Json::object(); w["from"] = "00:00"; w["to"] = "00:00"; Json d = Json::array(); d.push((i64)0); w["days"] = d; w["mbit"] = 1e308; win.push(w); }
    for (i64 m = 0; m < 3000; m += 7) { WindowState s = WindowsAt(win, m); (void)s; }
    WindowsAt(win, (i64)9223372036854775807ll / 4); WindowsAt(win, -(i64)9223372036854775807ll / 4);
    printf("%d failures, hostile section %.2fs\n", fails, now() - t0);
    return fails != 0;
}
