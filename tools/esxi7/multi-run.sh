#!/bin/bash
# Раунд 3, пункт C: восстановление ВМ vm2b-ms (диски на двух датасторах; multi-setup.sh) на выбранный датастор / «как было», esxi.restore_options, нехватка места, занятое имя.
#   tools/esxi7/multi-run.sh [options chosen asis full busy]
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi; S=$D/ssh.sh; RES=${RES:-$ROOT/.winvm-results/esxi7-round3-$(date +%Y%m%d)}; export RES; mkdir -p "$RES"
VM=vm2b-ms; FL='"flats":["vm2b-ms-flat.vmdk","vm2b-ms_1-flat.vmdk"]'
where() { $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $1 ' | cut -d' ' -f1); echo \"vmx: \$(vim-cmd vmsvc/get.config \$I 2>/dev/null | grep -m1 vmPathName | cut -d'\"' -f2)\"; for d in datastore1 datastore2; do echo \"\$d: \$(ls -m /vmfs/volumes/\$d/$1 2>/dev/null)\"; done; for f in /vmfs/volumes/datastore1/$1/*flat.vmdk /vmfs/volumes/datastore2/$1/*flat.vmdk; do [ -f \$f ] && echo \"SHA \$(basename \$f | sed 's/$1/$VM/') \$(sha256sum \$f | cut -d' ' -f1)\"; done" | tee "$RES/$1-host.txt"
  python3 - "$RES/$1-host.txt" "$E/ms-hashes.json" <<'PY'
import json, sys
ref = json.load(open(sys.argv[2])); ok = bad = 0
for l in open(sys.argv[1]):
    if l.startswith('SHA '):
        _, n, h = l.split(); r = ref.get(n)
        if r and r['sha256'] == h: ok += 1
        else: bad += 1; print('НЕ СОВПАЛО', n)
print('ИТОГ на хосте sha256: совпало %d, не совпало %d' % (ok, bad))
PY
}
drop() { $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $1 ' | cut -d' ' -f1); [ -n \"\$I\" ] && { vim-cmd vmsvc/power.off \$I >/dev/null 2>&1; vim-cmd vmsvc/unregister \$I; }; rm -rf /vmfs/volumes/datastore1/$1 /vmfs/volumes/datastore2/$1; ls /vmfs/volumes/datastore1 /vmfs/volumes/datastore2 | grep -c rt" | tail -1; }
run() { echo "=== $1"; "$D/fs-run.sh" esxireal_multi_datastore "$1" "$2" | grep -E "FSRUN|OPTIONS|FAIL|PASS|ошибка" | cut -c1-900; rm -rf "$E/fsout/$1" "$E/fsout/$1-state" "$E/fsout/store-$1"; }
STEPS=${*:-options chosen asis full busy}
for st in $STEPS; do case $st in
  options) run ms-options "{\"vm\":\"$VM\",\"mode\":\"cold\",$FL,\"options\":true}" ;;
  chosen)  run ms-chosen "{\"vm\":\"$VM\",\"mode\":\"cold\",$FL,\"restore_name\":\"vm2b-ms-rtA\",\"restore_datastore\":\"datastore2\"}"; where vm2b-ms-rtA ;;
  asis)    run ms-asis "{\"vm\":\"$VM\",\"mode\":\"cold\",$FL,\"restore_name\":\"vm2b-ms-rtB\"}"; where vm2b-ms-rtB ;;
  full)    $S 'F=$(df -k /vmfs/volumes/datastore2 | tail -1 | awk "{print int(\$4/1024)-60}"); vmkfstools -c ${F}M -d zeroedthick /vmfs/volumes/datastore2/filler.vmdk >/dev/null; df -h /vmfs/volumes/datastore2 | tail -1' ; run ms-full "{\"vm\":\"$VM\",\"mode\":\"cold\",$FL,\"restore_name\":\"vm2b-ms-rtC\",\"restore_datastore\":\"datastore2\",\"expect_error\":\"space\"}"; echo "после отказа: $(drop vm2b-ms-rtC) остатков"; $S 'rm -f /vmfs/volumes/datastore2/filler*.vmdk; ls /vmfs/volumes/datastore2; df -h /vmfs/volumes/datastore2 | tail -1' ;;
  busy)    run ms-busy "{\"vm\":\"$VM\",\"mode\":\"cold\",$FL,\"restore_name\":\"vm2b-ms-rtA\",\"restore_datastore\":\"datastore2\",\"expect_error\":\"exists\"}"; where vm2b-ms-rtA; drop vm2b-ms-rtA; drop vm2b-ms-rtB ;;
esac; done
