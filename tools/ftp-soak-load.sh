#!/bin/bash
# Раунд 11: длительный прогон ftp_fault_injection_upload (или другого теста) несколькими копиями параллельно под искусственной нагрузкой процессора,
# чтобы сдвинуть тайминги (на Windows 10 / qemu NAT сбой «TLS-канал закрыт без close_notify» шёл около 5 % запусков).
#   tools/ftp-soak-load.sh <минуты> <копий> <потоков нагрузки> <каталог журналов> [тест] [первое зерно] [уровень журнала]
# У каждой копии свои серверы (launch.py, свой диапазон портов), журналы - <каталог>/inst-N/.  Всё гасится по PID (никаких pkill).
# В конце печатает: число запусков теста, падений и случаев «канал данных загрузки закрыт БЕЗ close_notify клиента» из журналов серверов.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MIN=${1:-10}; N=${2:-3}; LOAD=${3:-0}; OUT=${4:?каталог}; T=${5:-ftp_fault_injection_upload}; S0=${6:-500}; LVL=${7:-INFO}
PY=$ROOT/.venv/bin/python; [ -x "$PY" ] || PY=python3
mkdir -p "$OUT"; export WINEDEBUG=-all VM2B_FTP_LOGLEVEL=$LVL
PIDS=(); HOGS=()
for i in $(seq 1 "$LOAD"); do ( while :; do :; done ) & HOGS+=($!); done
for i in $(seq 1 "$N"); do
  RUN=$OUT/inst-$i; rm -rf "$RUN"; mkdir -p "$RUN"; : > "$RUN/pids"
  "$PY" "$ROOT/tools/testservers/launch.py" "$RUN" "$RUN/servers.json" "$RUN/pids" $((S0 + i * 7)) > "$RUN/launch.txt" 2>&1
  wine64 "${EXE:-$ROOT/build/x64/vm2bautotest.exe}" --servers "$RUN/servers.json" --only "$T" --seed $((S0 + i)) --minutes "$MIN" > "$OUT/inst-$i.txt" 2>&1 &
  PIDS+=($!)
done
wait "${PIDS[@]}"
for h in "${HOGS[@]}"; do kill "$h" 2>/dev/null; done
for i in $(seq 1 "$N"); do for p in $(cat "$OUT/inst-$i/pids"); do kill "$p" 2>/dev/null; done; rm -rf "$OUT/inst-$i/certs"; done
runs=$(cat "$OUT"/inst-*.txt | grep -a -c '^PASS\|^FAIL'); fails=$(cat "$OUT"/inst-*.txt | grep -a -c '^FAIL')
nocn=$(cat "$OUT"/inst-*/ftp*.log 2>/dev/null | grep -a -c 'closed WITHOUT')
echo "запусков: $runs, падений: $fails, каналов данных загрузки без close_notify: $nocn"
