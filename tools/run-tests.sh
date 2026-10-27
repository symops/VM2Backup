#!/bin/bash
# Runs every test suite of VM2Backup one after another and prints one summary.  Everything runs under Wine (the Windows code) or
# natively (the pure modules with ASan/UBSan); no suite needs a real Windows machine, none touches the network outside 127.0.0.1.
#   tools/run-tests.sh [--seed N] [--skip a,b] [--only a,b] [--build]
# (t_tar is an interop helper for Python, not a self-test; tar is covered by core tar_roundtrip)
# Suites: native core service ftp ssh esxi web gui package package2     (core = the default groups of vm2bautotest.exe)
# Environment: LOGDIR (default $TMPDIR/vm2b-tests-<time>), TMPDIR; the suites use ports 22000-24999 and the Xvfb display :95 (GUI_DISPLAY).
cd "$(dirname "$0")/.."
ROOT=$PWD
SEED=; SKIP=,; ONLY=; BUILD=0
while [ $# -gt 0 ]; do case "$1" in
    --seed) SEED="--seed $2"; shift 2;; --skip) SKIP=",$2,"; shift 2;; --only) ONLY=",$2,"; shift 2;; --build) BUILD=1; shift;;
    *) echo "unknown argument $1"; exit 2;; esac; done
export TMPDIR=${TMPDIR:-/tmp}
LOGDIR=${LOGDIR:-$TMPDIR/vm2b-tests-$(date +%H%M%S)}
mkdir -p "$LOGDIR"
export WINEDEBUG=-all
if [ $BUILD = 1 ]; then ./build.sh > "$LOGDIR/build.log" 2>&1 || { echo "BUILD FAILED, see $LOGDIR/build.log"; exit 1; }; fi
[ -f build/x64/vm2bautotest.exe ] || { echo "build first: ./build.sh"; exit 2; }
RES=()
want() { [ -n "$ONLY" ] && { [[ "$ONLY" == *",$1,"* ]] || return 1; }; [[ "$SKIP" != *",$1,"* ]]; }
run() {   # run <name> <command...>
    local n=$1; shift
    want $n || return 0
    local t0=$SECONDS
    echo "== $n"
    "$@" > "$LOGDIR/$n.log" 2>&1; local rc=$?
    local sum=$(grep -E '^[0-9]+ passed' "$LOGDIR/$n.log" | tail -1)
    RES+=("$(printf '%-8s %-5s %4ds  %s' $n $([ $rc = 0 ] && echo ok || echo FAIL) $((SECONDS - t0)) "$sum")")
    [ $rc = 0 ] || { echo "   FAILED (rc=$rc); last lines:"; tail -5 "$LOGDIR/$n.log" | sed 's/^/   | /'; }
}
native() {
    local N=$LOGDIR/native; mkdir -p $N; local ok=0
    g() { g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -pthread "$@"; }
    g tests/native/t_base.cpp src/base.cpp -o $N/t_base && $N/t_base || ok=1
    g tests/native/t_schedule.cpp src/schedule.cpp src/base.cpp -o $N/t_schedule && $N/t_schedule || ok=1
    g tests/native/t_xz.cpp src/xz.cpp src/base.cpp -llzma -o $N/t_xz && $N/t_xz self || ok=1
    return $ok
}
run native native
run core    env WINEPREFIX=$LOGDIR/prefix-core wine64 build/x64/vm2bautotest.exe $SEED
run core32  env WINEPREFIX=$LOGDIR/prefix-core wine64 build/x86/vm2bautotest.exe $SEED --only core,engine,resume,restore,xz
run ftp     env WINEPREFIX=$LOGDIR/prefix-ftp tools/run-ftp-tests.sh $SEED
run ssh     env WINEPREFIX=$LOGDIR/prefix-ssh tools/run-ssh-tests.sh $SEED
run esxi    env WINEPREFIX=$LOGDIR/prefix-esxi tools/run-esxi-tests.sh $SEED
run web     env WINEPREFIX=$LOGDIR/prefix-web tools/run-web-tests.sh
run gui     env WINEPREFIX_GUI=$LOGDIR/prefix-gui GUI_TEST_DIR=$LOGDIR/gui tools/run-gui-tests.sh $SEED
run package env PKG_TEST_DIR=$LOGDIR/pkg DISPLAY=${GUI_DISPLAY:-:95} tools/test-package-wine.sh
run package2 env PKG_TEST_DIR=$LOGDIR/pkg2 PKG_DISPLAY=${GUI_DISPLAY:-:95} WINEPREFIX_PKG2=$LOGDIR/prefix-pkg2 tools/test-package-extra.sh
echo; echo "logs: $LOGDIR"; printf '%s\n' "${RES[@]}"
printf '%s\n' "${RES[@]}" | grep -q ' FAIL ' && exit 1; exit 0
