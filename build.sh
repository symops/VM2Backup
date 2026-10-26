#!/bin/sh
# Builds the VM2Backup programs on Linux (clang + MinGW-w64).  ARCH=x64 (default) -> build/x64/   ARCH=x86 -> build/x86/
#   VM2Backup.exe           the program (a small control panel: service, web site, language)                                  [when src/main.cpp exists]
#   VM2BackupService.exe    the Windows service: scheduler, runs the tasks         [when src/service.cpp exists]
#   VM2Backup-Setup.exe     graphical installer                                    [when src/vm2bsetup.cpp exists]
#   vm2bautotest.exe        the randomized end-to-end test (console)
#   vm2blauncher.exe        (x86 only) the package's root VM2Backup-Setup.exe
# ONLY=autotest ./build.sh   builds only vm2bautotest.exe
set -e
cd "$(dirname "$0")/src"
ARCH=${ARCH:-x64}
if [ "$ARCH" = x86 ]; then TRIPLE=i686-w64-mingw32; TARGET=i686-w64-windows-gnu
else TRIPLE=x86_64-w64-mingw32; TARGET=x86_64-w64-windows-gnu; fi
O=${OUT:-../build/$ARCH}
mkdir -p "$O"
T=${TMPDIR:-/tmp}/vm2bapp.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
CXX="clang++ --target=$TARGET -std=c++17 -O2 -fno-exceptions -fno-rtti -municode
     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter
     -Wno-missing-field-initializers -isystem /usr/$TRIPLE/include"
# The engine: everything the service, the program and the test share (no window code).
CORE="base tar xz crypto cxxrt applog throttle"
# Third-party libraries built by tools/deps.sh into deps/out/<arch> (OpenSSL, ...): headers + the static link line, generically.
DEPDIR=$(cd .. && pwd)/deps/out/$ARCH
[ -f "$DEPDIR/openssl.flags" ] || { echo "OpenSSL for $ARCH is not built: run  tools/deps.sh build $ARCH" >&2; exit 1; }
DEPCF=$(../tools/deps-env.sh $ARCH cflags); DEPLIBS=$(../tools/deps-env.sh $ARCH libs)
[ -n "$DEPCF" ] && CXX="$CXX $(echo "$DEPCF" | sed "s|-I$DEPDIR/include|-isystem $DEPDIR/include|")"
# Optional parts, switched on by the presence of their source file (see docs/DEV.md).
[ -f target_ftp.cpp ] && CXX="$CXX -DVM2B_HAVE_FTP"
[ -f target_ssh.cpp ] && CXX="$CXX -DVM2B_HAVE_SSH"
[ -f esxi.cpp ] && CXX="$CXX -DVM2B_HAVE_ESXI"
# Optional core modules, compiled when they exist.
for f in winutil codec engine source_dir target_local target_smb target_ftp tls http target_ssh ssh sftp schedule config ipc webtls webauth webserver svccore esxi xml panelutil; do [ -f $f.cpp ] && CORE="$CORE $f"; done
ALL="$CORE"
GUI="lang ui client guitest main"
if [ "$ONLY" = autotest ]; then LATE="lang vm2bautotest"; elif [ "$ONLY" = service ]; then LATE="lang vm2bautotest service"; elif [ "$ONLY" = gui ]; then LATE="vm2bautotest service $GUI"; else LATE="vm2bautotest service $GUI vm2bsetup vm2blauncher"; fi
for f in $LATE; do [ -f $f.cpp ] && ALL="$ALL $f"; done
TESTOBJS=""
for f in tests_*.cpp; do [ -f "$f" ] && { ALL="$ALL ${f%.cpp}"; TESTOBJS="$TESTOBJS $T/${f%.cpp}.o"; }; done
export CXX T
printf '%s\n' $ALL | xargs -P 8 -I{} sh -c '$CXX -c {}.cpp -o "$T/{}.o"'
OBJS=""; for f in $CORE; do OBJS="$OBJS $T/$f.o"; done
NETLIBS="$DEPLIBS -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -lshell32 -luser32 -lole32"
if [ -f vm2bautotest.cpp ]; then
    $TRIPLE-gcc -municode -mconsole -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -Wl,-Map,"$O/vm2bautotest.map" -o "$O/vm2bautotest.exe" \
        "$T/vm2bautotest.o" $TESTOBJS $OBJS $( [ -f lang.cpp ] && echo "$T/lang.o" ) $NETLIBS
fi
if [ "$ONLY" = autotest ]; then echo "built $O/vm2bautotest.exe only"; exit 0; fi
if [ -f service.cpp ]; then
    $TRIPLE-gcc -municode -mconsole -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -Wl,-Map,"$O/VM2BackupService.map" -o "$O/VM2BackupService.exe" \
        "$T/service.o" $OBJS $( [ -f lang.cpp ] && echo "$T/lang.o" ) $NETLIBS
fi
if [ "$ONLY" = service ]; then echo "built $O (service)"; exit 0; fi
if [ -f main.cpp ]; then
    $TRIPLE-windres VM2Backup.rc -O coff -o "$T/app.res.o"
    GOBJS=""; for f in $GUI; do GOBJS="$GOBJS $T/$f.o"; done
    # the panel needs only the pipe client and the small helpers, not the engine / service / web / targets
    POBJS=""; for f in base cxxrt applog winutil ipc panelutil; do [ -f $f.cpp ] && POBJS="$POBJS $T/$f.o"; done
    $TRIPLE-gcc -municode -mwindows -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -Wl,-Map,"$O/VM2Backup.map" -o "$O/VM2Backup.exe" \
        $GOBJS $POBJS "$T/app.res.o" -lcomctl32 -lcomdlg32 -lgdi32 -lmsimg32 -ldwmapi -lshlwapi -luxtheme -ladvapi32 -lshell32 -luser32 -lole32
fi
if [ "$ONLY" = gui ]; then echo "built $O (GUI)"; exit 0; fi
if [ -f vm2bsetup.cpp ]; then
    $TRIPLE-windres VM2Backup-Setup.rc -O coff -o "$T/setup.res.o"
    $TRIPLE-gcc -municode -mwindows -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o "$O/VM2Backup-Setup.exe" \
        "$T/vm2bsetup.o" "$T/lang.o" "$T/applog.o" "$T/cxxrt.o" "$T/setup.res.o" \
        -lcomctl32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid
fi
if [ "$ARCH" = x86 ] && [ -f vm2blauncher.cpp ]; then
    $TRIPLE-windres VM2Backup-Setup.rc -O coff -o "$T/launcher.res.o"
    $TRIPLE-gcc -municode -mwindows -static -s -Wl,--gc-sections -Wl,--no-insert-timestamp -o "$O/vm2blauncher.exe" "$T/vm2blauncher.o" "$T/cxxrt.o" "$T/launcher.res.o" \
        -lsetupapi -lshell32 -luser32 -ladvapi32 -lole32
fi
echo "built $O"
