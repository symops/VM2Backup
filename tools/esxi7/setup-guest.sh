#!/bin/bash
# Внутри вложенного ESXi: VMFS-датастор на втором диске и тестовые ВМ БЕЗ ОС (vmx + vmdk) с известным содержимым.
#   vm2b-off  выключенная: диск 1 thin 512 МБ (≈100 МБ случайных данных кусками, остальное дыры), диск 2 zeroedthick 64 МБ целиком случайный
#   vm2b-on   включённая:  диск thin 256 МБ (≈50 МБ случайных данных); процесс ВМ крутит BIOS и пишет только в дельту снимка
# Результат: .winvm/esxi/hashes.json  {"vm2b-off": {"vm2b-off-flat.vmdk": "<sha256>", ...}, ...}  (sha256sum на самом хосте, ДО бэкапа)
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; ROOT=$(cd "$D/../.." && pwd)
OUT=$($S 'sh -s' <<'GUEST'
set -e
DS=/vmfs/volumes/datastore1
if [ ! -d "$DS" ]; then
  for d in $(ls /vmfs/devices/disks/ | grep '^t10\.' | grep -v ':'); do
    # второй диск = тот, у которого нет разделов
    ls /vmfs/devices/disks/ | grep -q "^$d:1\$" || DISK=$d
  done
  [ -n "$DISK" ] || { echo "нет пустого диска"; exit 1; }
  END=$(partedUtil getptbl /vmfs/devices/disks/$DISK | sed -n 2p | awk '{print $1*$2*$3-1}')
  partedUtil mklabel /vmfs/devices/disks/$DISK gpt
  partedUtil setptbl /vmfs/devices/disks/$DISK gpt "1 2048 $END AA31E02A400F11DB9590000C2911D1B8 0"
  vmkfstools -C vmfs6 -S datastore1 /vmfs/devices/disks/$DISK:1
  sleep 3
fi
ls -la /vmfs/volumes/ | head
# pciBridge*: без них pvscsi во вложенном ESXi не получает слот PCIe («No PCIe slot available for SCSI0»), ВМ не включается
make_vm() { # $1 имя, $2 guestOS
  V=$DS/$1; mkdir -p $V
  cat > $V/$1.vmx <<VMX
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "$1"
guestOS = "other-64"
memSize = "256"
numvcpus = "1"
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "$1.vmdk"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
pciBridge5.present = "TRUE"
pciBridge5.virtualDev = "pcieRootPort"
pciBridge5.functions = "8"
pciBridge6.present = "TRUE"
pciBridge6.virtualDev = "pcieRootPort"
pciBridge6.functions = "8"
pciBridge7.present = "TRUE"
pciBridge7.virtualDev = "pcieRootPort"
pciBridge7.functions = "8"
$3
firmware = "bios"
vmx.scoreboard.enabled = "FALSE"
VMX
}
# повторный запуск: старые тестовые ВМ выключаются, снимаются с учёта и удаляются
for id in $(vim-cmd vmsvc/getallvms | grep " vm2b-" | cut -d" " -f1); do vim-cmd vmsvc/power.off $id >/dev/null 2>&1 || true; vim-cmd vmsvc/unregister $id >/dev/null 2>&1 || true; done
rm -rf $DS/vm2b-off $DS/vm2b-on $DS/VM2Backup-tmp1
make_vm vm2b-off x "scsi0:1.present = \"TRUE\"
scsi0:1.fileName = \"vm2b-off_1.vmdk\""
vmkfstools -c 512M -d thin $DS/vm2b-off/vm2b-off.vmdk
vmkfstools -c 64M -d zeroedthick $DS/vm2b-off/vm2b-off_1.vmdk
make_vm vm2b-on x ""
vmkfstools -c 256M -d thin $DS/vm2b-on/vm2b-on.vmdk
F=$DS/vm2b-off/vm2b-off-flat.vmdk
for off in 3 40 41 97 200 333 480; do dd if=/dev/urandom of=$F bs=1M count=14 seek=$off conv=notrunc 2>/dev/null; done
dd if=/dev/urandom of=$DS/vm2b-off/vm2b-off_1-flat.vmdk bs=1M count=64 conv=notrunc 2>/dev/null
F=$DS/vm2b-on/vm2b-on-flat.vmdk
for off in 1 30 31 100 200; do dd if=/dev/urandom of=$F bs=1M count=10 seek=$off conv=notrunc 2>/dev/null; done
# vm2b-hist: СВОЁ дерево снимков s1 -> s2 и s1 -> s3, текущее состояние на s3 (диск 128 МБ thin с данными в базе)
rm -rf $DS/vm2b-hist
make_vm vm2b-hist x ""
vmkfstools -c 128M -d thin $DS/vm2b-hist/vm2b-hist.vmdk
for off in 2 40 77; do dd if=/dev/urandom of=$DS/vm2b-hist/vm2b-hist-flat.vmdk bs=1M count=6 seek=$off conv=notrunc 2>/dev/null; done
vim-cmd solo/registervm $DS/vm2b-off/vm2b-off.vmx
vim-cmd solo/registervm $DS/vm2b-on/vm2b-on.vmx
vim-cmd solo/registervm $DS/vm2b-hist/vm2b-hist.vmx
HID=$(vim-cmd vmsvc/getallvms | grep " vm2b-hist " | cut -d" " -f1)
vim-cmd vmsvc/snapshot.create $HID s1 "" 0 0 >/dev/null
vim-cmd vmsvc/snapshot.create $HID s2 "" 0 0 >/dev/null
S1=$(vim-cmd vmsvc/snapshot.get $HID | awk '/Snapshot Name *: s1$/ {getline; print $NF; exit}')
vim-cmd vmsvc/snapshot.revert $HID $S1 1 >/dev/null
vim-cmd vmsvc/snapshot.create $HID s3 "" 0 0 >/dev/null
vim-cmd vmsvc/snapshot.get $HID | grep -E "Name"
vim-cmd vmsvc/getallvms
for f in $DS/vm2b-off/vm2b-off-flat.vmdk $DS/vm2b-off/vm2b-off_1-flat.vmdk $DS/vm2b-on/vm2b-on-flat.vmdk $DS/vm2b-hist/vm2b-hist-flat.vmdk; do echo "HASH $(basename $f) $(sha256sum $f | cut -d' ' -f1)"; done
GUEST
) || { echo "$OUT"; echo 'ошибка внутри ESXi'; exit 1; }
echo "$OUT" | grep -v '^HASH '
echo "$OUT" | python3 -c "
import sys, json
h = {}
for l in sys.stdin:
    if l.startswith('HASH '):
        _, f, d = l.split()
        vm = f.split('-flat')[0].rsplit('_', 1)[0] if '_1-flat' in f else f.split('-flat')[0]
        h.setdefault(vm, {})[f] = d
json.dump(h, open('$ROOT/.winvm/esxi/hashes.json', 'w'))
"
chmod 600 "$ROOT/.winvm/esxi/hashes.json"
