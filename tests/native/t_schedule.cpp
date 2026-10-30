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
// Round 8 (a real Windows 10, W. Europe): a local minute that does not exist (spring forward) must convert to the FIRST instant at or after the gap on every
// C library (the Windows CRT used to put it an hour before the gap: "daily 02:30" ran at 01:30). Every minute of the day of the change, three zones.
static void NonexistentMinute(const char* zone, const char* date)
{
    setenv("TZ", zone, 1); tzset();
    i64 day; ParseLocalTime(date, day);
    i64 prev = -1; int gapMinutes = 0, bad = 0;
    for (i64 lm = day; lm < day + 1440; ++lm) {
        i64 u = LocalMinToUnix(lm), back = UnixToLocalMin(u);
        if (back < lm) { if (bad++ < 5) printf("%s %s: local minute %s converts to an instant that is %lld min BEFORE it\n", zone, date, FormatLocalMin(lm).c_str(), (long long)(lm - back)); }
        if (back > lm) { ++gapMinutes; if (UnixToLocalMin(u - 60) >= lm && bad++ < 5) printf("%s %s: nonexistent %s is not the first instant after the gap\n", zone, date, FormatLocalMin(lm).c_str()); }
        if (prev >= 0 && u < prev) { if (bad++ < 5) printf("%s %s: conversion goes backwards at %s\n", zone, date, FormatLocalMin(lm).c_str()); }
        prev = u;
    }
    CHECK(bad == 0); CHECK(gapMinutes > 0);
    // "once at 02:30" / "daily 02:30" on that day: never earlier than 02:30 local time
    for (int w = 0; w < 2; ++w) {
        Json sp = Json::object(); sp["enabled"] = true;
        if (w == 0) { sp["mode"] = "once"; sp["at"] = Str(date) + "T02:30"; }
        else { sp["mode"] = "weekly"; sp["start_time"] = "02:30"; Json d = Json::array(); for (int k = 0; k < 7; ++k) d.push(k); sp["weekdays"] = d; sp["week_interval"] = 1; }
        i64 n = 0; CHECK(ScheduleNext(sp, LocalMinToUnix(day) - 1, n));
        CHECK(UnixToLocalMin(n) >= day + 150 && UnixToLocalMin(n) < day + 1440);
    }
}

// Round 8: a local minute that happens twice (clock set back) converts to its FIRST occurrence on every C library (the Windows CRT returned the second one:
// the scheduler never met such a slot and lost an hour of "every 30 minutes"; a daily slot of that hour did not run on that day at all).
static void AmbiguousMinute(const char* zone, const char* date)
{
    setenv("TZ", zone, 1); tzset();
    i64 day; ParseLocalTime(date, day);
    int twice = 0, bad = 0;
    for (i64 lm = day; lm < day + 1440; ++lm) {
        i64 u = LocalMinToUnix(lm);
        if (UnixToLocalMin(u) != lm) continue;
        bool earlier = false; for (int sh : { 7200, 3600, 1800 }) if (UnixToLocalMin(u - sh) == lm) earlier = true;
        if (earlier && bad++ < 5) printf("%s %s: %s is converted to its SECOND occurrence\n", zone, date, FormatLocalMin(lm).c_str());
        // is the minute ambiguous at all? then the instant one shift later must show the same local minute
        for (int sh : { 7200, 3600, 1800 }) if (UnixToLocalMin(u + sh) == lm) { ++twice; break; }
    }
    CHECK(bad == 0); CHECK(twice > 0);
    // the chain "every 30 minutes" through the repeated hour never goes back and every slot of the first pass is met in real time
    Json sp = Json::object(); sp["enabled"] = true; sp["mode"] = "every"; Json ev = Json::object(); ev["value"] = 30; ev["unit"] = "minutes"; sp["every"] = ev; sp["first_time"] = Str(date) + "T00:00";
    i64 u = LocalMinToUnix(day) - 1, last = 0; int runs = 0;
    for (int k = 0; k < 60; ++k) { i64 n; CHECK(ScheduleNext(sp, u, n) && n > u); if (last) CHECK(n - last <= 100 * 60); last = n; u = n; if (UnixToLocalMin(n) >= day + 1440) break; ++runs; }
    CHECK(runs >= 40);
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
    NonexistentMinute("Europe/Berlin", "2027-03-28"); NonexistentMinute("America/New_York", "2027-03-14"); NonexistentMinute("Australia/Lord_Howe", "2026-10-04"); NonexistentMinute("Europe/Berlin", "2026-03-29");
    AmbiguousMinute("Europe/Berlin", "2026-10-25"); AmbiguousMinute("America/New_York", "2026-11-01"); AmbiguousMinute("Australia/Lord_Howe", "2027-04-04"); AmbiguousMinute("Europe/London", "2027-10-31");
    printf(fails ? "schedule FAILED %d\n" : "schedule ok\n", fails); return fails != 0;
}
