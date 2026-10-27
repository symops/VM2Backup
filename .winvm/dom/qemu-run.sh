#!/bin/bash
# Работает ВНУТРИ образа dockurr/windows (нужен только его qemu): ВМ Windows Server 2022 для теста домена AD на чистом qemu.
# Окружение: VM_NAME (vm2b-dc | vm2b-member), ROLE (dc | member), VM_RAM, VM_CPUS.  /shared = общая папка ВМ (\\10.0.2.4\qemu).
# Две сетевые карты e1000: n0 = user-режим (хост, общая папка, интернета нет нужды), n1 = внутренняя сеть домена 10.77.0.0/24:
# L2-туннель между контейнерами (DC слушает :7700, member подключается к vm2b-dc:7700 по docker-сети vm2b-domnet).
# MAC: n0 52:54:00:12:34:0X, n1 52:54:00:77:00:0X (X=1 DC, 2 member) - по ним скрипты гостя находят карты.
[ -f /s/disk.img ] || qemu-img create -f raw /s/disk.img 40G
bash /shared/smb-start.sh
if [ "$ROLE" = dc ]; then N1="socket,id=n1,listen=:7700"; X=1; else N1="socket,id=n1,connect=vm2b-dc:7700"; X=2; fi
EXTRA=""; [ -f /x/answer-$ROLE.iso ] && EXTRA="-drive file=/x/answer-$ROLE.iso,media=cdrom,if=ide,index=3,readonly=on"
exec qemu-system-x86_64 -machine pc -accel kvm -cpu host,hv_relaxed,hv_time,hv_vapic,hv_spinlocks=0x1fff -m ${VM_RAM:-3G} -smp ${VM_CPUS:-2} -name $VM_NAME \
  -drive file=/s/disk.img,format=raw,if=ide,index=0 \
  -drive file=/boot.iso,media=cdrom,if=ide,index=2,readonly=on $EXTRA \
  -boot order=dc -vga std -display none -vnc :0 -monitor unix:/tmp/m.sock,server=on,wait=off \
  -netdev "user,id=n0,smb=/shared,smbserver=10.0.2.4,hostfwd=tcp::18443-:8443,hostfwd=tcp::18444-:8444,guestfwd=tcp:10.0.2.50:445-cmd:nc 127.0.0.1 4455,guestfwd=tcp:10.0.2.51:445-cmd:nc 127.0.0.1 4455" -device e1000,netdev=n0,mac=52:54:00:12:34:0$X \
  -netdev "$N1" -device e1000,netdev=n1,mac=52:54:00:77:00:0$X \
  -rtc base=localtime -usb -device usb-tablet
