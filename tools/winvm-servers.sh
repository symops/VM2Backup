#!/bin/bash
# Test servers on THIS host for the Windows VM (docs/WINDOWS-TESTS.md):  winvm-servers.sh start <ftp|ssh|esxi> | stop-all
#   Servers listen on the Docker bridge address (VM2B_BIND_ADDR, default 172.17.0.1) and the files they serve live in .winvm/shared/srv/<name>/, which the guest
#   reaches as a UNC path (VM2B_GUEST_MAP); servers.json is written there with the addresses and paths AS THE GUEST SEES THEM.  PIDs: .winvm/shared/srv/pids-<name> (killed by PID only).
#   Environment: VM2B_GUEST_SHARE (default \\10.0.2.4\qemu), VM2B_SEED.
R=$(cd "$(dirname "$0")/.." && pwd); W=$R/.winvm; S=$W/shared/srv
export VM2B_BIND_ADDR=${VM2B_BIND_ADDR:-172.17.0.1}; export VM2B_SERVER_HOST=${VM2B_SERVER_HOST:-$VM2B_BIND_ADDR}
export VM2B_SERVER_NAME=${VM2B_SERVER_NAME:-vm2b-host.test}      # certificates are made for this name; the guest's hosts file maps it to the bridge address (run-winvm.sh does that)
GS=${VM2B_GUEST_SHARE:-'\\10.0.2.4\qemu'}
export VM2B_GUEST_MAP="$W/shared=$GS"
PY=$R/.venv/bin/python; [ -x "$PY" ] || PY=python3
mkdir -p "$S"
case "$1" in
  start)
    N=$2; rm -rf "$S/$N"; mkdir -p "$S/$N"
    case "$N" in
      ftp)  "$PY" "$R/tools/testservers/launch.py" "$S/ftp" "$S/ftp/servers.json" "$S/pids-ftp" ${VM2B_SEED:-} || exit 1 ;;
      esxi) "$PY" -I "$R/tools/testservers/esxi_launch.py" "$S/esxi" "$S/esxi/servers.json" "$S/pids-esxi" ${VM2B_SEED:-} || exit 1 ;;
      ssh)  rm -rf "$S/ssh"; ( WORK="$S/ssh" VM2B_SERVERS_ONLY=1 SSH_TEST_USER=vm2bwin PATH="$R/.venv/bin:$PATH" setsid "$R/tools/run-ssh-tests.sh" > "$S/ssh.log" 2>&1 & echo $! >> "$S/pids-ssh-runner" )
            for i in $(seq 1 200); do [ -f "$S/ssh/ready" ] && break; sleep 0.5; done
            [ -f "$S/ssh/ready" ] || { echo "ssh servers did not start, see $S/ssh.log"; exit 1; } ;;
      *) echo "unknown set $N"; exit 2;;
    esac
    echo "servers $N: $S/$N/servers.json" ;;
  stop-all)
    for f in "$S"/pids-*; do [ -f "$f" ] && for p in $(cat "$f"); do kill "$p" 2>/dev/null; done; done
    sleep 2
    for f in "$S"/pids-*; do [ -f "$f" ] && for p in $(cat "$f"); do kill -9 "$p" 2>/dev/null; done; rm -f "$f"; done
    echo "servers stopped" ;;
  *) echo "usage: winvm-servers.sh start ftp|ssh|esxi | stop-all"; exit 2;;
esac
