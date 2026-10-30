#include "schedule.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace vm2b {

// ---- civil dates (proleptic Gregorian, days since 1970-01-01) --------------------------------------------------
static i64 DaysFromCivil(i64 y, int m, int d)
{
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void CivilFromDays(i64 z, int& y, int& m, int& d)
{
    z += 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 yy = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2));
}

static int DaysInMonth(int y, int m)
{
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

static i64 FloorDiv(i64 a, i64 b) { i64 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static i64 FloorMod(i64 a, i64 b) { return a - FloorDiv(a, b) * b; }
static int WeekdayOfDay(i64 day) { return (int)FloorMod(day + 3, 7); }          // 1970-01-01 was a Thursday; Monday = 0

i64 CivilToMin(int y, int m, int d, int hh, int mm) { return DaysFromCivil(y, m, d) * 1440 + hh * 60 + mm; }

bool ParseLocalTime(const char* s, i64& out)
{
    int y, mo, d, h = 0, mi = 0; int n = 0;
    if (sscanf(s, "%d-%d-%d%n", &y, &mo, &d, &n) != 3) return false;
    const char* p = s + n;
    if (*p == 'T' || *p == ' ') { if (sscanf(p + 1, "%d:%d", &h, &mi) != 2) return false; }
    else if (*p) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > DaysInMonth(y, mo) || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    out = CivilToMin(y, mo, d, h, mi);
    return true;
}

static bool ParseClock(const Json& j, int& h, int& m)
{
    Str s = j.str("");
    return sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60 && s.size() >= 3;
}

Str FormatLocalMin(i64 min)
{
    i64 day = FloorDiv(min, 1440); int mm = (int)FloorMod(min, 1440); int y, mo, d; CivilFromDays(day, y, mo, d);
    return Str::fmt("%04d-%02d-%02d %02d:%02d", y, mo, d, mm / 60, mm % 60);
}

static i64 EveryStepMin(const Json& ev)
{
    Str u = ev.get("unit").str(); i64 v = ev.get("value").integer();
    i64 mul = u == "minutes" ? 1 : u == "hours" ? 60 : u == "days" ? 1440 : 0;
    if (v > 100000000ll) return 0;                                                  // ~190 years of minutes: also keeps v * mul far from overflow
    return mul && v >= 1 ? v * mul : 0;
}

bool ScheduleValidate(const Json& sp, Str& err)
{
    if (!sp.is_object() || !sp.get("enabled").boolean()) return true;
    Str mode = sp.get("mode").str();
    i64 t;
    if (mode == "every") {
        if (!EveryStepMin(sp.get("every"))) { err = "interval: a number >= 1 and a unit minutes / hours / days"; return false; }
        if (!ParseLocalTime(sp.get("first_time").str().c_str(), t)) { err = "the first start time must look like 2026-10-24T08:00"; return false; }
    } else if (mode == "weekly") {
        int h, m; if (!ParseClock(sp.get("start_time"), h, m)) { err = "the start time must look like 22:30"; return false; }
        const Json& wd = sp.get("weekdays");
        if (!wd.size()) { err = "choose at least one day of the week"; return false; }
        for (size_t i = 0; i < wd.size(); ++i) { i64 d = wd.at(i).integer(-1); if (d < 0 || d > 6) { err = "days of the week are 0 (Monday) .. 6 (Sunday)"; return false; } }
        if (sp.has("week_interval") && (sp.get("week_interval").integer() < 1 || sp.get("week_interval").integer() > 5200)) { err = "the week interval must be 1..5200"; return false; }
        if (sp.has("anchor") && !ParseLocalTime(sp.get("anchor").str().c_str(), t)) { err = "bad anchor date"; return false; }
    } else if (mode == "monthly") {
        int h, m; if (!ParseClock(sp.get("start_time"), h, m)) { err = "the start time must look like 03:00"; return false; }
        const Json& d = sp.get("day");
        if (!(d.is_string() ? d.str() == "last" : (d.integer(0) >= 1 && d.integer(0) <= 31))) { err = "the day of the month is 1..31 or \"last\""; return false; }
        if (sp.has("month_interval") && (sp.get("month_interval").integer() < 1 || sp.get("month_interval").integer() > 1200)) { err = "the month interval must be 1..1200"; return false; }
    } else if (mode == "once") {
        if (!ParseLocalTime(sp.get("at").str().c_str(), t)) { err = "the start time must look like 2026-10-24T08:00"; return false; }
    } else { err = "unknown schedule mode"; return false; }
    return true;
}

bool ScheduleNextMin(const Json& sp, i64 after, i64& out)
{
    if (!sp.is_object() || !sp.get("enabled").boolean()) return false;
    Str mode = sp.get("mode").str();
    if (mode == "once") {
        i64 at; if (!ParseLocalTime(sp.get("at").str().c_str(), at)) return false;
        if (at > after) { out = at; return true; }
        return false;
    }
    if (mode == "every") {
        i64 step = EveryStepMin(sp.get("every")), first;
        if (!step || !ParseLocalTime(sp.get("first_time").str().c_str(), first)) return false;
        if (after < first) { out = first; return true; }
        out = first + step * ((after - first) / step + 1);
        return true;
    }
    int h = 0, mi = 0;
    if (!ParseClock(sp.get("start_time"), h, mi)) { h = 12; mi = 0; }
    if (mode == "weekly") {
        i64 interval = sp.get("week_interval").integer(1); if (interval < 1) interval = 1; if (interval > 5200) interval = 5200;   // a hand-edited file must not stall the scheduler
        i64 anchorDay = DaysFromCivil(2000, 1, 3);                                  // a Monday
        i64 am; if (sp.has("anchor") && ParseLocalTime(sp.get("anchor").str().c_str(), am)) anchorDay = FloorDiv(am, 1440);
        anchorDay -= WeekdayOfDay(anchorDay);                                       // back to that week's Monday
        const Json& wd = sp.get("weekdays");
        i64 day = FloorDiv(after, 1440);
        for (i64 k = 0; k < 7 * interval * 2 + 8; ++k, ++day) {
            bool listed = false; for (size_t i = 0; i < wd.size(); ++i) if (wd.at(i).integer(-1) == WeekdayOfDay(day)) listed = true;
            if (!listed || FloorMod(FloorDiv(day - anchorDay, 7), interval) != 0) continue;
            i64 cand = day * 1440 + h * 60 + mi;
            if (cand > after) { out = cand; return true; }
        }
        return false;
    }
    if (mode == "monthly") {
        i64 interval = sp.get("month_interval").integer(1); if (interval < 1) interval = 1; if (interval > 1200) interval = 1200;
        const Json& dj = sp.get("day"); bool last = dj.is_string() && dj.str() == "last"; int dd0 = last ? 31 : (int)dj.integer(1);
        int y, mo, d; CivilFromDays(FloorDiv(after, 1440), y, mo, d);
        i64 idx = (i64)y * 12 + mo - 1;
        for (i64 k = 0; k < interval * 2 + 14; ++k) {
            i64 i = idx + k;
            if (FloorMod(i - 2000 * 12, interval) != 0) continue;
            int yy = (int)FloorDiv(i, 12), mm = (int)FloorMod(i, 12) + 1, lastDay = DaysInMonth(yy, mm);
            int dd = last ? lastDay : (dd0 < lastDay ? dd0 : lastDay);
            i64 cand = CivilToMin(yy, mm, dd, h, mi);
            if (cand > after) { out = cand; return true; }
        }
        return false;
    }
    return false;
}

Str ScheduleDescribe(const Json& sp)
{
    if (!sp.get("enabled").boolean()) return "disabled";
    Str mode = sp.get("mode").str();
    if (mode == "every") return Str::fmt("every %lld %s from %s", (long long)sp.get("every").get("value").integer(), sp.get("every").get("unit").str().c_str(), sp.get("first_time").str().replace_all("T", " ").c_str());
    if (mode == "weekly") {
        static const char* dn[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" }; Str days;
        for (int d = 0; d < 7; ++d) for (size_t i = 0; i < sp.get("weekdays").size(); ++i) if (sp.get("weekdays").at(i).integer(-1) == d) { if (!days.empty()) days += ","; days += dn[d]; }
        i64 n = sp.get("week_interval").integer(1);
        return Str::fmt("%s on %s at %s", n <= 1 ? "every week" : Str::fmt("every %lld weeks", (long long)n).c_str(), days.c_str(), sp.get("start_time").str().c_str());
    }
    if (mode == "monthly") {
        i64 n = sp.get("month_interval").integer(1); Str d = sp.get("day").is_string() ? sp.get("day").str() : Str::fmt("%lld", (long long)sp.get("day").integer(1));
        return Str::fmt("%s on day %s at %s", n <= 1 ? "every month" : Str::fmt("every %lld months", (long long)n).c_str(), d.c_str(), sp.get("start_time").str().c_str());
    }
    return "once at " + sp.get("at").str().replace_all("T", " ");
}

// ---- windows ---------------------------------------------------------------------------------------------------
bool WindowsValidate(const Json& w, Str& err)
{
    if (w.is_null()) return true;
    if (!w.is_array()) { err = "the windows must be a list"; return false; }
    for (size_t i = 0; i < w.size(); ++i) {
        const Json& x = w.at(i); int h, m;
        if (!ParseClock(x.get("from"), h, m) || !ParseClock(x.get("to"), h, m)) { err = "a window needs from / to times like 08:00"; return false; }
        if (!x.get("days").size()) { err = "a window needs at least one day"; return false; }
        for (size_t k = 0; k < x.get("days").size(); ++k) { i64 d = x.get("days").at(k).integer(-1); if (d < 0 || d > 6) { err = "window days are 0 (Monday) .. 6 (Sunday)"; return false; } }
        if (x.get("mbit").num(0) < 0) { err = "the speed limit cannot be negative"; return false; }
        if (!(x.get("mbit").num(0) <= 1e7)) { err = "the speed limit is too large"; return false; }
    }
    return true;
}

WindowState WindowsAt(const Json& w, i64 minute)
{
    WindowState st = { true, 0, -1 };
    if (!w.is_array() || !w.size()) return st;
    st.allowed = false;
    i64 day = FloorDiv(minute, 1440); int mod = (int)FloorMod(minute, 1440); int wd = WeekdayOfDay(day);
    i64 best = -1;                                                                  // the nearest minute at which any window starts or ends
    auto consider = [&](i64 t) { if (t > minute && (best < 0 || t < best)) best = t; };
    for (size_t i = 0; i < w.size(); ++i) {
        const Json& x = w.at(i); int fh, fm, th, tm; if (!ParseClock(x.get("from"), fh, fm) || !ParseClock(x.get("to"), th, tm)) continue;
        int from = fh * 60 + fm, to = th * 60 + tm;
        // the window that began on `day` and the one that began on the day before (wrapping over midnight)
        for (int back = 0; back < 2; ++back) {
            i64 d0 = day - back; int wd0 = WeekdayOfDay(d0);
            bool listed = false; for (size_t k = 0; k < x.get("days").size(); ++k) if (x.get("days").at(k).integer(-1) == wd0) listed = true;
            if (!listed) continue;
            i64 startAbs = d0 * 1440 + from, endAbs = d0 * 1440 + (to > from ? to : to + 1440);
            if (to == from) endAbs = startAbs + 1440;                               // from == to: the whole day
            consider(startAbs); consider(endAbs);
            if (minute >= startAbs && minute < endAbs) {
                double mb = x.get("mbit").num(0);
                if (!st.allowed) { st.allowed = true; st.mbit = mb; }
                else if (mb > 0 && (st.mbit <= 0 || mb < st.mbit)) st.mbit = mb;
            }
        }
        // starts / ends of the windows of the next day (so that "changesAt" is never far off)
        for (int fwd = 1; fwd <= 7; ++fwd) {
            i64 d1 = day + fwd; int wd1 = WeekdayOfDay(d1); bool listed = false;
            for (size_t k = 0; k < x.get("days").size(); ++k) if (x.get("days").at(k).integer(-1) == wd1) listed = true;
            if (listed) { consider(d1 * 1440 + from); consider(d1 * 1440 + (to > from ? to : to + 1440)); }
        }
    }
    (void)wd; (void)mod;
    st.changesAtMin = best;
    return st;
}

// ---- time zone -------------------------------------------------------------------------------------------------
#ifdef _WIN32
// The C runtime (msvcrt.dll) reads the machine's time zone ONCE, at its first use: a service that runs for weeks would go on with the old zone after the administrator
// changed it (found on a real Windows 10: tzutil /s while the service ran -> "next run" and the scheduler still in the old zone until the service was restarted).
// _tzset() reads the zone again; every few seconds is enough (the scheduler asks several times per tick).
static void RefreshZone()
{
    static volatile LONG64 last = 0;
    LONG64 now = (LONG64)GetTickCount64();
    if (last == 0 || now - last > 2000 || now < last) { last = now; _tzset(); }
}
static bool LocalTm(i64 t, struct tm* m) { RefreshZone(); __time64_t s = (__time64_t)t; return _localtime64_s(m, &s) == 0; }
static i64 MkTime(struct tm* t) { RefreshZone(); return (i64)_mktime64(t); }
#else
static bool LocalTm(i64 t, struct tm* m) { time_t s = (time_t)t; return localtime_r(&s, m) != nullptr; }
static i64 MkTime(struct tm* t) { return (i64)mktime(t); }
#endif

i64 UnixToLocalMin(i64 t)
{
    struct tm m;
    if (!LocalTm(t, &m)) return FloorDiv(t, 60);
    return CivilToMin(m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min);
}

i64 LocalMinToUnix(i64 lm)
{
    i64 day = FloorDiv(lm, 1440); int mm = (int)FloorMod(lm, 1440); int y, mo, d; CivilFromDays(day, y, mo, d);
    struct tm t; memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = mm / 60; t.tm_min = mm % 60; t.tm_isdst = -1;
    i64 r = MkTime(&t);
    if (r == -1) return lm * 60;
    // A wall-clock minute that does not exist (the clock jumped over it in spring) is placed differently by the C libraries: glibc puts it behind the gap, the
    // Windows CRT one hour BEFORE it (found on a real Windows 10 in the W. Europe zone: "daily at 02:30" ran at 01:30 on the day of the change; a "once at 02:30"
    // job ran an hour early). A task must never start before its time, so everywhere the answer is the first instant at or after the requested minute.
    if (UnixToLocalMin(r) != lm) {
        i64 from = r - 3 * 3600, to = r + 3 * 3600;
        for (i64 u = from - from % 60; u <= to; u += 60) if (UnixToLocalMin(u) >= lm) { r = u; break; }
        return r;
    }
    // A minute that happens TWICE (the clock was set back in autumn): the answer is the FIRST occurrence. The Windows CRT returns the second one (standard time), and then the
    // scheduler, which walks through the first pass in real time, never meets the slot: "every 30 minutes" lost 1-2 hours and a daily 01:30 never ran on that day (found on a
    // real Windows 10, GMT / W. Europe / Pacific). A slot of the repeated hour runs once, in its first pass; the second pass does not repeat it.
    static const int kShifts[] = { 7200, 3600, 1800 };
    for (int k = 0; k < 3; ++k) if (UnixToLocalMin(r - kShifts[k]) == lm) { r -= kShifts[k]; break; }
    return r;
}

bool ScheduleNext(const Json& sp, i64 afterUnix, i64& nextUnix)
{
    i64 n, cursor = UnixToLocalMin(afterUnix);
    // The wall clock repeats an hour when it is set back: a local minute of that hour may convert to its FIRST occurrence (the C library
    // decides), which is before `afterUnix` when we are already in the second pass. That run belongs to the first pass (it was due, and
    // fired, then), so look for the next local minute instead of returning a time in the past (a duplicate run).
    for (int i = 0; i < 8; ++i) {
        if (!ScheduleNextMin(sp, cursor, n)) return false;
        i64 u = LocalMinToUnix(n);
        if (u > afterUnix) { nextUnix = u; return true; }
        cursor = n;
    }
    nextUnix = afterUnix + 60;
    return true;
}

} // namespace vm2b
