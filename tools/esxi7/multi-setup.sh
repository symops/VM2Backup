#!/bin/bash
# Раунд 3, пункт C: ВМ vm2b-ms на ДВУХ датасторах (диск 1 thin 128 МБ на datastore1 вместе с .vmx, диск 2 zeroedthick 64 МБ на datastore2); эталон sha256 на хосте -> .winvm/esxi/ms-hashes.json.
set -e
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; ROOT=$(cd "$D/../.." && pwd); VM=vm2b-ms
OUT=$($S "sh -s" <<GUEST
set -e
D1=/vmfs/volumes/datastore1; D2=/vmfs/volumes/datastore2
for id in \$(vim-cmd vmsvc/getallvms | grep " $VM " | cut -d" " -f1); do vim-cmd vmsvc/power.off \$id >/dev/null 2>&1 || true; vim-cmd vmsvc/unregister \$id >/dev/null 2>&1 || true; done
rm -rf \$D1/$VM \$D2/$VM; mkdir -p \$D1/$VM \$D2/$VM
cat > \$D1/$VM/$VM.vmx <<VMX
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
scsi0:1.fileName = "/vmfs/volumes/datastore2/$VM/${VM}_1.vmdk"
pciBridge0.present = "TRUE"
pciBridge4.present = "TRUE"
pciBridge4.virtualDev = "pcieRootPort"
pciBridge4.functions = "8"
firmware = "bios"
vmx.scoreboard.enabled = "FALSE"
VMX
vmkfstools -c 128M -d thin \$D1/$VM/$VM.vmdk
vmkfstools -c 64M -d zeroedthick \$D2/$VM/${VM}_1.vmdk
for off in 3 50 100; do dd if=/dev/urandom of=\$D1/$VM/$VM-flat.vmdk bs=1M count=12 seek=\$off conv=notrunc 2>/dev/null; done
dd if=/dev/urandom of=\$D2/$VM/${VM}_1-flat.vmdk bs=1M count=64 conv=notrunc 2>/dev/null
vim-cmd solo/registervm \$D1/$VM/$VM.vmx >/dev/null
for f in \$D1/$VM/$VM-flat.vmdk \$D2/$VM/${VM}_1-flat.vmdk; do echo "HASH \$(basename \$f) \$(sha256sum \$f | cut -d' ' -f1) \$(stat -c %s \$f)"; done
vim-cmd vmsvc/getallvms | grep " $VM "
GUEST
) || { echo "$OUT"; exit 1; }
echo "$OUT" | grep -v '^HASH '
echo "$OUT" | python3 -c "
import sys, json
h = {}
for l in sys.stdin:
    if l.startswith('HASH '):
        _, f, d, sz = l.split(); h[f] = {'sha256': d, 'size': int(sz)}
json.dump(h, open('$ROOT/.winvm/esxi/ms-hashes.json', 'w'), indent=1); print(json.dumps(h))
"
