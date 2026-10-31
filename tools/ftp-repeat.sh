#!/bin/bash
# Повторяет один тест группы ftp N раз подряд с DEBUG-журналом FTP-сервера (разбор нестабильных тестов, например ftp_large_streamed).
#   tools/ftp-repeat.sh <тест> <N> <каталог журналов> [первое зерно]
# Для каждого прогона: свои серверы (launch.py), журнал серверов в <каталог>/run-<i>/, вывод автотеста в <каталог>/run-<i>.txt.  Серверы гасятся по PID.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$1; N=$2; OUT=$3; S0=${4:-100}
PY=$ROOT/.venv/bin/python; [ -x "$PY" ] || PY=python3
mkdir -p "$OUT"; export WINEDEBUG=-all VM2B_FTP_LOGLEVEL=${VM2B_FTP_LOGLEVEL:-DEBUG}
fail=0
for i in $(seq 1 "$N"); do
  RUN=$OUT/run-$i; rm -rf "$RUN"; mkdir -p "$RUN"; : > "$RUN/pids"
  "$PY" "$ROOT/tools/testservers/launch.py" "$RUN" "$RUN/servers.json" "$RUN/pids" $((S0 + i)) > "$RUN/launch.txt" 2>&1
  wine64 "${EXE:-$ROOT/build/x64/vm2bautotest.exe}" --servers "$RUN/servers.json" --only "$T" --seed $((S0 + i)) > "$OUT/run-$i.txt" 2>&1
  for p in $(cat "$RUN/pids"); do kill "$p" 2>/dev/null; done
  rm -rf "$RUN/certs"
  if grep -aq '^FAIL' "$OUT/run-$i.txt"; then fail=$((fail + 1)); echo "run $i: FAIL"; else echo "run $i: ok $(grep -a '^PASS' "$OUT/run-$i.txt" | awk '{print $4}')"; fi
done
echo "failures: $fail of $N"
