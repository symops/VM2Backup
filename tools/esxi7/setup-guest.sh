#!/bin/bash
# Внутри вложенного ESXi: VMFS-датастор на втором диске и тестовые ВМ БЕЗ ОС (vmx + vmdk) с известным содержимым.
#   vm2b-off  выключенная: диск 1 thin 512 МБ (≈100 МБ случайных данных кусками, остальное дыры), диск 2 zeroedthick 64 МБ целиком случайный
#   vm2b-on   включённая:  диск thin 256 МБ (≈50 МБ случайных данных); процесс ВМ крутит BIOS и пишет только в дельту снимка
# Результат: .winvm/esxi/hashes.json  {"vm2b-off": {"vm2b-off-flat.vmdk": "<sha256>", ...}, ...}  (sha256sum на самом хосте, ДО бэкапа)
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; ROOT=$(cd "$D/../.." && pwd)
$S 'sh -s' <<'GUEST'
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
$3
firmware = "bios"
vmx.scoreboard.enabled = "FALSE"
VMX
}
rm -rf $DS/vm2b-off $DS/vm2b-on
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
vim-cmd solo/registervm $DS/vm2b-off/vm2b-off.vmx
vim-cmd solo/registervm $DS/vm2b-on/vm2b-on.vmx
vim-cmd vmsvc/getallvms
for f in $DS/vm2b-off/*-flat.vmdk $DS/vm2b-on/*-flat.vmdk; do echo "HASH $(basename $f) $(sha256sum $f | cut -d' ' -f1)"; done
GUEST
