#!/bin/bash
# Раунд 12: длинный цикл ftp_fault_injection_upload на ВМ Windows (контейнер vm2b-win уже запущен и готов) под трассировкой.
#   tools/winvm-tlsclose.sh <метка> <минут> [--load N] [--defender on|off] [--bin ПАПКА] [--test ИМЯ] [--no-trace]
# Что делает: поднимает FTP-серверы на хосте (DEBUG-журнал с метками времени, секреты TLS в keys.log), пишет снимок трафика внутри сетевого пространства имён
# КОНТЕЙНЕРА (tcpdump, кольцо 100 x 100 МБ на интерфейсе qemu - стороне гостя, до NAT), запускает на госте тест по кругу с VM2B_STOP_ON_FAIL=1 (цикл останавливается
# на первом падении) и собирает всё в .winvm/tlsclose/<метка>/.  Если падения не было, снимок и ключи удаляются (большие), журналы остаются.
# Процессы останавливаются ТОЛЬКО по PID.  Пароли в командные строки не пишутся (серверы берут их из своих json).
set -u
R=$(cd "$(dirname "$0")/.." && pwd); W=$R/.winvm
TAG=${1:?метка}; MIN=${2:?минут}; shift 2
LOAD=0; BIN=""; TEST=ftp_fault_injection_upload; TRACE=1
while [ $# -gt 0 ]; do case "$1" in --load) LOAD=$2; shift 2;; --bin) BIN=$2; shift 2;; --test) TEST=$2; shift 2;; --no-trace) TRACE=0; shift;; *) echo "неизвестный аргумент $1"; exit 2;; esac; done
OUT=$W/tlsclose/$TAG; rm -rf "$OUT"; mkdir -p "$OUT"
RES=$W/shared/results; GS='\\host.lan\Data'; export VM2B_GUEST_SHARE=$GS
[ -n "$BIN" ] && cp "$BIN"/vm2bautotest.exe "$W/shared/pkg/bin/vm2bautotest.exe"
mkdir -p "$W/shared/pkg/guest"; cp "$W"/guest/* "$W/shared/pkg/guest/"
SEED=$(( (RANDOM << 15 | RANDOM) % 1000000000 )); echo "seed $SEED" > "$OUT/seed.txt"
KL=""; [ $TRACE = 1 ] && { KL=$OUT/keys.log; : > "$KL"; }
VM2B_FTP_LOGLEVEL=DEBUG VM2B_FTP_KEYLOG=$KL VM2B_SEED=$SEED "$R/tools/winvm-servers.sh" start ftp > "$OUT/servers-start.txt" 2>&1 || { cat "$OUT/servers-start.txt"; exit 1; }
CPID=$(docker inspect -f '{{.State.Pid}}' vm2b-win)
TPID=""
if [ $TRACE = 1 ]; then
  nsenter -t "$CPID" -n tcpdump -Z root -i qemu -n -s 0 -C 100 -W 100 -w "$OUT/cap.pcap" 'tcp portrange 21000-21999 or tcp port 21' > "$OUT/tcpdump.log" 2>&1 &
  TPID=$!; sleep 2
fi
LINES=("set TAG=tc-$TAG" "set ARGS=--seed $SEED --only $TEST --minutes $MIN --servers $GS\\srv\\ftp\\servers.json" "set VM2B_WEB_DEFAULT=0" "set VM2B_STOP_ON_FAIL=1")
[ "$LOAD" -gt 0 ] && LINES+=("powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vm2b\\guest\\cpuload.ps1 $LOAD $(( MIN * 60 + 600 ))")
LINES+=('call C:\vm2b\guest\autotest.cmd')
T0=$(date +%s)
printf '%s\r\n' "@echo off" "${LINES[@]}" | "$W/job.sh" "tc-$TAG" - $(( MIN * 60 + 1800 )) > "$OUT/job.out" 2>&1
RC=$?
[ -n "$TPID" ] && { kill "$TPID" 2>/dev/null; sleep 1; kill -9 "$TPID" 2>/dev/null; wait "$TPID" 2>/dev/null; }
"$R/tools/winvm-servers.sh" stop-all > /dev/null
L=$RES/autotest-tc-$TAG.txt; tr -d '\r' < "$L" > "$OUT/autotest.txt" 2>/dev/null
P=$(grep -ac '^PASS' "$OUT/autotest.txt"); F=$(grep -ac '^FAIL' "$OUT/autotest.txt")
cp "$W"/shared/srv/ftp/*.log "$OUT/" 2>/dev/null
U=$(cat "$OUT/ftpes.log" "$OUT/ftps.log" 2>/dev/null | grep -ac "closed WITHOUT")
echo "метка=$TAG минут=$MIN load=$LOAD запусков PASS=$P FAIL=$F каналов-без-close_notify=$U rc=$RC время=$(( $(date +%s) - T0 ))с" | tee "$OUT/summary.txt"
grep -aA8 '^FAIL' "$OUT/autotest.txt" | head -20 | tee -a "$OUT/summary.txt"
if [ "$F" = 0 ] && [ "${KEEP_TRACE:-0}" != 1 ]; then rm -f "$OUT"/cap.pcap* "$OUT/keys.log"; else echo "ПАДЕНИЕ: снимок и ключи оставлены в $OUT"; fi
