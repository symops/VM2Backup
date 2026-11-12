#!/bin/bash
# Раунд 3, пункт B: копии vm2b-big (диски 6 ГиБ thin, 1 ГиБ lazy-zeroed, 1 ГиБ eager-zeroed; big-setup.sh) на настоящем ESXi; sha256 восстановленных дисков сверяется с эталоном хоста.
#   tools/esxi7/big-run.sh [cold-xz1 cold-pw-xz7 hot-xz1 cold-resume]      (по умолчанию все)
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi; S=$D/ssh.sh; RES=${RES:-$ROOT/.winvm-results/esxi7-round3-$(date +%Y%m%d)}; export RES; mkdir -p "$RES"
VM=vm2b-big; FL='"flats":["vm2b-big-flat.vmdk","vm2b-big_1-flat.vmdk","vm2b-big_2-flat.vmdk"]'
power() { $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $VM ' | cut -d' ' -f1); vim-cmd vmsvc/snapshot.removeall \$I >/dev/null 2>&1; S=\$(vim-cmd vmsvc/power.getstate \$I | tail -1); if [ '$1' = on ]; then [ \"\$S\" = 'Powered on' ] || vim-cmd vmsvc/power.on \$I >/dev/null; else [ \"\$S\" = 'Powered off' ] || vim-cmd vmsvc/power.off \$I >/dev/null; fi; rm -rf /vmfs/volumes/datastore1/VM2Backup-* /vmfs/volumes/datastore1/$VM/VM2Backup-*; sleep 3; vim-cmd vmsvc/power.getstate \$I | tail -1"; }
check() { # $1 тег: sha256 из вывода теста против эталона хоста
  python3 - "$RES/$1.txt" "$E/big-hashes.json" <<'PY'
import json, sys, re
ref = json.load(open(sys.argv[2])); ok = bad = 0
for l in open(sys.argv[1]):
    m = re.search(r'SHA (\S+) (\S*) (\d+)', l)
    if m:
        n, h, sz = m.groups()
        if n in ref and h == ref[n]['sha256'] and int(sz) == ref[n]['size']: ok += 1; print('  sha256 совпал:', n, sz)
        else: bad += 1; print('  НЕ СОВПАЛО:', n, sz, h[:12])
print('ИТОГ sha256: совпало %d, не совпало %d' % (ok, bad))
PY
}
hostcheck() { # $1 имя восстановленной ВМ на хосте
  $S "for f in /vmfs/volumes/datastore1/$1/*-flat.vmdk; do echo \"\$(basename \$f) \$(sha256sum \$f | cut -d' ' -f1) \$(stat -c %s \$f) du=\$(du -k \$f | cut -f1)\"; done" | tee "$RES/$1-host.txt" | python3 -c "
import json, sys
ref = json.load(open('$E/big-hashes.json')); ok = bad = 0
for l in sys.stdin:
    n, h, sz, du = l.split(); n = n.replace('$1', 'vm2b-big', 1)
    r = ref.get(n)
    if r and r['sha256'] == h and r['size'] == int(sz): ok += 1; print('  на хосте sha256 совпал:', n, sz, du)
    else: bad += 1; print('  НА ХОСТЕ НЕ СОВПАЛО:', n, sz, h[:12])
print('ИТОГ на хосте: совпало %d, не совпало %d' % (ok, bad))"
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $1 ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$I >/dev/null 2>&1; vim-cmd vmsvc/unregister \$I; rm -rf /vmfs/volumes/datastore1/$1; df -h /vmfs/volumes/datastore1 | tail -1" | tail -1
}
run() { echo "=== $1"; "$D/fs-run.sh" esxireal_big_disks "$1" "$2" | grep -E "FSRUN|FAIL|PASS|ошибка" | cut -c1-420; check "$1"; rm -rf "$E/fsout/$1" "$E/fsout/$1-state" "$E/fsout/store-$1"; df -h /home | tail -1; }
STEPS=${*:-cold-xz1 cold-pw-xz7 hot-xz1 cold-resume}
for st in $STEPS; do case $st in
  cold-xz1)  echo "ВМ: $(power off)"; run big-cold-xz1 "{\"vm\":\"$VM\",\"mode\":\"cold\",\"level\":1,$FL,\"restore_name\":\"vm2b-big-rt1\",\"restore_datastore\":\"datastore1\"}"; hostcheck vm2b-big-rt1 ;;
  cold-pw-xz7) echo "ВМ: $(power off)"; run big-cold-pw-xz7 "{\"vm\":\"$VM\",\"mode\":\"cold\",\"level\":7,\"password\":\"big-пароль-4GiB\",\"segment_mb\":100,$FL}" ;;
  hot-xz1)   echo "ВМ: $(power on)"; run big-hot-xz1 "{\"vm\":\"$VM\",\"mode\":\"hot\",\"quiesce\":false,\"level\":1,$FL,\"task_timeout\":1800}"; power off >/dev/null ;;
  cold-resume) echo "ВМ: $(power off)"; run big-cold-resume "{\"vm\":\"$VM\",\"mode\":\"cold\",\"level\":1,\"pause_mb\":4400,$FL}" ;;
esac; done
