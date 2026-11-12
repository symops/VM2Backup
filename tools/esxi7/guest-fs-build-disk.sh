#!/bin/bash
# Собирает ДИСКИ гостя с НАСТОЯЩЕЙ файловой системой локально под qemu/KVM (быстро), потом guest-fs-setup.sh переносит их на ESXi.
#   guest-fs-build-disk.sh <каталог-результата>        даёт root.raw (1 ГиБ: Alpine, УСТАНОВЛЕННЫЙ `setup-disk -m sys`, ext4) и data.raw (512 МиБ, ext4, UUID фиксирован)
# Установка автоматическая: apkovl на КОПИИ образа Alpine virt (исходный только читается); пакеты - с самого образа и open-vm-tools из .winvm/guest-images/vmtools-apks (сети нет;
# пакеты скачаны и проверены guest/fetch-apks.py: подпись индекса и контрольные суммы).
# В установленную систему кладутся: open-vm-tools (+ vmbackup), /usr/local/bin/fsw, /etc/local.d/vm2b.start (проверка диска данных при каждой загрузке и писатель), скрипт quiesce.
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); [ -n "$1" ] || { sed -n 2,4p "$0"; exit 1; }; OUT=$(realpath -m "$1")
T=${TMPDIR:-/home/symo/tmp-esxi7f}; mkdir -p "$T" "$OUT"
SRC=$ROOT/.winvm/guest-images/alpine-virt-3.24.3-x86_64.iso; REPO=$ROOT/.winvm/guest-images/alpine-repo      # guest/fetch-apks.py
gcc -O2 -static -o "$T/fsw" "$D/guest/fsw.c"
B=$T/alpine-tree; [ -d "$B/boot" ] || { rm -rf "$B"; mkdir -p "$B"; 7z x -y -o"$B" "$SRC" >/dev/null; rm -rf "$B/[BOOT]"; }
O=$T/ovl-fs; rm -rf "$O"; mkdir -p "$O/etc/apk" "$O/etc/local.d" "$O/etc/runlevels/"{sysinit,boot,default,shutdown}
echo alpine-base > "$O/etc/apk/world"; echo vm2b-install > "$O/etc/hostname"
for s in devfs dmesg mdev hwdrivers modloop; do ln -s /etc/init.d/$s "$O/etc/runlevels/sysinit/$s"; done
for s in modules sysctl hostname bootmisc syslog hwclock; do ln -s /etc/init.d/$s "$O/etc/runlevels/boot/$s"; done
ln -s /etc/init.d/local "$O/etc/runlevels/default/local"
cat > "$O/etc/local.d/install.start" <<'S'
#!/bin/sh
exec >/dev/ttyS0 2>&1
echo "INSTALL begin"
M=/media/cdrom; [ -d $M/apks ] || M=/media/sr0
printf "%s\n%s\n%s\n" "$M/apks" "$M/repo/main" "$M/repo/community" > /etc/apk/repositories
# имена sda/sdb у SATA не детерминированы: диски находятся по размеру (корень 1 ГиБ, данные 512 МиБ)
for d in /sys/block/sd*; do n=$(cat $d/size); b=${d##*/}; [ "$n" = 2097152 ] && RD=$b; [ "$n" = 1048576 ] && DD=$b; done
echo "DISKS root=$RD data=$DD"
export ERASE_DISKS=/dev/$RD ROOTFS=ext4 BOOTLOADER=syslinux
echo y | setup-disk -m sys -s 0 /dev/$RD
echo "SETUPDISK rc=$?"
mkdir -p /mnt
for p in /dev/${RD}3 /dev/${RD}2 /dev/${RD}1; do mount $p /mnt 2>/dev/null && [ -d /mnt/etc ] && break; umount /mnt 2>/dev/null; done
echo "ROOTMOUNT $(df /mnt | tail -1)"
apk add --root /mnt --no-network --no-cache --repository $M/repo/main --repository $M/repo/community open-vm-tools open-vm-tools-vmbackup open-vm-tools-openrc e2fsprogs-extra
echo "APK rc=$?"
cp $M/fsw /mnt/usr/local/bin/fsw; chmod 755 /mnt/usr/local/bin/fsw
cp $M/vm2b.start /mnt/etc/local.d/vm2b.start; chmod 755 /mnt/etc/local.d/vm2b.start
mkdir -p /mnt/etc/vmware-tools/backupScripts.d; cp $M/10-vm2b /mnt/etc/vmware-tools/backupScripts.d/10-vm2b; chmod 755 /mnt/etc/vmware-tools/backupScripts.d/10-vm2b
printf '[logging]\nlog = true\nvmsvc.level = debug\nvmsvc.handler = file\nvmsvc.data = /var/log/vmsvc.log\nvmbackup.level = debug\n' >> /mnt/etc/vmware-tools/tools.conf
rm -f /mnt/etc/vmware-tools/poweroff-vm-default   # СИМВОЛИЧЕСКАЯ ссылка на statechange.sh: запись «через» неё ломала и poweron-vm-default (гость выключался сам)
printf '#!/bin/sh\nexec /sbin/poweroff\n' > /mnt/etc/vmware-tools/poweroff-vm-default; chmod 755 /mnt/etc/vmware-tools/poweroff-vm-default
chroot /mnt rc-update add local default; chroot /mnt rc-update add open-vm-tools default
echo vm2b-fs > /mnt/etc/hostname; : > /mnt/etc/apk/repositories
mkdir -p /mnt/data; rm -f /mnt/etc/local.d/install.start
# серийная консоль ядра выключена (в гостях ESXi каждый символ дорог): тихая загрузка
for f in /mnt/boot/extlinux.conf; do [ -f $f ] && sed -i 's/^\(\s*APPEND .*\)$/\1 quiet/;s/console=ttyS0[,0-9a-z]*//' $f; done
grep -i -E "append|root=" /mnt/boot/extlinux.conf
cat /mnt/etc/fstab
sync; umount /mnt
mkfs.ext4 -q -F -L vm2bdata -U 7e57da7a-0000-4000-8000-0000000d47a1 /dev/$DD
echo "INSTALL done"
poweroff
S
chmod 755 "$O/etc/local.d/install.start"
W=$T/isowork-fs; rm -rf "$W"; cp -a "$B" "$W"
(cd "$O" && tar --owner=0 --group=0 -czf "$W/localhost.apkovl.tar.gz" .)
sed -i "s/^APPEND .*/APPEND modules=loop,squashfs,sd-mod,usb-storage,ahci quiet console=ttyS0/" "$W/boot/syslinux/syslinux.cfg"
cp -r "$REPO" "$W/repo"; cp "$T/fsw" "$W/fsw"; cp "$D/guest/fs-guest.start" "$W/vm2b.start"; cp "$D/guest/fs-quiesce.sh" "$W/10-vm2b"
genisoimage -quiet -V "alpine-virt 3.24.3 x86_64" -r -J -joliet-long -b boot/syslinux/isolinux.bin -c boot/syslinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -o "$T/fs-install.iso" "$W"
rm -rf "$W" "$O"
rm -f "$OUT/root.raw" "$OUT/data.raw"; truncate -s 1G "$OUT/root.raw"; truncate -s 512M "$OUT/data.raw"; trap - EXIT
rm -f "$T/install-serial.log"
timeout 900 qemu-system-x86_64 -machine q35,accel=kvm -cpu host -smp 2 -m 1024 -display none -no-reboot -serial file:"$T/install-serial.log" \
  -drive file="$OUT/root.raw",if=none,id=a,format=raw -device ide-hd,drive=a,bus=ide.0 -drive file="$OUT/data.raw",if=none,id=b,format=raw -device ide-hd,drive=b,bus=ide.1 \
  -cdrom "$T/fs-install.iso" -boot d || true
grep -E "DISKS|SETUPDISK|APK rc|INSTALL done" "$T/install-serial.log" | tail -5
fdisk -l "$OUT/root.raw" | grep -q "root.raw2" || { echo "ОШИБКА: на root.raw нет разделов"; exit 1; }
dumpe2fs -h "$OUT/data.raw" >/dev/null 2>&1 || { echo "ОШИБКА: data.raw не ext4"; exit 1; }
