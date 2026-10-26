// VM2Backup — what the control panel share.
#pragma once
#include "client.h"
#include "ui.h"

using ui::Str; using ui::Json; using ui::Vec;

#define WM_APP_REFRESH (WM_APP + 40)               // the panel: refresh the state now
#define WM_APP_SVCDONE (WM_APP + 50)               // the panel: a service start / stop / restart finished (wParam: 0 = ok, 1 = stop failed, 2 = start failed)
