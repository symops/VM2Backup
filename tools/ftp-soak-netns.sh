#!/bin/bash
# Раунд 11: длительный прогон теста FTP между двумя сетевыми пространствами имён с искусственной задержкой / потерями (tc netem), как приближение к пути
# гость Windows -> qemu NAT -> хост.  Нужны права root (ip netns, tc).  Пространства имён называются vm2b-srv / vm2b-cli и в конце удаляются.
#   tools/ftp-soak-netns.sh <минуты> <копий> <каталог журналов> [тест] [netem-параметры] [первое зерно]
# По умолчанию netem: "delay 2ms 2ms loss 0.05% rate 200mbit".  Серверы (launch.py) работают в vm2b-srv и слушают 10.77.0.1; тест - в vm2b-cli.  Журнал серверов - INFO
# (виден случай «канал данных загрузки закрыт БЕЗ close_notify клиента»).  Всё гасится по PID.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MIN=${1:-8}; N=${2:-2}; OUT=${3:?каталог}; T=${4:-ftp_fault_injection_upload}; NETEM=${5:-delay 2ms 2ms loss 0.05% rate 200mbit}; S0=${6:-1100}
PY=$ROOT/.venv/bin/python; [ -x "$PY" ] || PY=python3
mkdir -p "$OUT"; export WINEDEBUG=-all VM2B_FTP_LOGLEVEL=INFO
# ОТДЕЛЬНЫЙ префикс Wine обязателен: сокеты Wine открывает wineserver, и сервер, запущенный из пространства имён, обслуживал бы потом ВСЕ прогоны Wine
# того же префикса (соединения из других запусков уходили бы в vm2b-cli и получали «Connection refused»).
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-vm2b-ns}
ip netns del vm2b-srv 2>/dev/null; ip netns del vm2b-cli 2>/dev/null
ip netns add vm2b-srv && ip netns add vm2b-cli || exit 1
ip link add vm2bs type veth peer name vm2bc || exit 1
ip link set vm2bs netns vm2b-srv; ip link set vm2bc netns vm2b-cli
ip -n vm2b-srv addr add 10.77.0.1/24 dev vm2bs; ip -n vm2b-cli addr add 10.77.0.2/24 dev vm2bc
for ns in vm2b-srv vm2b-cli; do ip -n $ns link set lo up; done
ip -n vm2b-srv link set vm2bs up; ip -n vm2b-cli link set vm2bc up
ip netns exec vm2b-srv tc qdisc add dev vm2bs root netem $NETEM
ip netns exec vm2b-cli tc qdisc add dev vm2bc root netem $NETEM
PIDS=()
for i in $(seq 1 "$N"); do
  RUN=$OUT/inst-$i; rm -rf "$RUN"; mkdir -p "$RUN"; : > "$RUN/pids"
  VM2B_BIND_ADDR=10.77.0.1 VM2B_SERVER_HOST=10.77.0.1 ip netns exec vm2b-srv "$PY" "$ROOT/tools/testservers/launch.py" "$RUN" "$RUN/servers.json" "$RUN/pids" $((S0 + i * 7)) > "$RUN/launch.txt" 2>&1
  ip netns exec vm2b-cli wine64 "${EXE:-$ROOT/build/x64/vm2bautotest.exe}" --servers "$RUN/servers.json" --only "$T" --seed $((S0 + i)) --minutes "$MIN" > "$OUT/inst-$i.txt" 2>&1 &
  PIDS+=($!)
done
wait "${PIDS[@]}"
for i in $(seq 1 "$N"); do for p in $(cat "$OUT/inst-$i/pids"); do kill "$p" 2>/dev/null; done; rm -rf "$OUT/inst-$i/certs"; done
ip netns del vm2b-srv; ip netns del vm2b-cli
runs=$(cat "$OUT"/inst-*.txt | grep -a -c '^PASS\|^FAIL'); fails=$(cat "$OUT"/inst-*.txt | grep -a -c '^FAIL')
nocn=$(cat "$OUT"/inst-*/ftp*.log 2>/dev/null | grep -a -c 'closed WITHOUT')
echo "запусков: $runs, падений: $fails, каналов данных загрузки без close_notify: $nocn"
