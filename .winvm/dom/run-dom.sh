#!/bin/bash
# run-dom.sh dc|member : запускает ВМ (контейнер vm2b-dc / vm2b-member на образе dockurr/windows, чистый qemu внутри; см. qemu-run.sh).
# Возвращается сразу; установка Windows Server 2022 занимает 25-40 минут (wait-dom.sh <роль> ждёт исполнителя).
# Каталоги: .winvm/dom/storage-<роль> (диск), .winvm/dom/shared-<роль> (общая папка: jobs, results, pkg). VNC: 127.0.0.1:5909 (dc), 5910 (member).
D=/home/symo/Claude/VM2Backup/.winvm/dom; W=/home/symo/Claude/VM2Backup/.winvm
ISO=/home/symo/Downloads/2222222/ISO/ru-ru_windows_server_2022_updated_feb_2023_x64_dvd_11cf5ceb.iso
ROLE=$1
case "$ROLE" in
  dc)     NAME=vm2b-dc;     NAME_AD=VM2B-DC; SHORT=DC;     IP=10.77.0.1; MAC=52-54-00-77-00-01; VNC=5909; RAM=${DC_RAM:-3G};     CPUS=${DC_CPUS:-2};;
  member) NAME=vm2b-member; NAME_AD=VM2B-MEMBER; SHORT=MEMBER; IP=10.77.0.2; MAC=52-54-00-77-00-02; VNC=5910; RAM=${MEMBER_RAM:-3G}; CPUS=${MEMBER_CPUS:-4};;
  *) echo "usage: run-dom.sh dc|member"; exit 1;;
esac
docker ps -a --format '{{.Names}}' | grep -qx "$NAME" && { echo "container $NAME exists: run destroy-dom.sh first"; exit 1; }
FREE=$(df -BG --output=avail /home | tail -1 | tr -dc 0-9); [ "$FREE" -ge 25 ] || { echo "only ${FREE} GB free on /home"; exit 1; }
docker network inspect vm2b-domnet >/dev/null 2>&1 || docker network create vm2b-domnet >/dev/null
S=$D/shared-$ROLE
rm -rf "$S/results" "$S/jobs"; mkdir -p "$D/storage-$ROLE" "$S/jobs/done" "$S/results" "$S/pkg"
cp "$W/shared/poller.bat" "$W/shared/smb-start.sh" "$W/shared/smb-real.py" "$S/"
sed -e "s/@IP@/$IP/g" -e "s/@MAC@/$MAC/g" -e "s/@NAME@/$NAME_AD/g" -e "s/@SHORT@/$SHORT/g" "$D/bootstrap.cmd.in" > "$S/bootstrap.cmd"
rm -rf "$D/ans-$ROLE"; mkdir -p "$D/ans-$ROLE"
sed "s/@NAME@/$NAME_AD/g" "$D/autounattend.xml.in" > "$D/ans-$ROLE/autounattend.xml"; cp "$D/vm2bboot.cmd" "$D/ans-$ROLE/"
genisoimage -quiet -J -r -V ANSWER -o "$D/answer-$ROLE.iso" "$D/ans-$ROLE" || exit 1
docker run -d --name $NAME --network vm2b-domnet --device /dev/kvm --stop-timeout 60 -p 127.0.0.1:$VNC:5900 \
  -e ROLE=$ROLE -e VM_NAME=$NAME -e VM_RAM=$RAM -e VM_CPUS=$CPUS \
  -v "$D:/x:ro" -v "$S:/shared" -v "$D/storage-$ROLE:/s" -v "$ISO:/boot.iso:ro" \
  --entrypoint bash dockurr/windows /x/qemu-run.sh
date +%s > "$D/start-$ROLE"
