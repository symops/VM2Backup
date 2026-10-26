#!/bin/sh
# Runs the TLS / HTTP / FTP autotests of VM2Backup against local Python test servers (tools/testservers).
#   tools/run-ftp-tests.sh [--exe path/to/vm2bautotest.exe] [vm2bautotest arguments...]      default: --only ftp,tls,http
# Needs: the project's .venv (pyftpdlib, pyOpenSSL, cryptography), wine64.  Servers listen on 127.0.0.1, ports inside 21000-21999.
# They are stopped by PID only.  VM2B_BIG=1 makes the big-file test 2.2 GB (offset sanity above 2 GiB).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=$ROOT/build/x64/vm2bautotest.exe
if [ "$1" = "--exe" ]; then EXE=$2; shift 2; fi
PY=$ROOT/.venv/bin/python
[ -x "$PY" ] || PY=python3
RUN=$(mktemp -d /tmp/vm2b-ftp-run.XXXXXX)
PIDS=$RUN/pids
cleanup() { if [ -f "$PIDS" ]; then for p in $(cat "$PIDS"); do kill "$p" 2>/dev/null || true; done; fi; rm -rf "$RUN"; }
trap cleanup EXIT INT TERM
"$PY" "$ROOT/tools/testservers/launch.py" "$RUN" "$RUN/servers.json" "$PIDS" $VM2B_PORT_SEED
export WINEDEBUG=${WINEDEBUG:--all}
case " $* " in *" --only "*) ;; *) set -- --only ftp,tls,http "$@";; esac
wine64 "$EXE" --servers "$RUN/servers.json" "$@"
