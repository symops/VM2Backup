#!/bin/bash
# Прогон группы esxireal (src/tests_esxi_real.cpp) против НАСТОЯЩЕГО вложенного ESXi, затем побитовая сверка восстановленных на хост дисков.
#   tools/esxi7/run-real.sh [--exe путь/к/vm2bautotest.exe] [доп. аргументы vm2bautotest]
# Нужно: запущенный ESXi (tools/esxi7/vm.sh start), setup-guest.sh уже выполнен (.winvm/esxi/hashes.json), wine64, sshpass.
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi
EXE=$ROOT/build/x64/vm2bautotest.exe
if [ "$1" = "--exe" ]; then EXE=$2; shift 2; fi
RES=${RES:-$ROOT/.winvm-results/esxi7-$(date +%Y%m%d)}; mkdir -p "$RES"
FP=$(echo | openssl s_client -connect 10.213.77.10:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr A-F a-f)
python3 - "$E" "$FP" <<'PY'
import json, sys
e, fp = sys.argv[1:3]
h = json.load(open(e + '/hashes.json'))
vms = [{"name": n, "on": n.endswith("-on"), "hist": n.endswith("-hist"), "flat": f} for n, f in h.items()]
json.dump({"esxi_real": {"host": "10.213.77.10", "port": 443, "user": "root", "password_file": "Z:" + e.replace('/', '\\') + "\\secret",
          "fingerprint": "sha256:" + fp, "vms": vms, "restore_prefix": "vm2b-rt"}}, open(e + '/servers.json', 'w'))
PY
chmod 600 "$E/servers.json"
export WINEDEBUG=${WINEDEBUG:--all}
export VM2B_ESXI_TRACE=${VM2B_ESXI_TRACE:-}
wine64 "$EXE" --servers "$E/servers.json" --only esxireal "$@" 2>&1 | tee "$RES/autotest-esxireal.txt"
