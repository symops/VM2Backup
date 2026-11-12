#!/bin/bash
# Раунд 3, пункт B: ВМ vm2b-big без ОС на настоящем ESXi: диск 1 thin 6 ГиБ (разреженный, случайные области в т.ч. ЧЕРЕЗ границу 4 ГиБ), диск 2 zeroedthick (lazy-zeroed) 1 ГиБ, диск 3 eagerzeroedthick 1 ГиБ.
# Эталон sha256 по логическому содержимому (flat-файлов) снимается на самом хосте при выключенной ВМ.  Результат: .winvm/esxi/big-hashes.json.
#   tools/esxi7/big-setup.sh [датастор, по умолчанию datastore1] [имя, по умолчанию vm2b-big]
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; ROOT=$(cd "$D/../.." && pwd); DS=${1:-datastore1}; VM=${2:-vm2b-big}
OUT=$($S "sh -s" <<GUEST
set -e
DS=/vmfs/volumes/$DS; V=\$DS/$VM
for id in \$(vim-cmd vmsvc/getallvms | grep " $VM " | cut -d" " -f1); do vim-cmd vmsvc/power.off \$id >/dev/null 2>&1 || true; vim-cmd vmsvc/snapshot.removeall \$id >/dev/null 2>&1 || true; vim-cmd vmsvc/unregister \$id >/dev/null 2>&1 || true; done
rm -rf \$V; mkdir -p \$V
cat > \$V/$VM.vmx <<VMX
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "19"
displayName = "$VM"
guestOS = "other-64"
memSize = "256"
numvcpus = "1"
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "$VM.vmdk"
scsi0:1.present = "TRUE"
scsi0:1.fileName = "${VM}_1.vmdk"
scsi0:2.present = "TRUE"
scsi0:2.fileName = "${VM}_2.vmdk"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
pciBridge5.present = "TRUE"
pciBridge5.virtualDev = "pcieRootPort"
pciBridge5.functions = "8"
firmware = "bios"
vmx.scoreboard.enabled = "FALSE"
VMX
vmkfstools -c 6G -d thin \$V/$VM.vmdk
vmkfstools -c 1G -d zeroedthick \$V/${VM}_1.vmdk
vmkfstools -c 1G -d eagerzeroedthick \$V/${VM}_2.vmdk
F=\$V/$VM-flat.vmdk
# случайные области: начало, середина, ЧЕРЕЗ границу 4 ГиБ (4090..4110 МиБ), после неё, у самого конца
for off in 5 1000 2047 4090 4100 5000 6100; do dd if=/dev/urandom of=\$F bs=1M count=20 seek=\$off conv=notrunc 2>/dev/null; done
for off in 0 900; do dd if=/dev/urandom of=\$V/${VM}_1-flat.vmdk bs=1M count=40 seek=\$off conv=notrunc 2>/dev/null; done
dd if=/dev/urandom of=\$V/${VM}_2-flat.vmdk bs=1M count=50 seek=400 conv=notrunc 2>/dev/null
vim-cmd solo/registervm \$V/$VM.vmx >/dev/null
ls -l \$V | grep flat
for f in \$V/$VM-flat.vmdk \$V/${VM}_1-flat.vmdk \$V/${VM}_2-flat.vmdk; do echo "HASH \$(basename \$f) \$(sha256sum \$f | cut -d' ' -f1) \$(stat -c %s \$f) \$(du -k \$f | cut -f1)"; done
df -h \$DS | tail -1
GUEST
) || { echo "$OUT"; exit 1; }
echo "$OUT" | grep -v '^HASH '
echo "$OUT" | python3 -c "
import sys, json
h = {}
for l in sys.stdin:
    if l.startswith('HASH '):
        _, f, d, sz, du = l.split(); h[f] = {'sha256': d, 'size': int(sz), 'du_kb': int(du)}
json.dump(h, open('$ROOT/.winvm/esxi/big-hashes.json', 'w'), indent=1)
print(json.dumps(h, indent=1))
"
