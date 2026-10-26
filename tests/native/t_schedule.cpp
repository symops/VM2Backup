#include <stdlib.h>
#include <time.h>
#include <initializer_list>
#include "../../src/schedule.h"
#include "../../src/tests_schedule_vectors.inc"
using namespace vm2b;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int fails = 0;

// DST edges: the Unix-time wrappers (ScheduleNext) run on the machine's zone. Whatever the zone does, the next run must lie strictly
// after `after`, must not be absurdly far (a skipped / repeated hour only), and following the chain must never go backwards or loop.
static void DstProperty(const char* zone)
{
    setenv("TZ", zone, 1); tzset();
    Json sps = Json::array();
    const char* texts[] = {
        "{\"enabled\":true,\"mode\":\"every\",\"every\":{\"value\":30,\"unit\":\"minutes\"},\"first_time\":\"2026-01-01T00:00\"}",
        "{\"enabled\":true,\"mode\":\"every\",\"every\":{\"value\":1,\"unit\":\"hours\"},\"first_time\":\"2026-01-01T00:15\"}",
        "{\"enabled\":true,\"mode\":\"every\",\"every\":{\"value\":1,\"unit\":\"days\"},\"first_time\":\"2026-01-01T02:30\"}",
        "{\"enabled\":true,\"mode\":\"weekly\",\"start_time\":\"02:30\",\"weekdays\":[0,1,2,3,4,5,6],\"week_interval\":1}",
        "{\"enabled\":true,\"mode\":\"weekly\",\"start_time\":\"01:30\",\"weekdays\":[0,1,2,3,4,5,6],\"week_interval\":1}",
        "{\"enabled\":true,\"mode\":\"monthly\",\"start_time\":\"02:15\",\"day\":\"last\",\"month_interval\":1}",
    };
    int bad = 0;
    for (const char* tx : texts) {
        Json sp; Str e; CHECK(Json::parse(tx, strlen(tx), sp, e));
        // two years of chain, started in a different place each time so that every DST edge is crossed from every phase
        for (int off = 0; off < 7; ++off) {
            i64 u = (i64)1767225600 + off * 3600 * 5 + 61;                 // 2026-01-01 + a few hours
            for (int k = 0; k < 4000 && u < (i64)1767225600 + 730 * 86400; ++k) {
                i64 n = 0;
                if (!ScheduleNext(sp, u, n)) { CHECK(false); break; }
                if (n <= u || n - u > 40 * 86400LL) { if (bad++ < 8) printf("%s: after %lld next %lld (%s)\n", zone, (long long)u, (long long)n, tx); break; }
                u = n;
            }
        }
        // every single minute around the edges of 2026: the answer is always in the future
        for (const char* d : { "2026-03-08", "2026-03-29", "2026-10-04", "2026-10-25", "2026-11-01", "2026-04-05" }) {
            i64 lm; ParseLocalTime(d, lm); i64 u0 = LocalMinToUnix(lm) - 6 * 3600;
            for (i64 u = u0; u < u0 + 36 * 3600; u += 60) { i64 n; if (ScheduleNext(sp, u, n) && n <= u) { if (bad++ < 8) printf("%s: edge after %lld next %lld (%s)\n", zone, (long long)u, (long long)n, tx); } }
        }
    }
    CHECK(bad == 0);
}
int main() {
    Json cases; Str e; CHECK(Json::parse(kScheduleVectors, strlen(kScheduleVectors), cases, e));
    int bad = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const Json& c = cases.at(i); i64 n = 0;
        bool got = ScheduleNextMin(c.get("sp"), c.get("after").integer(), n);
        bool want = !c.get("next").is_null();
        if (got != want || (got && n != c.get("next").integer())) {
            if (bad++ < 5) printf("case %zu differs: %s after %s: got %s want %s\n", i, c.get("sp").dump().c_str(), FormatLocalMin(c.get("after").integer()).c_str(), got ? FormatLocalMin(n).c_str() : "none", want ? FormatLocalMin(c.get("next").integer()).c_str() : "none");
        }
        Str ve; CHECK(ScheduleValidate(c.get("sp"), ve));
    }
    CHECK(bad == 0); printf("%zu vector cases, %d differ\n", cases.size(), bad);
    Json w = Json::array(); // windows: brute force against a simple rule, minute by minute over three weeks
    { Json a = Json::object(); Json d = Json::array(); for (int k = 0; k < 5; ++k) d.push(k); a["days"] = d; a["from"] = "08:00"; a["to"] = "18:00"; a["mbit"] = 20; w.push(a); }
    { Json a = Json::object(); Json d = Json::array(); for (int k = 0; k < 7; ++k) d.push(k); a["days"] = d; a["from"] = "22:00"; a["to"] = "06:00"; a["mbit"] = 0; w.push(a); }
    Str we; CHECK(WindowsValidate(w, we));
    i64 start = CivilToMin(2026, 10, 19, 0, 0);                                    // a Monday
    int mism = 0;
    for (i64 t = start; t < start + 21 * 1440; ++t) {
        WindowState st = WindowsAt(w, t);
        i64 day = (t / 1440), mod = t % 1440; int wd = (int)((day + 3) % 7);
        bool in1 = wd <= 4 && mod >= 8 * 60 && mod < 18 * 60;
        int pwd = (int)((day + 2) % 7);
        bool in2 = mod >= 22 * 60 || mod < 6 * 60; (void)pwd;
        bool allowed = in1 || in2;
        double mb = in1 ? 20 : 0;
        if (in1 && in2) mb = 20;
        if (st.allowed != allowed || (allowed && st.mbit != mb)) { if (mism++ < 3) printf("window mismatch at %s: allowed %d/%d mbit %g/%g\n", FormatLocalMin(t).c_str(), st.allowed, allowed, st.mbit, mb); }
        if (st.changesAtMin >= 0 && st.changesAtMin <= t) { mism++; printf("changesAt not in the future at %s\n", FormatLocalMin(t).c_str()); }
        if (t + 1 < start + 21 * 1440 && st.changesAtMin > t + 1) { WindowState n2 = WindowsAt(w, t + 1); if (n2.allowed != st.allowed || n2.mbit != st.mbit) { mism++; if (mism < 5) printf("changed before changesAt at %s\n", FormatLocalMin(t).c_str()); } }
    }
    CHECK(mism == 0);
    Str err; Json bad1 = Json::object(); bad1["enabled"] = true; bad1["mode"] = "weekly"; bad1["start_time"] = "25:00"; CHECK(!ScheduleValidate(bad1, err));
    for (const char* z : { "Europe/Berlin", "America/New_York", "Australia/Lord_Howe", "UTC", "Asia/Kolkata" }) DstProperty(z);
    printf(fails ? "schedule FAILED %d\n" : "schedule ok\n", fails); return fails != 0;
}
