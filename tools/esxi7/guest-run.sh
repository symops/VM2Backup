#!/bin/bash
# Проверка на настоящем ESXi 7: гость ПИШЕТ на диск во время копии включённой ВМ (docs/ESXI.md, «Запись гостя во время копии»).
#   tools/esxi7/guest-run.sh copy        2 режима копии (xz1; пароль + xz5 + тома 100 МБ) идут минуты, пока гость пишет; восстановление в папку, проверка «префикса»
#   tools/esxi7/guest-run.sh host        копия + восстановление на ХОСТ ESXi; восстановленная ВМ загружается с проверяющего ISO и печатает «VERIFY OK seq<=S»
#   tools/esxi7/guest-run.sh scenario    пауза посреди копии -> (A) продолжение при работающей ВМ, (B) ВМ выключена и включена -> продолжение ПО ТОМУ ЖЕ снимку,
#                                        (D) ВМ выключена к моменту продолжения (диск читается из клона, ВМ потом включается), (E) три цикла выключить/включить,
#                                        (F) негативный контроль: снимок пересоздан -> копия начинается заново, (G) негативный контроль: замороженный клон изменён -> заново
#   tools/esxi7/guest-run.sh all    (SCEN="A B C D E F G" выбирает этапы сценария; C = ВМ выключается посреди передачи копии)
# Нужно: ESXi запущен (vm.sh start), setup-guest.sh и guest-setup.sh выполнены (ВМ vm2b-guest включена, serial показывает «RUN start»), wine64, sshpass, numpy.
# Результат: .winvm-results/esxi7-guest-<дата>/ (сырые журналы: наблюдатель хоста, serial гостя, вывод тестов; сводка analyse.py).  Пароль нигде не печатается.
set -e
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi; S=$D/ssh.sh
EXE=${VM2B_EXE:-$ROOT/build/x64/vm2bautotest.exe}
RES=${RES:-$ROOT/.winvm-results/esxi7-guest-$(date +%Y%m%d)}; mkdir -p "$RES"
OUT=$E/guest-out; mkdir -p "$OUT"
MBIT=${MBIT:-10}
DS=/vmfs/volumes/datastore1; VMDIR=$DS/vm2b-guest; SER=$DS/vm2b-serial/guest.log
export WINEDEBUG=${WINEDEBUG:--all}
cfg() { # $1 tag, $2 mbit, $3 pause_mb, $4 restore_name
  FP=$(echo | openssl s_client -connect 10.213.77.10:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr A-F a-f)
  python3 - "$E" "$FP" "$1" "$2" "${3:-100}" "${4:-}" <<'PY'
import json, sys, os
e, fp, tag, mbit, pmb, rn = sys.argv[1:7]
w = lambda p: "Z:" + p.replace('/', '\\')
cfg = {"esxi_real": {"host": "10.213.77.10", "port": 443, "user": "root", "password_file": w(e + "/secret"), "fingerprint": "sha256:" + fp, "vms": [], "restore_prefix": "vm2b-rt"},
       "esxi_guest": {"vm": "vm2b-guest", "out": w(e + "/guest-out"), "tag": tag, "mbit": int(mbit), "pause_mb": int(pmb), "restore_name": rn, "expect_resume": os.environ.get("EXPECT_RESUME", "1") == "1"}}
json.dump(cfg, open(e + "/servers.json", "w")); import os; os.chmod(e + "/servers.json", 0o600)
PY
}
watch_start() { # $1 имя журнала
  $S 'cat > /tmp/esxi-watch.py' < "$D/guest/esxi-watch.py"
  $S "rm -f /tmp/vm2b-watch.log; nohup python3 /tmp/esxi-watch.py $VMDIR $SER /tmp/vm2b-watch.log >/dev/null 2>&1 & echo \$! > /tmp/vm2b-watch.pid"
}
watch_stop() { # $1 куда сохранить
  $S 'kill $(cat /tmp/vm2b-watch.pid) 2>/dev/null; sleep 1; cat /tmp/vm2b-watch.log' > "$1"
}
host_state() { $S "date +%s; ls -la $VMDIR | grep -E 'vmdk|vmsd|VM2Backup'; ls -d $DS/VM2Backup-* $VMDIR/VM2Backup-* 2>/dev/null; vim-cmd vmsvc/snapshot.get \$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1) | head -5; echo 'остатков VM2Backup-*:' \$(ls -d $DS/VM2Backup-* $VMDIR/VM2Backup-* 2>/dev/null | wc -l)"; }
run_test() { # $1 тест
  wine64 "$EXE" --servers "$E/servers.json" --only "$1" 2>&1 | tee -a "$RES/autotest-guest.txt"
}
need_running() {
  $S "grep -c '^R seq' $SER" | grep -q -v '^0$' || { echo "гость не пишет (нет «R seq» в serial)"; exit 1; }
}
analyse() { # $1 тег  $2 образ  $3 журнал хоста  $4 serial  [$5 доп. параметры analyse.py]
  python3 "$D/guest/analyse.py" "$RES" "$1" "$2" --watch "$3" --serial "$4" --autotest "$RES/autotest-guest.txt" --json "$RES/analysis-$1.json" $5 > "$RES/analysis-$1.line" 2>> "$RES/analysis-errors.txt" || true
  python3 -c "
import json,sys
d=json.load(open('$RES/analysis-$1.json'))
keys=['tag','verdict','M','blocks','ok','zero','torn','stale','inflight','violations','stun','M_vs_stun','clock_window_2s','clock_window_4s','M_write_time_vs_snapshot_s','delta_bytes_at_snapshot','delta_bytes_max','guest_seq_at_copy_end','writes_after_snapshot','blocks_differ_from_state_at_copy_end','copy_seconds']
print('ANALYSIS', ' '.join('%s=%s'%(k,d.get(k)) for k in keys))" | tee -a "$RES/analysis-all.txt"
}
cmd_copy() {
  need_running; cfg copy "$MBIT"
  echo "=== host before: $(date +%s)" | tee -a "$RES/host-state.txt"; host_state | tee -a "$RES/host-state.txt"
  watch_start; run_test esxireal_guest_write_copy; watch_stop "$RES/watch-copy.log"
  $S "cat $SER" > "$RES/serial-copy.log"
  echo "=== host after" | tee -a "$RES/host-state.txt"; host_state | tee -a "$RES/host-state.txt"
  for t in xz1 pw-xz5-vol100; do analyse $t "$OUT/$t/vm2b-guest/vm2b-guest-flat.vmdk" "$RES/watch-copy.log" "$RES/serial-copy.log"; done
}
cmd_host() {
  need_running; cfg host "$MBIT" 100 vm2b-rt-guest
  $S "for id in \$(vim-cmd vmsvc/getallvms | grep ' vm2b-rt-guest ' | cut -d' ' -f1); do vim-cmd vmsvc/power.off \$id >/dev/null 2>&1; vim-cmd vmsvc/unregister \$id >/dev/null 2>&1; done; rm -rf $DS/vm2b-rt-guest $DS/vm2b-serial/restored.log"
  watch_start; run_test esxireal_guest_restore_to_host; watch_stop "$RES/watch-host.log"; $S "cat $SER" > "$RES/serial-host.log"
  analyse tohost-folder "$OUT/tohost-folder/vm2b-guest/vm2b-guest-flat.vmdk" "$RES/watch-host.log" "$RES/serial-host.log"
  read LO HI < <(python3 -c "
import json; d=json.load(open('$RES/analysis-tohost-folder.json')); s=d.get('stun')
print(s['seq']-2, s['seq']) if s else print(d['clock_window_2s'][0], d['clock_window_2s'][1])")
  echo "окно допустимых M для проверяющего гостя: [$LO, $HI]"
  TMPDIR=${TMPDIR:-/home/symo/tmp-esxi7c} "$D/guest/build-iso.sh" verify "$TMPDIR/alpine-verify.iso" $LO $HI
  $S "cat > $DS/vm2b-iso/alpine-verify.iso" < "$TMPDIR/alpine-verify.iso"
  $S "V=\$(ls -d $DS/vm2b-rt-guest*/ | head -1); X=\$(ls \$V*.vmx | head -1); sed -i 's#alpine-writer.iso#alpine-verify.iso#; s#vm2b-serial/guest.log#vm2b-serial/restored.log#' \$X; grep -E 'ide0:0.fileName|serial0.fileName|scsi0:0.fileName' \$X; ls -la \$V; ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-rt-guest ' | cut -d' ' -f1); vim-cmd vmsvc/reload \$ID; vim-cmd vmsvc/power.on \$ID" | tee -a "$RES/host-restored.txt"
  echo "ждём загрузку восстановленной ВМ и её проверку диска (до 40 мин: во вложенном ESXi загрузка 9-25 мин)"
  for i in $(seq 1 480); do $S "grep -q 'VERIFY OK\|VERIFY BAD' $DS/vm2b-serial/restored.log 2>/dev/null" && break; sleep 5; done
  $S "cat $DS/vm2b-serial/restored.log" > "$RES/serial-restored.log"; grep -E "BOOT|VERIFY" "$RES/serial-restored.log" | tee -a "$RES/host-restored.txt"
  python3 - "$RES" <<'PY'
import json, re, sys
res = sys.argv[1]; a = json.load(open(res + '/analysis-tohost-folder.json'))
t = open(res + '/serial-restored.log', errors='replace').read()
m = re.search(r'VERIFYRESULT M=(\d+) .*violations=(\d+)', t); ok = 'VERIFY OK seq<=S' in t
print('ВОССТАНОВЛЕННАЯ ВМ: гость сообщил M=%s violations=%s, M копии в папке=%s, «VERIFY OK seq<=S»: %s -> %s' % (m and m.group(1), m and m.group(2), a['M'], ok, 'OK' if ok and m and int(m.group(1)) == a['M'] else 'BAD'))
PY
}
wait_writing() { # гость пишет: число строк «R seq» в serial РАСТЁТ за 12 с (serial при включении ДОПИСЫВАЕТСЯ, старые строки остаются; при включении ВМ файл «меняется», пока гость ещё грузится, и BOOT/RUN прежней загрузки на месте: по ним и по времени файла загрузку не отличить)
  for i in $(seq 1 400); do
    n1=$($S "grep -c '^R seq' $SER" 2>/dev/null) || { sleep 6; continue; }
    sleep 12
    n2=$($S "grep -c '^R seq' $SER" 2>/dev/null) || continue
    [ -n "$n1" ] && [ -n "$n2" ] && [ "$n2" -gt "$n1" ] && $S "[ \$(grep -c '^BOOT' $SER) -eq \$(grep -c '^RUN start' $SER) ]" && return 0
  done; return 1
}
cmd_scenario() {
  KINDS=${SCEN:-"A B C"}
  for kind in $KINDS; do
    wait_writing || { echo "гость не пишет"; exit 1; }
    case $kind in
    A|B|D|E|F|G)
      case $kind in F|G) export EXPECT_RESUME=0 ;; *) export EXPECT_RESUME=1 ;; esac
      cfg pause$kind "${MBIT_S:-20}" 100
      watch_start; run_test esxireal_guest_stage_pause
      $S "cat $SER" > "$RES/serial-pause$kind-before.log"; host_state | tee -a "$RES/host-state.txt"
      case $kind in
      A) echo "--- A: ВМ работает, копия на паузе 40 с"; sleep 40 ;;
      B)
        echo "--- B: ВМ выключается посреди паузы, 20 с, включается (загрузка идёт, пока копия продолжается): .vmx переписан хостом, продолжение обязано идти по ПЕРВОМУ снимку"
        $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$ID | tail -1; sleep 3" ; host_state | tee -a "$RES/host-state.txt"; sleep 20
        $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.on \$ID | tail -1" ;;
      D)
        echo "--- D: ВМ выключается и ОСТАЁТСЯ выключенной к моменту продолжения (клон замороженного состояния читается, ВМ не нужна)"
        $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$ID | tail -1; sleep 3; ls -la $VMDIR | grep -E 'vmx|nvram'" | tee -a "$RES/host-state.txt"; sleep 15 ;;
      E)
        echo "--- E: три цикла выключить/включить посреди паузы"
        for i in 1 2 3; do
          $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$ID | tail -1; sleep 2; ls -la $VMDIR | grep -E 'vmx\$|nvram'; vim-cmd vmsvc/power.on \$ID | tail -1; sleep 8" | tee -a "$RES/host-state.txt"
        done ;;
      F)
        echo "--- F (негативный контроль): снимок VM2Backup-* удалён и создан новый -> продолжать нельзя"
        $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/snapshot.removeall \$ID | tail -1; vim-cmd vmsvc/snapshot.create \$ID VM2Backup-0123456789 negative-control 0 0 | tail -1; vim-cmd vmsvc/snapshot.get \$ID | head -4" | tee -a "$RES/host-state.txt" ;;
      G)
        echo "--- G (негативный контроль): замороженный клон изменён (в конец дописан байт) -> продолжать нельзя"
        $S "cd $VMDIR/VM2Backup-*/ && ls -la && printf x >> d0-flat.vmdk && ls -la" | tee -a "$RES/host-state.txt" ;;
      esac
      run_test esxireal_guest_stage_resume; watch_stop "$RES/watch-pause$kind.log"
      $S "cat $SER" > "$RES/serial-pause$kind.log"; host_state | tee -a "$RES/host-state.txt"
      # копия = состояние диска на момент снимка (первого для A B D E, нового для F G): M = последняя запись гостя до его остановки на снимке
      analyse pause$kind "$OUT/restored-pause$kind/vm2b-guest/vm2b-guest-flat.vmdk" "$RES/watch-pause$kind.log" "$RES/serial-pause$kind.log"
      if [ $kind = D ]; then $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.on \$ID | tail -1"; fi ;;
    C)
      cfg fullC "${MBIT_S:-20}" 100; unset EXPECT_RESUME
      watch_start
      wine64 "$EXE" --servers "$E/servers.json" --only esxireal_guest_stage_full 2>&1 | tee -a "$RES/autotest-guest.txt" > "$RES/stage-full.out" &
      WP=$!
      for i in $(seq 1 200); do $S "ls $VMDIR/VM2Backup-*/ready" >/dev/null 2>&1 && break; sleep 3; done
      sleep 60
      echo "--- C: ВМ выключается (hard power-off) посреди передачи копии, unix=$($S date +%s)"
      $S "ID=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-guest ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$ID | tail -1; sleep 2"; host_state | tee -a "$RES/host-state.txt"
      wait $WP; watch_stop "$RES/watch-fullC.log"; $S "cat $SER" > "$RES/serial-fullC.log"; host_state | tee -a "$RES/host-state.txt"
      analyse fullC "$OUT/restored-fullC/vm2b-guest/vm2b-guest-flat.vmdk" "$RES/watch-fullC.log" "$RES/serial-fullC.log" ;;
    esac
  done
}
case "${1:-copy}" in
copy) cmd_copy ;;
host) cmd_host ;;
scenario) cmd_scenario ;;
rerun-b-c) SCEN="B C" cmd_scenario ;;
rerun) SCEN="${SCEN:-B D E F G}" cmd_scenario ;;
all) cmd_copy; cmd_host; cmd_scenario ;;
*) sed -n 2,8p "$0" ;;
esac
