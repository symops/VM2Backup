#!/bin/bash
# Негативные контроли и сверка с «живым» диском ПОСЛЕ прогона guest-run.sh (копии лежат в .winvm/esxi/guest-out/).
#   tools/esxi7/guest-negative.sh neg      детектор ловит склейку во времени, испорченный бит, обнулённый блок, потерянную запись, ОДНУ запись после снимка (guest/negative.py)
#   tools/esxi7/guest-negative.sh final    ВМ выключается, её настоящий диск (flat, как он стал после всех записей и свёртки снимков) читается по SSH и сравнивается с копиями:
#                                          диск согласован сам (violations = 0), каждая копия — состояние СТАРШЕ него (блоков, отличающихся от него, много), т.е. проверка МОЖЕТ ловить записи после снимка
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi; OUT=$E/guest-out; S=$D/ssh.sh
RES=${RES:-$ROOT/.winvm-results/esxi7-guest-$(date +%Y%m%d)}; mkdir -p "$RES"; T=${TMPDIR:-/home/symo/tmp-esxi7c}
IMG=vm2b-guest/vm2b-guest-flat.vmdk
case "$1" in
neg) python3 "$D/guest/negative.py" "$OUT/xz1/$IMG" "$OUT/pw-xz5-vol100/$IMG" "$T/neg-work" | tee "$RES/negative.txt" ;;
final)
  $S 'ID=$(vim-cmd vmsvc/getallvms | grep " vm2b-guest " | cut -d" " -f1); vim-cmd vmsvc/power.off $ID | tail -1; vim-cmd vmsvc/snapshot.get $ID | head -3; ls -la /vmfs/volumes/datastore1/vm2b-guest'
  $S 'cat /vmfs/volumes/datastore1/vm2b-guest/vm2b-guest-flat.vmdk' > "$OUT/final-live-flat.img"
  python3 "$D/guest/final_compare.py" "$OUT/final-live-flat.img" $(ls -d "$OUT"/*/vm2b-guest/vm2b-guest-flat.vmdk) | tee -a "$RES/final-live.txt"
  rm -f "$OUT/final-live-flat.img" ;;
*) sed -n 2,6p "$0" ;;
esac
