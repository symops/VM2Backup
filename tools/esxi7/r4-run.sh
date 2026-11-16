#!/bin/bash
# Раунд 4: фоновый прогон fs-run.sh и ожидание результата: r4-run.sh <тест> <тег> '<json>' [ожидание, секунд (по умолчанию 100)]; на vCenter: VM2B_REAL_KEY=vcenter_real.
# Вывод - только значимые строки; полный журнал - $RES/<тег>.txt.
D=$(cd "$(dirname "$0")" && pwd); RES=${RES:-$D/../../.winvm-results/esxi7-round4-20261113}; export RES
mkdir -p "$RES"; : > "$RES/$2.done"; rm -f "$RES/$2.done"
( "$D/fs-run.sh" "$1" "$2" "$3" > /dev/null 2>&1; touch "$RES/$2.done" ) &
disown
for i in $(seq 1 ${4:-100}); do [ -f "$RES/$2.done" ] && break; sleep 1; done
grep -a "FSRUN\|OPTIONS\|ошибка\|^PASS\|^FAIL\|предупреждение" "$RES/$2.txt" 2>/dev/null | cut -c1-${COLS:-1800}
[ -f "$RES/$2.done" ] || echo "(ещё идёт: $RES/$2.txt)"
