#!/bin/bash
# Раунд 3, пункт A: ВМ vm2b-fs = Alpine, УСТАНОВЛЕННЫЙ на диск (guest-fs-build-disk.sh), корень и диск данных ext4, open-vm-tools (vmbackup), без сети; serial -> файл на датасторе.
#   guest-fs-setup.sh <каталог с root.raw и data.raw> [датастор, по умолчанию datastore1] [имя ВМ vm2b-fs]
# Диски SATA (sata0:0 корень, sata0:1 данные): гость сам находит данные по метке vm2bdata (имена sda/sdb у SATA не детерминированы).  ВМ включается; прежняя ВМ с этим именем удаляется.
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; SRC=$(realpath "$1"); DS=${2:-datastore1}; VM=${3:-vm2b-fs}
[ -f "$SRC/root.raw" ] && [ -f "$SRC/data.raw" ] || { sed -n 2,5p "$0"; exit 1; }
$S "for id in \$(vim-cmd vmsvc/getallvms | grep ' $VM ' | cut -d' ' -f1); do vim-cmd vmsvc/power.off \$id >/dev/null 2>&1; vim-cmd vmsvc/snapshot.removeall \$id >/dev/null 2>&1; vim-cmd vmsvc/unregister \$id >/dev/null 2>&1; done; rm -rf /vmfs/volumes/$DS/$VM /vmfs/volumes/$DS/VM2Backup-*; mkdir -p /vmfs/volumes/$DS/$VM /vmfs/volumes/$DS/vm2b-serial; rm -f /vmfs/volumes/$DS/vm2b-serial/$VM.log
V=/vmfs/volumes/$DS/$VM; vmkfstools -c 1G -d thin \$V/$VM.vmdk; vmkfstools -c 512M -d thin \$V/${VM}_1.vmdk"
gzip -1 -c "$SRC/root.raw" | $S "gunzip -c | dd of=/vmfs/volumes/$DS/$VM/$VM-flat.vmdk bs=1M conv=notrunc 2>&1 | tail -1"
gzip -1 -c "$SRC/data.raw" | $S "gunzip -c | dd of=/vmfs/volumes/$DS/$VM/${VM}_1-flat.vmdk bs=1M conv=notrunc 2>&1 | tail -1"
$S "sh -s" <<GUEST
set -e
V=/vmfs/volumes/$DS/$VM
cat > \$V/$VM.vmx <<VMX
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "$VM"
guestOS = "other4xlinux-64"
memSize = "512"
numvcpus = "1"
sata0.present = "TRUE"
sata0:0.present = "TRUE"
sata0:0.fileName = "$VM.vmdk"
sata0:1.present = "TRUE"
sata0:1.fileName = "${VM}_1.vmdk"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "/vmfs/volumes/$DS/vm2b-serial/$VM.log"
serial0.yieldOnMsrRead = "TRUE"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
firmware = "bios"
bios.bootOrder = "hdd"
tools.syncTime = "FALSE"
vmx.scoreboard.enabled = "FALSE"
VMX
vim-cmd solo/registervm \$V/$VM.vmx
ID=\$(vim-cmd vmsvc/getallvms | grep " $VM " | cut -d" " -f1)
vim-cmd vmsvc/power.on \$ID
GUEST
echo "ВМ $VM включена; serial: $S 'cat /vmfs/volumes/$DS/vm2b-serial/$VM.log'"
