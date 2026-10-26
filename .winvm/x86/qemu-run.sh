#!/bin/bash
# Runs inside the dockurr/windows image (only for its qemu): the 32-bit Windows 10 test VM on plain qemu with BIOS, an IDE disk,
# an e1000 card and a standard VGA (devices 32-bit Windows 10 has drivers for); /shared is the SMB share \\10.0.2.4\qemu.
# /boot.iso is the user's WIN10X86.iso as it is; /x/answer.iso is a tiny second CD whose autounattend.xml Windows Setup finds on its own.
# Guest network (user mode): 10.0.2.15 guest, 10.0.2.2 = the container's loopback, 10.0.2.3 dns, 10.0.2.4 = our share, any other address is
# NATted out of the container (so the Docker host is 172.17.0.1).  10.0.2.50:445 -> the container's 127.0.0.1:4455 (the password-protected
# test Samba, see smb-real.sh); hostfwd: container :18443 -> guest :8443 (the VM2Backup web site bound to the guest address).
[ -f /s/disk.img ] || qemu-img create -f raw /s/disk.img 32G
bash /shared/smb-start.sh
exec qemu-system-x86_64 -machine pc -accel kvm -cpu host,hv_relaxed,hv_time,hv_vapic,hv_spinlocks=0x1fff -m ${VM_RAM:-3G} -smp 4 \
  -drive file=/s/disk.img,format=raw,if=ide,index=0 \
  -drive file=/boot.iso,media=cdrom,if=ide,index=2,readonly=on \
  -drive file=/x/answer.iso,media=cdrom,if=ide,index=3,readonly=on \
  -boot order=dc -vga std -display none -vnc :0 -monitor unix:/tmp/m.sock,server=on,wait=off \
  -netdev "user,id=n0,smb=/shared,smbserver=10.0.2.4,hostfwd=tcp::18443-:8443,hostfwd=tcp::18444-:8444,guestfwd=tcp:10.0.2.50:445-cmd:nc 127.0.0.1 4455,guestfwd=tcp:10.0.2.51:445-cmd:nc 127.0.0.1 4455" -device e1000,netdev=n0 \
  -rtc base=localtime -usb -device usb-tablet
