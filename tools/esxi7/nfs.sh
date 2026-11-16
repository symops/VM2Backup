#!/bin/bash
# Раунд 4: общий NFS-датастор для двух вложенных ESXi. Сервер nfs-ganesha (пространство пользователя) в контейнере vm2b-nfs БЕЗ сети Docker:
# контейнеру выдаётся собственный veth (хост-сторона vm2besxi3t в мосту vm2besxi0, адрес 10.213.77.2), доступ только клиентам .10 и .12. Данные: .winvm/nfs-data.
#   tools/esxi7/nfs.sh up | down
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); D=$(cd "$(dirname "$0")" && pwd)
case "$1" in
up)
  docker image inspect vm2b-nfs-img >/dev/null 2>&1 || docker build -q -t vm2b-nfs-img "$D/nfs" >/dev/null
  mkdir -p "$ROOT/.winvm/nfs-data"
  docker rm -f vm2b-nfs >/dev/null 2>&1 || true
  docker run -d --name vm2b-nfs --network none --cap-add SYS_ADMIN --cap-add DAC_READ_SEARCH --security-opt seccomp=unconfined \
    -v "$ROOT/.winvm/nfs-data:/export" -v "$D/nfs/ganesha.conf:/etc/ganesha/ganesha.conf:ro" vm2b-nfs-img sleep infinity >/dev/null
  P=$(docker inspect -f '{{.State.Pid}}' vm2b-nfs)
  ip link show vm2besxi3t >/dev/null 2>&1 || ip link add vm2besxi3t type veth peer name vm2besxi3c
  ip link set vm2besxi3t master vm2besxi0; ip link set vm2besxi3t up
  ip link set vm2besxi3c netns $P name eth0
  nsenter -t $P -n ip addr add 10.213.77.2/24 dev eth0; nsenter -t $P -n ip link set eth0 up; nsenter -t $P -n ip link set lo up
  docker exec -d vm2b-nfs sh -c 'mkdir -p /var/run/ganesha; rpcbind -w; sleep 1; ganesha.nfsd -F -f /etc/ganesha/ganesha.conf -L /tmp/ganesha.log'
  sleep 5; docker exec vm2b-nfs sh -c 'tail -3 /tmp/ganesha.log' ;;
down)
  docker rm -f vm2b-nfs >/dev/null 2>&1 || true
  ip link del vm2besxi3t 2>/dev/null || true
  echo "NFS остановлен (данные .winvm/nfs-data остаются)" ;;
*) sed -n 2,5p "$0" ;;
esac
