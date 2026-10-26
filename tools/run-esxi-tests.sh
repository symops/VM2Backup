#!/bin/sh
# Runs the ESXi / vCenter autotests of VM2Backup against the local emulators (tools/testservers/esxi_mock.py), one per version profile
# 6.0 .. 9.0 as host and as vCenter.   tools/run-esxi-tests.sh [--exe path/to/vm2bautotest.exe] [vm2bautotest arguments...]      default: --only esxi,svcesxi
# Needs python3 (+ cryptography) and wine64.  The emulators listen on 127.0.0.1, ports inside 23000-23999, and are stopped by PID only.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=$ROOT/build/x64/vm2bautotest.exe
if [ "$1" = "--exe" ]; then EXE=$2; shift 2; fi
PY=python3
RUN=$(mktemp -d /tmp/vm2b-esxi-run.XXXXXX)
PIDS=$RUN/pids
cleanup() { if [ -f "$PIDS" ]; then for p in $(cat "$PIDS"); do kill "$p" 2>/dev/null || true; done; fi; if [ -n "$VM2B_KEEP_MOCK_LOGS" ]; then mkdir -p "$VM2B_KEEP_MOCK_LOGS"; for d in "$RUN"/*/; do [ -f "$d/mock.log" ] && cp "$d/mock.log" "$VM2B_KEEP_MOCK_LOGS/$(basename "$d").log"; done; fi; rm -rf "$RUN"; }
trap cleanup EXIT INT TERM
"$PY" -I "$ROOT/tools/testservers/esxi_launch.py" "$RUN" "$RUN/servers.json" "$PIDS" $VM2B_PORT_SEED
export WINEDEBUG=${WINEDEBUG:--all}
case " $* " in *" --only "*) ;; *) set -- --only esxi,svcesxi "$@";; esac
wine64 "$EXE" --servers "$RUN/servers.json" "$@"
