#!/bin/bash
# Runs the window test of the autotest (group "gui") under Wine with its own Xvfb display and its own Wine prefix (a Wine server
# that was started without a display never loads the graphics driver).
#   tools/run-gui-tests.sh [extra vm2bautotest arguments]      (NO_LAYOUT=1 skips the 17-language layout check, tools/run-gui-layout.sh)
# The group "gui" holds: program_selftest (the control panel opens in every state), gui_interaction (src/guitest.cpp plays the user: state display,
# open web interface, open logs folder, all 17 languages, Enter / Esc, fixed size), gui_service_stops_and_restarts (Start / Stop / Restart buttons) and
# panel_without_service.  The unit test panel_web_url (group "panel") needs no display.
#   GUI_BIN=<dir> runs the programs of that folder instead of build/x64.
cd "$(dirname "$0")/.."
ROOT=$PWD; BIN=${GUI_BIN:-$ROOT/build/x64}
W=${GUI_TEST_DIR:-${TMPDIR:-/tmp}/vm2b-guitest}
mkdir -p "$W"
export WINEPREFIX=${WINEPREFIX_GUI:-$W/prefix} WINEDEBUG=-all VM2B_GUI_TESTS=1
DISP=${GUI_DISPLAY:-:96}
XPID=
if [ ! -S "/tmp/.X11-unix/X${DISP#:}" ]; then Xvfb $DISP -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!; sleep 2; fi
export DISPLAY=$DISP
nice -n 19 wine64 "$BIN/vm2bautotest.exe" --only gui "$@"
RC=$?
[ -n "$XPID" ] && kill $XPID 2>/dev/null
# the layout check of every view and dialog in the 17 languages (texts that do not fit their controls)
if [ -z "$NO_LAYOUT" ]; then "$ROOT/tools/run-gui-layout.sh" --bin "$BIN" || RC=1; fi
exit $RC
