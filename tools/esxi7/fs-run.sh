#!/bin/bash
# Раунд 3: один прогон теста esxireal_{guest_fs_copy,big_disks,multi_datastore} (src/tests_esxi_real.cpp, ключ "esxi_fs") против НАСТОЯЩЕГО ESXi.
#   tools/esxi7/fs-run.sh <имя теста> <тег> '<json параметров esxi_fs>'
#   пример: fs-run.sh esxireal_guest_fs_copy hot-q1 '{"vm":"vm2b-fs","mode":"hot","quiesce":true,"flats":["vm2b-fs-flat.vmdk","vm2b-fs_1-flat.vmdk"]}'
# VM2B_REAL_KEY=vcenter_real, VC_USER (по умолчанию administrator@vsphere.local) и VC_SECRET (файл пароля в .winvm/esxi) - те же прогоны от имени другого пользователя vCenter (пункт D).
# Результат: $RES/<тег>.txt (вывод теста), восстановленные диски в $E/fsout/<тег>/ (вне git); пароль хоста нигде не печатается.  VM2B_REAL_KEY=vcenter_real - то же через vCenter.
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi
EXE=${VM2B_EXE:-$ROOT/build/x64/vm2bautotest.exe}; RES=${RES:-$ROOT/.winvm-results/esxi7-round3-$(date +%Y%m%d)}; mkdir -p "$RES" "$E/fsout"
TEST=$1; TAG=$2; PARAMS=$3
FPV=$(echo | timeout 10 openssl s_client -connect 10.213.77.11:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 2>/dev/null | sed 's/.*=//; s/://g' | tr A-F a-f); export VC_FP="sha256:$FPV"
FP=$(echo | openssl s_client -connect 10.213.77.10:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr A-F a-f)
python3 - "$E" "$FP" "$TAG" "$PARAMS" <<'PY'
import json, sys, os
e, fp, tag, params = sys.argv[1:5]
w = lambda p: "Z:" + p.replace('/', '\\')
p = json.loads(params); p.setdefault("tag", tag); p["store"] = w(e + "/fsout/store-" + p.get("store_tag", tag)); p["out"] = w(e + "/fsout")
cfg = {"esxi_real": {"host": "10.213.77.10", "port": 443, "user": "root", "password_file": w(e + "/secret"), "fingerprint": "sha256:" + fp, "vms": [], "restore_prefix": "vm2b-rt"}, "esxi_fs": p}
if os.environ.get("VM2B_REAL_KEY") == "vcenter_real" and os.path.exists(e + "/secret-vc-sso"):
    cfg["vcenter_real"] = {"host": "10.213.77.11", "port": 443, "user": os.environ.get("VC_USER", "administrator@vsphere.local"), "password_file": w(e + "/" + os.environ.get("VC_SECRET", "secret-vc-sso")), "fingerprint": os.environ.get("VC_FP", ""), "datacenter": "vm2b-dc", "vms": [], "restore_prefix": "vm2b-vc-rt"}
json.dump(cfg, open(e + "/servers.json", "w")); os.chmod(e + "/servers.json", 0o600)
PY
export WINEDEBUG=-all
wine64 "$EXE" --servers "$E/servers.json" --only "$TEST" 2>&1 | grep -vE "^$" | cut -c1-3000 | tee "$RES/$TAG.txt"
