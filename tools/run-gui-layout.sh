#!/bin/bash
# Layout check of the control panel under Wine: opens it in each of the 17 languages, in every state (VM2Backup.exe /selftest) and lists
# the controls whose text is cut off / wrapped past its control / outside the form (Form::checkLayout in ui.cpp). Exit code 1 = problems.
#   tools/run-gui-layout.sh [--bin dir] [--langs "1 2 3"] [--dpi 96|144|192]
# Own Xvfb display (GUI_DISPLAY, default :95) and Wine prefix (WINEPREFIX_GUI); the service runs in a console of its own.
cd "$(dirname "$0")/.."
ROOT=$PWD; BIN=$ROOT/build/x64; LANGS="$(seq -s' ' 1 17)"; DPI=
while [ $# -gt 0 ]; do case "$1" in --bin) BIN=$2; shift 2;; --langs) LANGS=$2; shift 2;; --dpi) DPI=$2; shift 2;; *) echo "unknown $1"; exit 2;; esac; done
W=${GUI_TEST_DIR:-${TMPDIR:-/tmp}/vm2b-guilayout}; mkdir -p "$W/data"; rm -rf "$W/data"/* "$W"/layout-*.txt
export WINEPREFIX=${WINEPREFIX_GUI:-$W/prefix} WINEDEBUG=-all
DISP=${GUI_DISPLAY:-:95}; XPID=
if [ ! -S "/tmp/.X11-unix/X${DISP#:}" ]; then Xvfb $DISP -screen 0 ${GUI_SCREEN:-3840x2160x24} >/dev/null 2>&1 & XPID=$!; sleep 2; fi
export DISPLAY=$DISP
wineserver -k 2>/dev/null; sleep 1
wine64 reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d ${DPI:-96} /f >/dev/null 2>&1
PIPE=vm2b-layout-$$
zp() { printf 'Z:%s' "$(echo "$1" | sed 's|/|\\\\|g')"; }
wine64 "$BIN/VM2BackupService.exe" /console /data "$(zp $W/data)" /pipe $PIPE >"$W/service.log" 2>&1 &
SPID=$!; sleep 4
bad=0
for L in $LANGS; do
  out="$W/layout-$L.txt"; rm -f "$out"
  VM2B_SELFTEST_LANG=$L VM2B_LAYOUT_OUT="$(zp $out)" wine64 "$BIN/VM2Backup.exe" /pipe $PIPE /selftest 250 >/dev/null 2>&1; rc=$?
  n=$(grep -vc '^#' "$out" 2>/dev/null); n=${n:-0}; dpi=$(sed -n 's/^# dpi \([0-9]*\).*/\1/p' "$out" 2>/dev/null | tr -d '\r')
  echo "language $L (dpi ${dpi:-?}): exit $rc, $n layout problem(s)"
  [ "$n" != 0 ] && { grep -v '^#' "$out" | sed "s/^/    /"; bad=1; }
  [ $rc -ne 0 ] && bad=1
done
kill $SPID 2>/dev/null; [ -n "$XPID" ] && kill $XPID 2>/dev/null
exit $bad
