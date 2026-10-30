#!/bin/bash
# Checks the finished package under Wine: dist/VM2Backup-Setup.exe (the launcher with the cabinet appended) and the
# unpacked dist/VM2Backup/ folder install the right files, clean up their temporary folder and remove cleanly.
#   ./package.sh && tools/test-package-wine.sh
# Own Wine prefix (WINEPREFIX_PKG), own Xvfb display when DISPLAY is not set; prints PASS / FAIL, exit 1 on a failure.
cd "$(dirname "$0")/.."
ROOT=$PWD; B64=${BUILD_X64:-$ROOT/build/x64}; B86=${BUILD_X86:-$ROOT/build/x86}; DIST=${DIST_DIR:-$ROOT/dist}      # BUILD_X64 / BUILD_X86 / DIST_DIR: another build + package folder
W=${PKG_TEST_DIR:-${TMPDIR:-/tmp}/vm2b-pkgtest}
mkdir -p "$W"
export WINEPREFIX=${WINEPREFIX_PKG:-$W/prefix} WINEDEBUG=-all
LOW="nice -n 19 ionice -c3"
if [ -z "$DISPLAY" ]; then
    export DISPLAY=:79
    [ -S /tmp/.X11-unix/X79 ] || { nice -n 19 Xvfb :79 -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 3; }
fi
FAILS=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; FAILS=$((FAILS + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
# same <файл в Program Files> <файл сборки>: одинаковы байт в байт, либо отличаются только подписью Authenticode (VM2B_SIGN_PFX подписывает exe пакета)
same() { cmp -s "$1" "$2" && return 0; osslsigncode remove-signature -in "$1" -out "$W/unsigned.tmp" >/dev/null 2>&1 && cmp -s "$W/unsigned.tmp" "$2"; }

SETUP="$DIST/VM2Backup-Setup.exe"
UNPACKED="$DIST/VM2Backup/VM2Backup-Setup.exe"
[ -f "$SETUP" ] || { echo "FAIL  $SETUP does not exist: run ./package.sh first"; exit 1; }
PF="$WINEPREFIX/drive_c/Program Files/VM2Backup"
TEMPDIR="$WINEPREFIX/drive_c/users/$USER/AppData/Local/Temp"
[ -d "$TEMPDIR" ] || TEMPDIR=$(ls -d "$WINEPREFIX"/drive_c/users/*/AppData/Local/Temp 2>/dev/null | head -1)
rm -rf "$PF"

# the package is one file: the launcher + a cabinet + the trailer
# (подписанный файл: сертификат лежит ПОСЛЕ хвоста, поэтому хвост проверяем у копии без подписи)
osslsigncode remove-signature -in "$SETUP" -out "$W/setup-nosig.exe" >/dev/null 2>&1 || cp "$SETUP" "$W/setup-nosig.exe"
check "the single file ends with the VM2BPKG1 trailer (before the signature, if signed)" "tail -c 16 '$W/setup-nosig.exe' | head -c 8 | grep -q VM2BPKG1"
check "the single file is much bigger than the launcher (the payload is in it)" "[ \$(stat -c %s '$SETUP') -gt \$(( \$(stat -c %s '$B86/vm2blauncher.exe') + 200000 )) ]"

for MODE in single unpacked; do
    EXE=$SETUP; [ $MODE = unpacked ] && EXE=$UNPACKED
    $LOW timeout 180 wine "$EXE" /install /silent /autostart; RC=$?
    check "[$MODE] silent install returns 0" "[ $RC -eq 0 ]"
    TEMPDIR=$(ls -d "$WINEPREFIX"/drive_c/users/*/AppData/Local/Temp 2>/dev/null | head -1)      # the prefix exists only after the first wine run (round 8: the "temporary folder is gone" checks were vacuous with an empty TEMPDIR)
    # Wine runs the 64-bit installer (the machine is 64-bit): the program must be the x64 build
    check "[$MODE] VM2Backup.exe is in Program Files and is the x64 build" "same '$PF/VM2Backup.exe' '$B64/VM2Backup.exe'"
    check "[$MODE] VM2BackupService.exe is in Program Files and is the x64 build" "same '$PF/VM2BackupService.exe' '$B64/VM2BackupService.exe'"
    check "[$MODE] the Windows service is registered" "wine sc query VM2BackupService 2>&1 | grep -q 'SERVICE_NAME'"
    check "[$MODE] the installer copy for removal is there" "same '$PF/VM2Backup-Setup.exe' '$B64/VM2Backup-Setup.exe'"
    check "[$MODE] uninstall.cmd is there" "[ -f '$PF/uninstall.cmd' ]"
    check "[$MODE] the temporary extraction folder is gone" "[ -z \"\$(ls -d '$TEMPDIR'/VM2Backup-Setup-* 2>/dev/null)\" ]"
    $LOW timeout 120 wine "C:\\Program Files\\VM2Backup\\VM2Backup-Setup.exe" /remove /silent; RC=$?
    sleep 6
    check "[$MODE] silent removal returns 0" "[ $RC -eq 0 ]"
    check "[$MODE] the program folder is gone after the removal" "[ ! -e '$PF/VM2Backup.exe' ]"
    check "[$MODE] the service is gone after the removal" "! wine sc query VM2BackupService 2>&1 | grep -q 'SERVICE_NAME'"
done
# the 32-bit installer on its own (what a 32-bit Windows would run)
$LOW timeout 180 wine "$DIST/VM2Backup/x86/VM2Backup-Setup.exe" /install /silent; RC=$?
check "[x86] the 32-bit installer installs (return 0)" "[ $RC -eq 0 ]"
PF86="$WINEPREFIX/drive_c/Program Files/VM2Backup"; [ -d "$WINEPREFIX/drive_c/Program Files (x86)/VM2Backup" ] && PF86="$WINEPREFIX/drive_c/Program Files (x86)/VM2Backup"
check "[x86] the 32-bit program is installed" "same '$PF86/VM2Backup.exe' '$B86/VM2Backup.exe'"
$LOW timeout 120 wine "$DIST/VM2Backup/x86/VM2Backup-Setup.exe" /remove /silent; sleep 6
check "[x86] and removed again" "[ ! -e '$PF86/VM2Backup.exe' ]"
# ПОДПИСАННЫЙ одиночный файл (tools/sign.sh, тестовый сертификат): загрузчик находит хвост перед таблицей сертификатов и ставит пакет как обычно
if command -v osslsigncode >/dev/null; then
    PKI=$W/pki; tools/make-test-cert.sh "$PKI" >/dev/null 2>&1
    cp "$SETUP" "$W/signed-setup.exe"
    tools/sign.sh --pfx "$PKI/test-sign.pfx" --password-file "$PKI/test-sign.pass" --name VM2Backup --verify --ca "$PKI/test-root.pem" "$W/signed-setup.exe" > "$W/sign.log" 2>&1
    check "[signed] the single file is signed and the signature verifies against the test root" "grep -q 'Signature verification: ok' '$W/sign.log'"
    check "[signed] the file is bigger than the unsigned one (certificate table appended)" "[ \$(stat -c %s '$W/signed-setup.exe') -gt \$(stat -c %s '$W/setup-nosig.exe') ]"
    check "[signed] the payload trailer is NOT at the end of the file any more" "! tail -c 16 '$W/signed-setup.exe' | head -c 8 | grep -q VM2BPKG1"
    $LOW timeout 180 wine "$W/signed-setup.exe" /install /silent /autostart; RC=$?
    check "[signed] the signed single file installs (return 0)" "[ $RC -eq 0 ]"
    check "[signed] VM2Backup.exe is in Program Files" "same '$PF/VM2Backup.exe' '$B64/VM2Backup.exe'"
    check "[signed] the temporary extraction folder is gone" "[ -z \"\$(ls -d '$TEMPDIR'/VM2Backup-Setup-* 2>/dev/null)\" ]"
    $LOW timeout 120 wine "C:\\Program Files\\VM2Backup\\VM2Backup-Setup.exe" /remove /silent; sleep 6
    check "[signed] removal after the signed install" "[ ! -e '$PF/VM2Backup.exe' ]"
    # а файл, изменённый ПОСЛЕ подписи, подпись не проходит (но запускается: проверку подписи делает Windows, не загрузчик)
    python3 - "$W/signed-setup.exe" "$W/tampered-setup.exe" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); i = d.find('VM2Backup setup'.encode('utf-16le')); d[i] = ord('X')
open(sys.argv[2], 'wb').write(d)
PY
    check "[signed] a file changed after signing fails the signature check" "osslsigncode verify -in '$W/tampered-setup.exe' -CAfile '$PKI/test-root.pem' 2>&1 | grep -q 'MISMATCH'"
fi
# РАУНД 8: пакет с повреждённым байтом ВНУТРИ cab (изменён после сборки / скачан не до конца): тихая установка завершается кодом != 0, причина записана в %TEMP%\VM2Backup-Setup.log
# (без окна: раньше код 1 был единственным следом), временная папка убрана, ничего не установлено
python3 - "$SETUP" "$W/damaged-setup.exe" <<'PY2'
import sys
d = bytearray(open(sys.argv[1], 'rb').read()); c = d.find(b'MSCF'); assert c > 0
size = int.from_bytes(d[c + 8:c + 12], 'little'); d[c + size // 2] ^= 0xFF
open(sys.argv[2], 'wb').write(d)
PY2
rm -f "$TEMPDIR/VM2Backup-Setup.log"
$LOW timeout 120 wine "$W/damaged-setup.exe" /install /silent; RC=$?
check "[damaged cabinet] silent install fails (return $RC, not 0)" "[ $RC -ne 0 ]"
check "[damaged cabinet] the reason is in VM2Backup-Setup.log: damaged or incomplete package" "grep -q 'damaged or incomplete' '$TEMPDIR/VM2Backup-Setup.log'"
check "[damaged cabinet] the temporary extraction folder is gone" "[ -z \"\$(ls -d '$TEMPDIR'/VM2Backup-Setup-* 2>/dev/null)\" ]"
check "[damaged cabinet] nothing was installed" "[ ! -e '$PF/VM2Backup.exe' ]"
echo "package test: $FAILS failure(s)"
[ $FAILS -eq 0 ]
