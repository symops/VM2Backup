// VM2Backup — the control panel (VM2Backup.exe): ids of its controls and the test seams used by src/guitest.cpp.
#pragma once
#include "app.h"

enum { PANEL_ID_STATE = 3001, PANEL_ID_VER, PANEL_ID_DATA, PANEL_ID_WEB, PANEL_ID_START, PANEL_ID_STOP, PANEL_ID_RESTART, PANEL_ID_OPENWEB,
       PANEL_ID_LOGS, PANEL_ID_LANGLBL, PANEL_ID_LANG };       // the Exit button has the id IDCANCEL (Esc presses it)

enum { PANEL_SVC_START = 0, PANEL_SVC_STOP = 1, PANEL_SVC_RESTART = 2 };

// Seams of the window test (/guitest): the test has no Windows service manager and must not start a browser.
struct PanelHooks {
    bool (*service)(int op);                        // PANEL_SVC_START / PANEL_SVC_STOP: do it (blocks until done); set = the service counts as installed
    void (*open)(const wchar_t* target);            // open a URL / a folder instead of ShellExecute
};
extern PanelHooks g_panelHooks;
