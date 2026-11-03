#!/bin/bash
# Внутри вложенного ESXi: ВМ vm2b-guest = Alpine Linux (live из ОЗУ, образ с apkovl, tools/esxi7/guest/build-iso.sh) + диск данных 256 МБ thin (pvscsi), без сети;
# serial-порт ВМ пишется в ФАЙЛ на датасторе (vm2b-serial/guest.log): хронология гостя без сети и без VMware Tools.  ISO и serial лежат ВНЕ папки ВМ (бэкап копирует только папку ВМ и диски).
#   [GUEST_MODE=tools] tools/esxi7/guest-setup.sh [записей/с, по умолчанию 150]       создаёт и включает ВМ (повторный запуск: старая vm2b-guest выключается, удаляется)
# Нужно: запущенный ESXi со стендом (tools/esxi7/setup-guest.sh уже создал datastore1), genisoimage, 7z, gcc.
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; T=${TMPDIR:-/home/symo/tmp-esxi7c}; RATE=${1:-150}
mkdir -p "$T"
TMPDIR=$T "$D/guest/build-iso.sh" ${GUEST_MODE:-writer} "$T/alpine-writer.iso" "$RATE"      # GUEST_MODE=tools: с VMware Tools (open-vm-tools), нужна для ShutdownGuest
$S 'mkdir -p /vmfs/volumes/datastore1/vm2b-iso /vmfs/volumes/datastore1/vm2b-serial; for id in $(vim-cmd vmsvc/getallvms | grep " vm2b-guest " | cut -d" " -f1); do vim-cmd vmsvc/power.off $id >/dev/null 2>&1; vim-cmd vmsvc/snapshot.removeall $id >/dev/null 2>&1; vim-cmd vmsvc/unregister $id >/dev/null 2>&1; done; rm -rf /vmfs/volumes/datastore1/vm2b-guest /vmfs/volumes/datastore1/VM2Backup-* /vmfs/volumes/datastore1/vm2b-serial/*'
$S 'cat > /vmfs/volumes/datastore1/vm2b-iso/alpine-writer.iso' < "$T/alpine-writer.iso"
$S 'sh -s' <<'GUEST'
set -e
DS=/vmfs/volumes/datastore1; V=$DS/vm2b-guest; mkdir -p $V
cat > $V/vm2b-guest.vmx <<VMX
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "vm2b-guest"
guestOS = "other4xlinux-64"
memSize = "512"
numvcpus = "1"
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "vm2b-guest.vmdk"
ide0:0.present = "TRUE"
ide0:0.deviceType = "cdrom-image"
ide0:0.fileName = "$DS/vm2b-iso/alpine-writer.iso"
ide0:0.startConnected = "TRUE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "$DS/vm2b-serial/guest.log"
serial0.yieldOnMsrRead = "TRUE"
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
firmware = "bios"
bios.bootOrder = "cdrom"
tools.syncTime = "FALSE"
vmx.scoreboard.enabled = "FALSE"
VMX
vmkfstools -c 256M -d thin $V/vm2b-guest.vmdk
vim-cmd solo/registervm $V/vm2b-guest.vmx
ID=$(vim-cmd vmsvc/getallvms | grep " vm2b-guest " | cut -d" " -f1)
vim-cmd vmsvc/power.on $ID
GUEST
echo "ВМ включена; serial: $S 'cat /vmfs/volumes/datastore1/vm2b-serial/guest.log'"
