#!/bin/bash
# destroy-dom.sh [метка]: копирует результаты ОБЕИХ ВМ в .winvm-results/domain-<метка>/ и удаляет контейнеры vm2b-dc, vm2b-member, диски, общие папки,
# сеть vm2b-domnet и ответные образы (абсолютные пути!). Запускать после КАЖДОГО прогона при любом результате. Файл .winvm/hold - ничего не удалять.
D=/home/symo/Claude/VM2Backup/.winvm/dom
[ -f "$D/../hold" ] && { echo "destroy-dom.sh: файл .winvm/hold есть: ВМ НЕ удалены"; exit 0; }
L=${1:-$(date +%Y%m%d-%H%M%S)}; O=/home/symo/Claude/VM2Backup/.winvm-results/domain-$L
mkdir -p "$O"
for r in dc member; do
  [ -d $D/shared-$r/results ] && cp -a $D/shared-$r/results "$O/results-$r"
  [ -d $D/shared-$r/jobs/done ] && cp -a $D/shared-$r/jobs/done "$O/jobs-$r"
  docker logs vm2b-$r > "$O/container-$r.log" 2>&1
  docker stop -t 30 vm2b-$r >/dev/null 2>&1; docker rm -f vm2b-$r >/dev/null 2>&1
  rm -rf "$D/storage-$r" "$D/shared-$r" "$D/answer-$r.iso" "$D/ans-$r" "$D/start-$r"
done
docker network rm vm2b-domnet >/dev/null 2>&1
echo "results: $O"
docker ps -a --format '{{.Names}} {{.Status}}' | grep -E '^vm2b-' || echo "containers vm2b-dc / vm2b-member: gone"
ls -d $D/storage-* 2>/dev/null || echo "storage dirs: gone"
df -h /home | tail -1
