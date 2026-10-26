// VM2Backup — when a scheduled task runs, and when it may use the network.
//
// The schedule is the one of the "Add Scheduled Task" dialog of VM Explorer:
//   {"enabled":true,"mode":"every","every":{"value":6,"unit":"hours"},"first_time":"2026-10-24T08:00"}
//   {"enabled":true,"mode":"weekly","start_time":"22:30","weekdays":[0,2,4],"week_interval":1,"anchor":"2026-10-19"}   (0 = Monday)
//   {"enabled":true,"mode":"monthly","start_time":"03:00","day":1,"month_interval":1}                                 (day 1..31 or "last")
//   {"enabled":true,"mode":"once","at":"2026-12-31T23:00"}
// "Windows" limit WHEN a job may run and how fast it may transfer (a working-hours limit, a night run):
//   [{"days":[0,1,2,3,4],"from":"08:00","to":"18:00","mbit":20}, {"days":[0,1,2,3,4,5,6],"from":"18:00","to":"06:00","mbit":0}]
//   no windows = always allowed, unlimited; with windows a job runs only inside one of them, at the smallest non-zero limit of the
//   windows it is in (mbit 0 = no limit); from > to wraps over midnight.
// All times are local clock times. The calculations work on "local minutes" (minutes since 1970-01-01 00:00 on the wall clock,
// no time zone), which keeps them exactly testable; the Unix-time wrappers convert with the machine's zone.
#pragma once
#include "base.h"

namespace vm2b {

bool ScheduleValidate(const Json& sp, Str& err);
// The next run strictly after `afterMin` (local minutes); false when the schedule never fires again (disabled, a past "once").
bool ScheduleNextMin(const Json& sp, i64 afterMin, i64& nextMin);
Str  ScheduleDescribe(const Json& sp);

struct WindowState { bool allowed; double mbit; i64 changesAtMin; };       // changesAtMin: the next minute at which this answer may change (-1 = never)
bool WindowsValidate(const Json& windows, Str& err);
WindowState WindowsAt(const Json& windows, i64 minute);

// Unix time <-> local minutes (uses the time zone of the machine).
i64 UnixToLocalMin(i64 unixTime);
i64 LocalMinToUnix(i64 localMin);
bool ScheduleNext(const Json& sp, i64 afterUnix, i64& nextUnix);
bool ParseLocalTime(const char* iso, i64& localMin);                       // "2026-10-24T08:00" (a date alone = 00:00)
Str  FormatLocalMin(i64 localMin);                                        // "2026-10-24 08:00"
i64  CivilToMin(int y, int m, int d, int hh, int mm);

} // namespace vm2b
