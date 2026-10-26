#!/bin/bash
# More installer tests under Wine (the basic ones are tools/test-package-wine.sh): flag combinations, a path with spaces and non-ASCII
# characters, install over a running service, update in place keeping ProgramData, moving to another folder, removal with the service
# running, a damaged / truncated Setup.exe (an error, not a crash or a hang).
# It assembles its OWN package from BUILD_X64 / BUILD_X86 (default build/x64, build/x86) in $PKG_TEST_DIR: it never touches dist/ or src/version.h.
#   BUILD_X64=... BUILD_X86=... tools/test-package-extra.sh
# Own Wine prefix (WINEPREFIX_PKG2), Xvfb display $PKG_DISPLAY (default :95) when it does not exist yet.
cd "$(dirname "$0")/.."
ROOT=$PWD
B64=${BUILD_X64:-$ROOT/build/x64}; B86=${BUILD_X86:-$ROOT/build/x86}
W=${PKG_TEST_DIR:-${TMPDIR:-/tmp}/vm2b-pkgtest2}
rm -rf "$W/dist"; mkdir -p "$W/dist/p/x64" "$W/dist/p/x86"
export WINEPREFIX=${WINEPREFIX_PKG2:-$W/prefix} WINEDEBUG=-all
DISP=${PKG_DISPLAY:-:95}; XPID=
if [ ! -S "/tmp/.X11-unix/X${DISP#:}" ]; then Xvfb $DISP -screen 0 1280x800x24 >/dev/null 2>&1 & XPID=$!; sleep 2; fi
export DISPLAY=$DISP
trap '[ -n "$XPID" ] && kill $XPID 2>/dev/null; wineserver -k 2>/dev/null' EXIT
FAILS=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# ---- own package (as package.sh does it, without touching the shared tree)
P=$W/dist/p
cp "$B86/vm2blauncher.exe" "$P/VM2Backup-Setup.exe"; cp scripts/uninstall.cmd "$P/"
for A in x64 x86; do B=$B64; [ $A = x86 ] && B=$B86; cp "$B/VM2Backup.exe" "$B/VM2BackupService.exe" "$B/VM2Backup-Setup.exe" "$P/$A/"; cp scripts/uninstall.cmd "$P/$A/"; done
(cd "$P" && find uninstall.cmd x64 x86 -type f | sort | xargs gcab -c -z ../payload.cab)
python3 -I - "$W/dist" "$B86/vm2blauncher.exe" <<'PY'
import struct, sys
d, l = sys.argv[1:3]
cab = open(d + '/payload.cab', 'rb').read()
open(d + '/Setup.exe', 'wb').write(open(l, 'rb').read() + cab + b'VM2BPKG1' + struct.pack('<Q', len(cab)))
PY
SETUP=$W/dist/Setup.exe
C=$WINEPREFIX/drive_c
PF="$C/Program Files/VM2Backup"
DATA="$C/ProgramData/VM2Backup"
run() { local t=$1; shift; nice -n 19 timeout "$t" wine "$@"; }
svc_state() { wine sc query VM2BackupService 2>&1 | sed -n 's/.*STATE *: *[0-9]* *\([A-Z_]*\).*/\1/p' | head -1; }
svc_registered() { wine sc query VM2BackupService 2>&1 | grep -q SERVICE_NAME; }
cleanup() { run 120 "$SETUP" /remove /silent >/dev/null 2>&1; sleep 4; rm -rf "$PF" "$C/Program Files/VM2Backup Ünï" "$C/Temp Dir"; }
cleanup

echo "== flags =="
run 180 "$SETUP" /install /silent /nostart; RC=$?
check "/install /silent /nostart returns 0" "[ $RC -eq 0 ]"
check "/nostart: the service is registered" "svc_registered"
check "/nostart: the service is NOT running" "[ \"\$(svc_state)\" != RUNNING ]"
LNKD=$(find "$C" -iname 'VM2Backup.lnk' 2>/dev/null | wc -l)
check "default: shortcuts were created (desktop + start menu: $LNKD)" "[ $LNKD -ge 1 ]"
run 120 "$SETUP" /remove /silent; sleep 5
check "removal after /nostart: files gone" "[ ! -e '$PF/VM2Backup.exe' ]"
check "removal after /nostart: shortcuts gone" "[ \$(find '$C' -iname 'VM2Backup.lnk' 2>/dev/null | wc -l) -eq 0 ]"
run 180 "$SETUP" /install /silent /desktop- /startmenu- /nostart; RC=$?
check "/desktop- /startmenu- install returns 0" "[ $RC -eq 0 ]"
check "/desktop- /startmenu-: no shortcut at all" "[ \$(find '$C' -iname 'VM2Backup.lnk' 2>/dev/null | wc -l) -eq 0 ]"
run 120 "$SETUP" /remove /silent; sleep 5
run 180 "$SETUP" /install /silent /desktop- /nostart; RC=$?
check "/desktop- only: one shortcut (start menu) remains" "[ \$(find '$C' -iname 'VM2Backup.lnk' 2>/dev/null | wc -l) -eq 1 ]"
run 120 "$SETUP" /remove /silent; sleep 5
run 180 "$SETUP" /silent; RC=$?
check "/silent without /install or /remove does nothing and returns" "[ ! -e '$PF/VM2Backup.exe' ]"
run 120 "$SETUP" /remove /silent; RC=$?
check "removal when nothing is installed returns 0 (or 1) without hanging: rc=$RC" "[ $RC -le 1 ]"

echo "== path with spaces and non-ASCII characters =="
DIR='C:\Program Files\VM2Backup Ünï Ж'
run 180 "$SETUP" /install /silent /nostart /dir "$DIR"; RC=$?
check "install into a folder with spaces / Ü / Cyrillic returns 0" "[ $RC -eq 0 ]"
UD="$C/Program Files/VM2Backup Ünï Ж"
check "files are in that folder" "[ -f '$UD/VM2Backup.exe' ] && [ -f '$UD/VM2BackupService.exe' ] && [ -f '$UD/VM2Backup-Setup.exe' ]"
check "the service is registered with the quoted path of that folder" "wine reg query 'HKLM\\System\\CurrentControlSet\\Services\\VM2BackupService' /v ImagePath 2>&1 | grep -a 'Program Files' | grep -aq 'VM2BackupService.exe. /service'"
run 120 'C:\Program Files\VM2Backup Ünï Ж\VM2BackupService.exe' /start >/dev/null 2>&1; sleep 3
check "the service starts from that folder" "[ \"\$(svc_state)\" = RUNNING ]"
run 120 "C:\\Program Files\\VM2Backup Ünï Ж\\VM2Backup-Setup.exe" /remove /silent; sleep 6
check "removal of that folder: files gone" "[ ! -e '$UD/VM2Backup.exe' ]"
check "removal of that folder: service gone" "! svc_registered"
check "removal of that folder: the folder itself is gone" "[ ! -d '$UD' ]"

echo "== bad /dir values =="
for BADDIR in 'relative\path' 'C:' 'C:\' '..\..\x' '\\\\server\\share\\vm2b' 'C:\Windows\System32\..\..\Temp\x' 'C:\a<b>c' 'CON' 'Z:\nonexistent-drive-root-ok\x'; do
    run 120 "$SETUP" /install /silent /nostart /dir "$BADDIR" >/dev/null 2>&1; RC=$?
    echo "   /dir '$BADDIR' -> rc $RC"
    check "bad /dir '$BADDIR' does not hang or crash (rc $RC)" "[ $RC -ne 124 ] && [ $RC -lt 128 ]"
    run 120 "$SETUP" /remove /silent >/dev/null 2>&1; sleep 3
done
rm -rf "$C/windows/system32/VM2Backup" 2>/dev/null
check "no program files leaked into Windows\\System32 or the drive root" "[ ! -e '$C/VM2Backup.exe' ] && [ ! -e '$C/windows/system32/VM2Backup.exe' ] && [ ! -e '$C/VM2Backup-Setup.exe' ]"
cleanup

echo "== install over a running service / update in place =="
run 180 "$SETUP" /install /silent; RC=$?
check "first install (with start) returns 0" "[ $RC -eq 0 ]"
sleep 3; check "the service runs after the install" "[ \"\$(svc_state)\" = RUNNING ]"
mkdir -p "$DATA"; echo keep-me > "$DATA/marker.txt"
cp "$PF/VM2Backup.exe" "$W/old.exe"; echo junk >> "$PF/VM2Backup.exe"          # make the installed copy differ from the package
run 180 "$SETUP" /install /silent; RC=$?
check "install OVER a running service returns 0" "[ $RC -eq 0 ]"
sleep 3
check "the program file was replaced by the package's" "cmp -s '$PF/VM2Backup.exe' '$B64/VM2Backup.exe'"
check "the service is running again after the update" "[ \"\$(svc_state)\" = RUNNING ]"
check "ProgramData survived the update" "[ \"\$(cat '$DATA/marker.txt' 2>/dev/null)\" = keep-me ]"
run 180 "$SETUP" /install /silent; RC=$?
check "a third identical install is harmless (rc 0)" "[ $RC -eq 0 ]"
sleep 3; check "...and the service still runs" "[ \"\$(svc_state)\" = RUNNING ]"
echo "== move to another folder (the old copy goes, one service) =="
run 180 "$SETUP" /install /silent /dir 'C:\Temp Dir\VM2B Moved'; RC=$?
check "install to another folder returns 0" "[ $RC -eq 0 ]"
sleep 3
check "new folder has the files" "[ -f '$C/Temp Dir/VM2B Moved/VM2Backup.exe' ]"
check "old folder files are gone" "[ ! -e '$PF/VM2Backup.exe' ] && [ ! -e '$PF/VM2BackupService.exe' ]"
check "the service now points at the new folder" "wine reg query 'HKLM\\System\\CurrentControlSet\\Services\\VM2BackupService' /v ImagePath 2>&1 | grep -qi 'VM2B Moved'"
check "ProgramData still there after the move" "[ \"\$(cat '$DATA/marker.txt' 2>/dev/null)\" = keep-me ]"
echo "== removal with the service running =="
sleep 1; check "the service runs before the removal" "[ \"\$(svc_state)\" = RUNNING ]"
run 120 'C:\Temp Dir\VM2B Moved\VM2Backup-Setup.exe' /remove /silent; RC=$?; sleep 6
check "removal returns 0" "[ $RC -eq 0 ]"
check "service unregistered after removal while it ran" "! svc_registered"
check "new folder emptied" "[ ! -e '$C/Temp Dir/VM2B Moved/VM2Backup.exe' ]"
check "ProgramData is kept by the removal" "[ \"\$(cat '$DATA/marker.txt' 2>/dev/null)\" = keep-me ]"
check "no service process left" "! pgrep -f 'VM2BackupService.exe' >/dev/null || [ -z \"\$(pgrep -f 'VM2BackupService.exe' | head -0)\" ]"
rm -rf "$DATA/marker.txt"

echo "== damaged Setup.exe =="
rm -rf "$C"/users/*/AppData/Local/Temp/VM2Backup-Setup-* 2>/dev/null
SZ=$(stat -c %s "$SETUP")
head -c $((SZ - 5000)) "$SETUP" > "$W/trunc.exe"
head -c $((SZ / 2)) "$SETUP" > "$W/half.exe"
python3 -I - "$SETUP" "$W" <<'PY'
import sys, struct
s = open(sys.argv[1], 'rb').read(); w = sys.argv[2]
open(w + '/badsize.exe', 'wb').write(s[:-8] + struct.pack('<Q', len(s) * 2))          # the size in the trailer is wrong (too big)
open(w + '/zerosize.exe', 'wb').write(s[:-8] + struct.pack('<Q', 0))
open(w + '/flip.exe', 'wb').write(s[:len(s) // 2] + bytes([s[len(s) // 2] ^ 0xff]) + s[len(s) // 2 + 1:])   # a byte inside the cabinet
open(w + '/nomagic.exe', 'wb').write(s[:-16] + b'XXXXXXXX' + s[-8:])
open(w + '/trailer-only.exe', 'wb').write(open(sys.argv[1], 'rb').read()[:60000] + b'VM2BPKG1' + struct.pack('<Q', 10 ** 12))
PY
for F in trunc half badsize zerosize flip nomagic trailer-only; do
    ( run 90 "$W/$F.exe" /install /silent /nostart >"$W/$F.out" 2>&1; echo $? > "$W/$F.rc" ) &
    BG=$!
    # a damaged package shows an error box; close it like a user would (click OK = close the window) so that the run can end
    for i in $(seq 1 40); do sleep 1; kill -0 $BG 2>/dev/null || break; done
    if kill -0 $BG 2>/dev/null; then echo "   $F: still running after 40 s (an error box waits?)"; fi
    wait $BG 2>/dev/null; RC=$(cat "$W/$F.rc" 2>/dev/null || echo 999)
    check "damaged package '$F': ends by itself with an error code, no crash, no box that waits (rc $RC)" "[ $RC -ge 1 ] && [ $RC -lt 124 ]"
    check "damaged package '$F': nothing installed" "[ ! -e '$PF/VM2Backup.exe' ]"
    check "damaged package '$F': temp extraction folder cleaned" "[ -z \"\$(ls -d '$C'/users/*/AppData/Local/Temp/VM2Backup-Setup-* 2>/dev/null)\" ]"
done
echo "package extra test: $FAILS failure(s)"
[ $FAILS -eq 0 ]
