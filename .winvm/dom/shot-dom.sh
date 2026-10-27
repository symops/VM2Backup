#!/bin/sh
# shot-dom.sh <dc|member> [out.png]: снимок экрана ВМ через монитор qemu (экран гаснет при простое: сначала клавиша)
R=$1; OUT=${2:-/tmp/vm2b-$1.png}; D=/home/symo/Claude/VM2Backup/.winvm/dom; C=vm2b-$R;
docker exec $C sh -c 'S=/tmp/m.sock; echo "sendkey shift" | nc -U -q1 $S >/dev/null 2>&1; sleep 0.5; echo "screendump /shared/shot.ppm" | nc -U -q1 $S >/dev/null 2>&1; sleep 1'
if [ -f $D/shared-$R/shot.ppm ]; then convert $D/shared-$R/shot.ppm "$OUT" && rm -f $D/shared-$R/shot.ppm && echo "$OUT"; else echo "no screendump"; exit 1; fi
