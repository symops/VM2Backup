#!/bin/bash
# Настоящий ESXi 7.0U3 как вложенная ВМ под KVM/qemu (ресурсы vm2b-esxi*).  Образ только читается.
#   tools/esxi7/vm.sh install  <путь к VMware-VMvisor-Installer-7.0U3n-*.iso>   автоматическая установка (kickstart, без кликов)
#   tools/esxi7/vm.sh start | stop | status | shot <файл.ppm> | delete
# Пароль root генерируется в .winvm/esxi/secret (права 600, не в git) и НИКОГДА не печатается.
# Сеть: ИЗОЛИРОВАННАЯ host-only 10.213.77.0/24 (резерв под стенд ESXi): мост vm2besxi0 (хост 10.213.77.1) + tap vm2besxi0t, ESXi 10.213.77.10 (статический),
# VCSA 10.213.77.11.  Без ip_forward, masquerade, маршрутов и DHCP; правил iptables/nftables не добавляется.  Создание/удаление: net-up / net-down.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/.winvm/esxi
TMPD=${TMPDIR:-/home/symo/tmp-esxi7}
mkdir -p "$D" "$TMPD"
PID=$D/qemu.pid
MON=$D/mon.sock
common() {
  echo -name vm2b-esxi -machine q35,accel=kvm -cpu host,+vmx,migratable=off -smp ${ESXI_SMP:-4} -m ${ESXI_MEM:-8192} -display none \
    -pidfile "$PID" -monitor unix:"$MON",server,nowait -serial file:"$D/serial.log" \
    -drive file="$D/esxi-os.qcow2",if=none,id=d0,format=qcow2 -device ide-hd,drive=d0,bus=ide.0 \
    -drive file="$D/esxi-ds.qcow2",if=none,id=d1,format=qcow2 -device ide-hd,drive=d1,bus=ide.1 \
    -netdev tap,id=n0,ifname=vm2besxi0t,script=no,downscript=no,vnet_hdr=off -device vmxnet3,netdev=n0
}
net_up() {
  ip link show vm2besxi0 >/dev/null 2>&1 || { ip link add vm2besxi0 type bridge; ip addr add 10.213.77.1/24 dev vm2besxi0; ip link set vm2besxi0 up; }
  ip link show vm2besxi0t >/dev/null 2>&1 || { ip tuntap add dev vm2besxi0t mode tap; ip link set vm2besxi0t master vm2besxi0; ip link set vm2besxi0t up; }
}
net_down() { ip link del vm2besxi0t 2>/dev/null || true; ip link del vm2besxi0 2>/dev/null || true; }
case "$1" in
net-up) net_up ;;
net-down) net_down ;;
install)
  ISO=$2; [ -r "$ISO" ] || { echo "нет ISO"; exit 1; }
  net_up
  [ -f "$D/secret" ] || { umask 077; python3 -c "import secrets,string;a=string.ascii_letters+string.digits;print(''.join(secrets.choice(a) for _ in range(14))+'Aa1@')" > "$D/secret"; }
  [ -f "$D/esxi-os.qcow2" ] || { qemu-img create -q -f qcow2 "$D/esxi-os.qcow2" 60G; qemu-img create -q -f qcow2 "$D/esxi-ds.qcow2" 40G; }
  W=$TMPD/isobuild; rm -rf "$W"; mkdir -p "$W"; 7z x -y -o"$W/iso" "$ISO" >/dev/null
  umask 077
  PW=$(cat "$D/secret")
  # kickstart кладётся в образ отдельным модулем ks.tgz (распаковывается в корень ramdisk установщика), ks=file:///ks.cfg:
  # ks=cdrom:/KS.CFG установщик на этом образе не находил, по HTTP (10.0.2.2) сеть установщика не поднималась.
  # Пароль лежит только в КОПИИ образа во временной папке; она удаляется после установки (rm -rf isobuild).
  mkdir -p "$W/ks"
  cat > "$W/ks/ks.cfg" <<KS
vmaccepteula
rootpw $PW
install --firstdisk=local --overwritevmfs --novmfsondisk
network --bootproto=static --ip=10.213.77.10 --netmask=255.255.255.0 --device=vmnic0 --addvmportgroup=1
reboot

%firstboot --interpreter=busybox
vim-cmd hostsvc/enable_ssh
vim-cmd hostsvc/start_ssh
vim-cmd hostsvc/enable_esx_shell
vim-cmd hostsvc/start_esx_shell
esxcli system settings advanced set -o /UserVars/SuppressShellWarning -i 1
esxcli network nic cso set -n vmnic0 -e 0
echo "esxcli network nic cso set -n vmnic0 -e 0" >> /etc/rc.local.d/local.sh
KS
  (cd "$W/ks" && tar czf "$W/iso/KS.TGZ" ks.cfg)
  for f in "$W/iso/BOOT.CFG" "$W/iso/EFI/BOOT/BOOT.CFG"; do
    sed -i -e 's#^kernelopt=.*#kernelopt=runweasel ks=file:///ks.cfg cdromBoot#' -e '/^modules=/ s#$# --- /ks.tgz#' "$f"
  done
  genisoimage -quiet -N -relaxed-filenames -allow-lowercase -allow-multidot -D -r -J -V ESXI7KS -b ISOLINUX.BIN -c BOOT.CAT \
    -no-emul-boot -boot-load-size 4 -boot-info-table -o "$W/esxi-ks.iso" "$W/iso" 2>/dev/null
  rm -rf "$W/iso"
  # -no-reboot: после kickstart-перезагрузки qemu завершается, затем запускаем уже с диска (start)
  qemu-system-x86_64 $(common) -no-reboot -cdrom "$W/esxi-ks.iso" -boot d -daemonize
  echo "установка запущена; ждите завершения qemu (tools/esxi7/vm.sh status), затем: start; удалить $W после установки"
  ;;
start) net_up; qemu-system-x86_64 $(common) -boot c -daemonize; echo started ;;
stop)
  [ -f "$PID" ] && P=$(cat "$PID") && kill -0 "$P" 2>/dev/null && { echo system_powerdown | socat - unix-connect:"$MON" >/dev/null 2>&1 || true; sleep 20; kill -0 "$P" 2>/dev/null && kill "$P"; }
  echo stopped ;;
status) [ -f "$PID" ] && kill -0 "$(cat "$PID")" 2>/dev/null && echo running || echo stopped ;;
shot) echo "screendump $2" | socat - unix-connect:"$MON" >/dev/null ;;
ks-stop) [ -f "$D/ks-http.pid" ] && kill "$(cat "$D/ks-http.pid")" 2>/dev/null; rm -f "$D/ks-http.pid"; rm -rf "$TMPD/isobuild/ks" ;;
delete) net_down; rm -f "$D/esxi-os.qcow2" "$D/esxi-ds.qcow2" "$D/serial.log" "$PID" "$MON"; rm -rf "$TMPD/isobuild" ;;
*) sed -n 2,7p "$0" ;;
esac
