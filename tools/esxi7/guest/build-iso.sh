#!/bin/bash
# Собирает КОПИЮ образа Alpine virt с apkovl: гость автоматически запускает vm2bio (исходный ISO только читается, в git не попадает).
#   build-iso.sh writer <out.iso> [rate]        писатель: диск данных заполняется, затем пишется непрерывно (записи/с, по умолчанию 150)
#   build-iso.sh verify <out.iso> <lo> <hi>     проверяющий: после загрузки читает диск данных и печатает на serial «VERIFY OK seq<=S ...» / «VERIFY BAD ...»
# Нужно: genisoimage, 7z, gcc (статический vm2bio), исходный ISO в .winvm/guest-images/ (путь можно задать ALPINE_ISO).  TMPDIR на /home.
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../../.." && pwd)
SRC=${ALPINE_ISO:-$ROOT/.winvm/guest-images/alpine-virt-3.24.3-x86_64.iso}
T=${TMPDIR:-/home/symo/tmp-esxi7c}; mkdir -p "$T"
MODE=$1; OUT=$2
[ -r "$SRC" ] || { echo "нет $SRC"; exit 1; }
[ -n "$MODE" ] && [ -n "$OUT" ] || { sed -n 2,5p "$0"; exit 1; }
B=$T/alpine-tree
[ -d "$B/boot" ] || { rm -rf "$B"; mkdir -p "$B"; 7z x -y -o"$B" "$SRC" >/dev/null; rm -rf "$B/[BOOT]"; }
[ -x "$T/vm2bio" ] && [ "$T/vm2bio" -nt "$D/vm2bio.c" ] || gcc -O2 -Wall -static -o "$T/vm2bio" "$D/vm2bio.c"
O=$T/ovl; rm -rf "$O"; mkdir -p "$O/etc/apk" "$O/etc/local.d" "$O/usr/local/bin" "$O/etc/runlevels/"{sysinit,boot,default,shutdown}
echo alpine-base > "$O/etc/apk/world"; echo vm2b-guest > "$O/etc/hostname"
for s in devfs dmesg mdev hwdrivers modloop; do ln -s /etc/init.d/$s "$O/etc/runlevels/sysinit/$s"; done
for s in modules sysctl hostname bootmisc syslog hwclock; do ln -s /etc/init.d/$s "$O/etc/runlevels/boot/$s"; done
ln -s /etc/init.d/local "$O/etc/runlevels/default/local"
for s in mount-ro killprocs savecache; do ln -s /etc/init.d/$s "$O/etc/runlevels/shutdown/$s"; done
cp "$T/vm2bio" "$O/usr/local/bin/vm2bio"
case "$MODE" in
writer)
  RATE=${3:-150}
  cat > "$O/etc/local.d/vm2b.start" <<S
#!/bin/sh
echo "BOOT writer \$(date +%s) \$(uname -r)" > /dev/ttyS0
i=0; while [ ! -b /dev/sda ] && [ \$i -lt 120 ]; do sleep 1; i=\$((i+1)); done
echo "DISK \$(ls /dev/sd* 2>&1 | tr '\n' ' ')" > /dev/ttyS0
VM2B_TTY=1 nohup /usr/local/bin/vm2bio write /dev/sda --rate $RATE >/dev/null 2>&1 &
S
  ;;
verify)
  LO=$3; HI=$4; [ -n "$LO" ] && [ -n "$HI" ] || { echo "нужны lo hi"; exit 1; }
  cat > "$O/etc/local.d/vm2b.start" <<S
#!/bin/sh
echo "BOOT verify \$(date +%s) \$(uname -r)" > /dev/ttyS0
i=0; while [ ! -b /dev/sda ] && [ \$i -lt 120 ]; do sleep 1; i=\$((i+1)); done
VM2B_TTY=1 nohup /usr/local/bin/vm2bio verify /dev/sda $LO $HI >/dev/null 2>&1 &
S
  ;;
*) echo "режим writer|verify"; exit 1 ;;
esac
chmod 755 "$O/etc/local.d/vm2b.start"
W=$T/isowork; rm -rf "$W"; cp -a "$B" "$W"
(cd "$O" && tar --owner=0 --group=0 -czf "$W/localhost.apkovl.tar.gz" .)
# консоль ядра на serial (KCONSOLE=1) очень замедляет загрузку во вложенном ESXi (каждый символ — выход в гипервизор): по умолчанию тихая загрузка, журнал гостя пишет vm2bio сам
if [ -n "$KCONSOLE" ]; then KO="console=ttyS0,115200"; else KO="quiet"; fi
sed -i "s/^APPEND .*/APPEND modules=loop,squashfs,sd-mod,usb-storage $KO/" "$W/boot/syslinux/syslinux.cfg"
genisoimage -quiet -V "alpine-virt 3.24.3 x86_64" -r -J -joliet-long -b boot/syslinux/isolinux.bin -c boot/syslinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table -o "$OUT" "$W"
rm -rf "$W" "$O"
echo "готово: $OUT ($(stat -c %s "$OUT") байт, режим $MODE)"
