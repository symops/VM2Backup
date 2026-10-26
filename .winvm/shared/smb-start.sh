#!/bin/bash
# (VM2Backup harness) runs inside the VM container: starts a Samba of its own (the one qemu spawns dies here: "Failed to create session") on
# 127.0.0.1, where the user-mode network of qemu forwards \\10.0.2.4\qemu to; the share is the folder /shared
mkdir -p /var/lib/samba/sysvol /var/lib/samba/private /var/lib/samba/bind-dns /run/samba /var/log/samba /tmp/vm2bsmb
cat > /tmp/vm2bsmb/smb.conf <<'EOF'
[global]
interfaces = 127.0.0.1
bind interfaces only = yes
private dir = /tmp/vm2bsmb
pid directory = /tmp/vm2bsmb
lock directory = /tmp/vm2bsmb
state directory = /tmp/vm2bsmb
cache directory = /tmp/vm2bsmb
log file = /tmp/vm2bsmb/log.smbd
security = user
map to guest = Bad User
server min protocol = SMB2
load printers = no
disable spoolss = yes
usershare max shares = 0
[qemu]
path = /shared
read only = no
guest ok = yes
force user = root
oplocks = no
level2 oplocks = no
kernel oplocks = no
strict locking = no
mangled names = no
case sensitive = auto
EOF
smbd -s /tmp/vm2bsmb/smb.conf -D
sleep 2
ss -ltn | grep -E ":445" || echo "smbd is not listening"
tail -3 /tmp/vm2bsmb/log.smbd 2>/dev/null
