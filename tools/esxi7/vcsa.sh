#!/bin/bash
# VCSA 7.0.3 как ОТДЕЛЬНАЯ qemu-ВМ (не внутри ESXi: 12 ГБ ОЗУ во вложенный ESXi на 8 ГБ не помещаются), в той же изолированной сети 10.213.77.0/24 (VCSA = 10.213.77.11).
#   tools/esxi7/vcsa.sh prepare <VMware-VCSA-all-7.0.3-*.iso>   достаёт OVA ТОЛЬКО ЧИТАЯ образ, переводит диски в qcow2 (тонкие), собирает 17 дисков "tiny" по OVF
#   tools/esxi7/vcsa.sh start | stop | status | shot <файл.ppm> | delete
# Особенности: под qemu нет VMware Tools (guestinfo.ovfEnv), а /bin/install-parameter читает свойства OVF только оттуда, из settings.json и из файлов
# /etc/vmware/install-defaults/<ключ>: свойства кладутся файлами на диск устройства (tools/esxi7/vcsa-ovfenv.py --seed); диски по OVF «tiny» (по размерам их узнаёт первая загрузка), контроллеры LSI (в ядре устройства они встроены),
# сетевая карта vmxnet3.  Пароли (SSO, root): .winvm/esxi/secret-vc-sso, secret-vc-root (права 600), нигде не печатаются.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/.winvm/esxi
TMPD=${TMPDIR:-/home/symo/tmp-esxi7b}
V=$D/vcsa
PID=$V/qemu.pid; MON=$V/mon.sock
tap_up() {
  ip link show vm2besxi0 >/dev/null 2>&1 || { echo "нет моста vm2besxi0 (tools/esxi7/vm.sh net-up)"; exit 1; }
  ip link show vm2besxi1t >/dev/null 2>&1 || { ip tuntap add dev vm2besxi1t mode tap; ip link set vm2besxi1t master vm2besxi0; ip link set vm2besxi1t up; }
}
args() {
  echo -name vm2b-vcsa -machine pc,accel=kvm -cpu host -smp ${VCSA_SMP:-2} -m ${VCSA_MEM:-12288} -display none -pidfile "$PID" -monitor unix:"$MON",server,nowait -serial file:"$V/serial.log"
  echo -device lsi53c895a,id=s0 -device lsi53c895a,id=s1 -device lsi53c895a,id=s2
  for i in $(seq 0 16); do
    c=$((i / 7)); u=$((i % 7))
    echo -drive file="$V/d$i.qcow2",if=none,id=k$i,format=qcow2,cache=writeback -device scsi-hd,drive=k$i,bus=s$c.0,scsi-id=$u$([ $i = 0 ] && echo ,bootindex=1)
  done
  echo -netdev tap,id=n0,ifname=vm2besxi1t,script=no,downscript=no,vnet_hdr=off -device vmxnet3,netdev=n0,mac=52:54:00:12:34:77
}
case "$1" in
prepare)
  ISO=$2; [ -r "$ISO" ] || { echo "нет ISO"; exit 1; }
  mkdir -p "$V" "$TMPD/vcsa-prep"; W=$TMPD/vcsa-prep
  7z x -y -o"$W/iso" "$ISO" 'vcsa/*.ova' >/dev/null
  OVA=$(ls "$W"/iso/vcsa/*.ova); B=$(basename "$OVA" .ova)
  tar -xf "$OVA" -C "$W" "$B-disk1.vmdk" "$B-disk2.vmdk"
  qemu-img convert -O qcow2 "$W/$B-disk1.vmdk" "$V/d0.qcow2"; qemu-img convert -O qcow2 "$W/$B-disk2.vmdk" "$V/d1.qcow2"
  qemu-img resize -q "$V/d0.qcow2" 49728M; qemu-img resize -q "$V/d1.qcow2" 5827M
  i=2; for s in 25 25 10 10 15 10 1 10 10 100 50 10 5 100 150; do qemu-img create -q -f qcow2 "$V/d$i.qcow2" ${s}G; i=$((i+1)); done
  rm -rf "$W"
  # параметры первой загрузки: файлами в /etc/vmware/install-defaults на диске устройства (см. vcsa-ovfenv.py)
  M=$TMPD/vcsa-mnt; mkdir -p "$M"; guestmount -a "$V/d0.qcow2" -i "$M"
  "$ROOT/tools/esxi7/vcsa-ovfenv.py" --seed "$M/etc/vmware/install-defaults"; sync; guestunmount "$M"
  echo "готово: $V" ;;
start) tap_up; qemu-system-x86_64 $(args) -boot c -daemonize; echo started ;;
stop) [ -f "$PID" ] && P=$(cat "$PID") && kill -0 "$P" 2>/dev/null && { echo system_powerdown | socat - unix-connect:"$MON" >/dev/null 2>&1 || true; sleep 60; kill -0 "$P" 2>/dev/null && kill "$P"; }; echo stopped ;;
status) [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null && echo running || echo stopped ;;
shot) echo "screendump $2" | socat - unix-connect:"$MON" >/dev/null ;;
delete) ip link del vm2besxi1t 2>/dev/null || true; rm -rf "$V" ;;
*) sed -n 2,5p "$0" ;;
esac
