#!/bin/bash
# Прогон группы esxireal (src/tests_esxi_real.cpp) против НАСТОЯЩЕГО вложенного ESXi, затем побитовая сверка восстановленных на хост дисков.
#   tools/esxi7/run-real.sh [--exe путь/к/vm2bautotest.exe] [доп. аргументы vm2bautotest]
# Нужно: запущенный ESXi (tools/esxi7/vm.sh start), setup-guest.sh уже выполнен (.winvm/esxi/hashes.json), wine64, sshpass.
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi
EXE=${VM2B_EXE:-$ROOT/build/x64/vm2bautotest.exe}
if [ "$1" = "--exe" ]; then EXE=$2; shift 2; fi
RES=${RES:-$ROOT/.winvm-results/esxi7-$(date +%Y%m%d)}; mkdir -p "$RES"
FP=$(echo | openssl s_client -connect 10.213.77.10:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr A-F a-f)
FPV=$(echo | timeout 10 openssl s_client -connect 10.213.77.11:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 2>/dev/null | sed 's/.*=//; s/://g' | tr A-F a-f)
python3 - "$E" "$FP" "$FPV" <<'PY'
import json, sys
e, fp, fpv = sys.argv[1:4]
h = json.load(open(e + '/hashes.json'))
vms = [{"name": n, "on": n.endswith("-on"), "hist": n.endswith("-hist"), "flat": f} for n, f in h.items()]
cfg = {"esxi_real": {"host": "10.213.77.10", "port": 443, "user": "root", "password_file": "Z:" + e.replace('/', '\\') + "\\secret",
          "fingerprint": "sha256:" + fp, "vms": vms, "restore_prefix": "vm2b-rt"}}
if fpv:    # vCenter (VCSA под qemu, tools/esxi7/vcsa.sh + vc-setup.py): те же ВМ видны через него
    cfg["vcenter_real"] = {"host": "10.213.77.11", "port": 443, "user": "administrator@vsphere.local", "password_file": "Z:" + e.replace('/', '\\') + "\\secret-vc-sso",
          "fingerprint": "sha256:" + fpv, "datacenter": "vm2b-dc", "vms": vms, "restore_prefix": "vm2b-vc-rt"}
json.dump(cfg, open(e + '/servers.json', 'w'))
PY
chmod 600 "$E/servers.json"
# состояние до прогона: vm2b-on включена и без снимков, остатков VM2Backup-* на датасторе нет
if [ -z "$NOPREP" ]; then   # NOPREP=1: состояние ВМ между этапами сценария не трогать (live-scenario.sh)
"$D/ssh.sh" 'for id in $(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1); do vim-cmd vmsvc/snapshot.removeall $id >/dev/null 2>&1; [ "$(vim-cmd vmsvc/power.getstate $id | tail -1)" = "Powered on" ] || vim-cmd vmsvc/power.on $id >/dev/null 2>&1; done; sleep 3; rm -rf /vmfs/volumes/datastore1/VM2Backup-* /vmfs/volumes/datastore1/vm2b-*/VM2Backup-*; for id in $(vim-cmd vmsvc/getallvms | grep -E " vm2b-(vc-)?rt" | cut -d" " -f1); do vim-cmd vmsvc/power.off $id >/dev/null 2>&1; vim-cmd vmsvc/unregister $id >/dev/null 2>&1; done; rm -rf /vmfs/volumes/datastore1/vm2b-rt* /vmfs/volumes/datastore1/vm2b-vc-rt*' >/dev/null 2>&1 || true
# LIVE_HIST=1: ещё и vm2b-hist включается (тест esxireal_live_hist_running); без этого она остаётся выключенной
if [ -n "$LIVE_HIST" ]; then "$D/ssh.sh" 'id=$(vim-cmd vmsvc/getallvms | grep " vm2b-hist " | cut -d" " -f1); vim-cmd vmsvc/power.on $id >/dev/null 2>&1; sleep 4' >/dev/null 2>&1 || true; fi
fi
export WINEDEBUG=${WINEDEBUG:--all}
export VM2B_ESXI_TRACE=${VM2B_ESXI_TRACE:-}
wine64 "$EXE" --servers "$E/servers.json" --only esxireal "$@" 2>&1 | tee "$RES/autotest-esxireal.txt"
# после прогона: на хосте нет остатков клонов (папки VM2Backup-* внутри папок ВМ и в корне), у ВМ нет снимков VM2Backup-*, sha256 восстановленных на хост дисков
"$D/ssh.sh" 'sh -s' <<'CHK' | tee "$RES/host-after.txt"
DS=/vmfs/volumes/datastore1
echo "остатки VM2Backup-*: $(ls -d $DS/VM2Backup-* $DS/*/VM2Backup-* 2>/dev/null | wc -l)"
for id in $(vim-cmd vmsvc/getallvms | grep " vm2b-" | cut -d" " -f1); do n=$(vim-cmd vmsvc/snapshot.get $id | grep -c "Name *: VM2Backup-"); [ "$n" = 0 ] || echo "снимков VM2Backup-* у ВМ $id: $n"; done
for f in $DS/vm2b-rt*/*-flat.vmdk $DS/vm2b-vc-rt*/*-flat.vmdk; do [ -f "$f" ] && echo "RESTORED $(basename $(dirname $f))/$(basename $f) $(sha256sum $f | cut -d' ' -f1)"; done
CHK
python3 - "$E/hashes.json" "$RES/host-after.txt" <<'PY'
import json, sys
ref = {d: f for vm in json.load(open(sys.argv[1])).values() for f, d in vm.items()}
bad = ok = 0
for l in open(sys.argv[2]):
    if l.startswith('RESTORED '):
        _, n, d = l.split()
        if d in ref: ok += 1
        else: bad += 1; print('НЕ СОВПАЛО:', n)
print('восстановлено на хост: sha256 совпал у %d файлов, не совпал у %d' % (ok, bad))
PY
