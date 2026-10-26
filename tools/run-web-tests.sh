#!/bin/sh
# Runs the administration web site tests of VM2Backup: the in-process "web" group, then the Python clients over real HTTPS
# against the service hosted by the Wine test process.  Needs wine64, python3 (stdlib only), openssl (optional), curl (optional).
#   tools/run-web-tests.sh [--exe path/to/vm2bautotest.exe] [--quick] [--no-python]
# svcweb = the same site hosted by an in-process ServiceCore (tests_svc_esxi.cpp; it picks its ports from 24000-24299).
# Port range 24000-24999.  Wine is stopped by PID only.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE=$ROOT/build/x64/vm2bautotest.exe
QUICK=""; PYTHON=1
while [ $# -gt 0 ]; do case "$1" in --exe) EXE=$2; shift 2;; --quick) QUICK=--quick; shift;; --no-python) PYTHON=0; shift;; *) echo "unknown $1"; exit 2;; esac; done
export WINEDEBUG=${WINEDEBUG:--all}
VM2B_SRC_DIR=Z:$(echo "$ROOT/src" | sed 's|/|\\|g'); export VM2B_SRC_DIR
echo "== web UI logic under node (fake DOM) =="
if command -v node >/dev/null 2>&1; then node "$ROOT/tools/testservers/web_ui_tests.js"; else echo "(node missing: skipped)"; fi
echo "== in-process tests (group web) =="
wine64 "$EXE" --only totp_rfc6238_vectors,policy_roles_bind_entries,auth_users_login_throttle_sessions,default_admin_created_once,auth_totp_recovery_2fa_rules,tls_certificate_and_server_basics,interfaces_choose_addresses_live,role_matrix_classifies_every_service_command,svcweb
[ "$PYTHON" = 1 ] || exit 0
RUN=$(mktemp -d /tmp/vm2b-web-run.XXXXXX)
mkdir -p "$RUN/ctl" "$RUN/data"
PORT=${VM2B_WEB_PORT:-$((24600 + $$ % 300))}
WPATH() { printf 'Z:%s' "$(echo "$1" | sed 's|/|\\\\|g')"; }
printf '{"web":{"ctl_dir":"%s","data_dir":"%s","kdf":2000}}' "$(WPATH $RUN/ctl)" "$(WPATH $RUN/data)" > "$RUN/servers.json"
cleanup() { [ -n "$WPID" ] && kill "$WPID" 2>/dev/null || true; rm -rf "$RUN"; }
trap cleanup EXIT INT TERM
echo "== Python clients over HTTPS, port $PORT =="
wine64 "$EXE" --servers "$RUN/servers.json" --only host_for_python_clients > "$RUN/wine.log" 2>&1 &
WPID=$!
i=0; while [ ! -f "$RUN/ctl/ready.txt" ] && [ $i -lt 600 ]; do sleep 0.2; i=$((i+1)); done
[ -f "$RUN/ctl/ready.txt" ] || { echo "the Wine host did not start"; cat "$RUN/wine.log"; exit 1; }
set +e
python3 -I "$ROOT/tools/testservers/web_client_tests.py" "$RUN/ctl" "$RUN/data" "$PORT" $QUICK ${VM2B_WEB_ONLY:+--only=$VM2B_WEB_ONLY}
RC=$?
i=0; while kill -0 "$WPID" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.2; i=$((i+1)); done
if [ -n "$VM2B_WEB_DEBUG" ]; then cat "$RUN/wine.log"; else tail -5 "$RUN/wine.log"; fi
echo "== done: python exit code $RC =="
exit $RC
