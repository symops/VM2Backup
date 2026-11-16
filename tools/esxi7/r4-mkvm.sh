#!/bin/bash
# Раунд 4: ВМ без ОС (vmx + thin vmdk с известным случайным содержимым) на указанном хосте и датасторе; эталон sha256 печатается строкой "HASH <файл> <sha256>".
#   ESXI_DIR=esxi2 ESXI_IP=10.213.77.12 tools/esxi7/r4-mkvm.sh <датастор> <имя> <размер МБ> <данных МБ> [register]
# Служебные настройки pciBridge нужны, чтобы включённая ВМ запускалась на вложенном ESXi (см. setup-guest.sh).
D=$(cd "$(dirname "$0")" && pwd)
exec "$D/ssh.sh" "sh -s" <<GUEST
set -e
V=/vmfs/volumes/$1/$2; mkdir -p \$V
cat > \$V/$2.vmx <<VMX
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "$2"
guestOS = "other-64"
memSize = "128"
numvcpus = "1"
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "$2.vmdk"
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
vmx.scoreboard.enabled = "FALSE"
VMX
vmkfstools -c $3M -d thin \$V/$2.vmdk
n=$4; off=1
while [ \$n -gt 0 ]; do c=8; [ \$n -lt 8 ] && c=\$n; dd if=/dev/urandom of=\$V/$2-flat.vmdk bs=1M count=\$c seek=\$off conv=notrunc 2>/dev/null; n=\$((n-c)); off=\$((off+c+6)); done
[ "$5" = register ] && vim-cmd solo/registervm \$V/$2.vmx
echo "HASH $2-flat.vmdk \$(sha256sum \$V/$2-flat.vmdk | cut -d' ' -f1)"
GUEST
