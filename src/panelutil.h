// VM2Backup — the pure helpers of the control panel (VM2Backup.exe): no window code, so that the autotest can check them.
#pragma once
#include "base.h"

namespace vm2b {

enum PanelWebResult { PANEL_WEB_OK = 0, PANEL_WEB_OFF = 1, PANEL_WEB_NOT_LISTENING = 2 };

// "127.0.0.1" -> "127.0.0.1", "::1" -> "[::1]", "fe80::1%12" -> "[fe80::1%2512]" (a host as it is written in a URL)
Str PanelUrlHost(const Str& address);
// The address the "Open web interface" button opens, from the answer of web.status: the first listening address, a loopback one preferred
// (127.x, then ::1); 0.0.0.0 / :: (all interfaces) are reached as 127.0.0.1 / ::1.  url = "https://<host>:<port>/".
PanelWebResult PanelWebUrl(const Json& webStatus, Str& url);
} // namespace vm2b
