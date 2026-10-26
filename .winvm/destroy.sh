#!/bin/bash
# destroy.sh [os]: copies the results (shared/results, screenshots, job outputs) to /home/symo/Claude/VM2Backup/.winvm-results/<os>-<timestamp>/ and then
# removes the container vm2b-win AND the storage dir (absolute paths!) and the custom ISOs.  Run it after EVERY test run, whatever the result.
W=/home/symo/Claude/VM2Backup/.winvm
# файл .winvm/hold: ничего не удалять (нужен, чтобы дочинить / перепроверить фазы на той же ВМ; потом файл убрать и запустить destroy.sh ещё раз)
[ -f "$(dirname "$0")/hold" ] && { echo "destroy.sh: the file .winvm/hold exists: the VM is NOT deleted yet"; exit 0; }
OS=${1:-$(cat $W/current-os 2>/dev/null || echo unknown)}
D=/home/symo/Claude/VM2Backup/.winvm-results/$OS-$(date +%Y%m%d-%H%M%S)
mkdir -p "$D"
[ -d $W/shared/results ] && cp -a $W/shared/results "$D/results"
[ -d $W/shot ] && cp -a $W/shot "$D/shot"
[ -d $W/shared/jobs/done ] && cp -a $W/shared/jobs/done "$D/jobs"
docker logs vm2b-win > "$D/container.log" 2>&1
docker exec vm2b-win sh -c 'cat /tmp/smbreal/log.smbd 2>/dev/null' > "$D/smbreal.log" 2>/dev/null
docker stop -t 30 vm2b-win >/dev/null 2>&1
docker rm -f vm2b-win >/dev/null 2>&1
rm -rf "$W/storage-x86" "$W/storage-w10" "$W/storage-w11" "$W/x86/answer.iso" "$W/shared/results" "$W/shared/jobs" "$W/shared/pkg" "$W/shared/srv" "$W/shot" "$W/shared/shot.ppm" "$W/current-os"
echo "results: $D"
docker ps -a --format '{{.Names}} {{.Status}}' | grep -x 'vm2b-win.*' || echo "container vm2b-win: gone"
ls -d $W/storage-* 2>/dev/null || echo "storage dirs: gone"
df -h /home | tail -1
