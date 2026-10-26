#!/bin/sh
# Screenshot of the VM screen -> PNG.  shot.sh [out.png]   (qemu monitor socket inside the container: /tmp/m.sock for x86, /run/shm/monitor.sock for dockur; the screen blanks when idle: a key first)
OUT=${1:-/tmp/vm2b-shot.png}
cd "$(dirname "$0")"
docker exec vm2b-win sh -c 'S=/tmp/m.sock; [ -S $S ] || S=/run/shm/monitor.sock; echo "sendkey shift" | nc -U -q1 $S >/dev/null 2>&1; sleep 0.5; echo "screendump /shared/shot.ppm" | nc -U -q1 $S >/dev/null 2>&1; sleep 1'
if [ -f shared/shot.ppm ]; then convert shared/shot.ppm "$OUT" && rm -f shared/shot.ppm && echo "$OUT"; else echo "no screendump"; exit 1; fi
