#!/bin/bash
# Холодный бэкап на НАСТОЯЩЕМ ESXi 7: этапы (tests_esxi_real.cpp, esxireal_cold_stage_*) чередуются с проверками ВМ по SSH мимо продукта.
#   tools/esxi7/cold-scenario.sh [main cancel crash unlock notools limits]       (по умолчанию все)
# Нужно: ESXi запущен, setup-guest.sh и `GUEST_MODE=tools guest-setup.sh` выполнены (ВМ vm2b-guest = Alpine с open-vm-tools, пишет на диск), wine64, sshpass.
# Результат: .winvm-results/esxi7-cold-<дата>/ (вывод тестов и факты по SSH).  Пароль нигде не печатается.
D=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$D/../.." && pwd); E=$ROOT/.winvm/esxi; S=$D/ssh.sh
EXE=${VM2B_EXE:-$ROOT/build/x64/vm2bautotest.exe}
RES=${RES:-$ROOT/.winvm-results/esxi7-cold-$(date +%Y%m%d)}; mkdir -p "$RES" "$E/cold"
DS=/vmfs/volumes/datastore1; G=vm2b-guest
export WINEDEBUG=${WINEDEBUG:--all}
W() { echo "Z:$1" | sed 's#/#\\#g'; }
cfg() { # $1 force_off(0/1)  $2 ref ("" = не сверять)  $3 restore_name
  FP=$(echo | openssl s_client -connect 10.213.77.10:443 2>/dev/null | openssl x509 -noout -fingerprint -sha256 | sed 's/.*=//; s/://g' | tr A-F a-f)
  python3 - "$E" "$FP" "$1" "$2" "${3:-vm2b-cold-rt}" <<'PY'
import json, sys, os
e, fp, force, ref, rn = sys.argv[1:6]
w = lambda p: "Z:" + p.replace('/', '\\')
cfg = {"esxi_real": {"host": "10.213.77.10", "port": 443, "user": "root", "password_file": w(e + "/secret"), "fingerprint": "sha256:" + fp, "vms": [], "restore_prefix": "vm2b-rt"},
       "esxi_cold": {"vm": os.environ.get("COLD_VM", "vm2b-guest"), "store": w(e + "/cold/store"), "restore": w(e + "/cold/restore"), "ref": (w(ref) if ref else ""), "force_off": force == "1", "shutdown_timeout": 1500, "pause_mb": 20, "restore_name": rn}}
json.dump(cfg, open(e + "/servers.json", "w")); os.chmod(e + "/servers.json", 0o600)
PY
}
stage() { echo "=== этап $1" | tee -a "$RES/cold.txt"; wine64 "$EXE" --servers "$E/servers.json" --only "$1" 2>&1 | grep -E "этап|после|перед|простой|sha256|состояние|возобнов|список|PASS|FAIL|check|простой" | cut -c1-400 | tee -a "$RES/cold.txt"; }
fact() { echo "--- $1" | tee -a "$RES/cold.txt"; }
id() { $S "vim-cmd vmsvc/getallvms | grep ' $1 ' | cut -d' ' -f1" | tail -1; }
vmstate() { # $1 имя
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $1 ' | cut -d' ' -f1); echo \"power: \$(vim-cmd vmsvc/power.getstate \$I | tail -1)\"; echo \"vmx: \$(grep -c 'vm2backup.hold' $DS/$1/$1.vmx) hold-lines, annotation: \$(grep '^annotation' $DS/$1/$1.vmx | cut -c1-160)\"; echo \"снимки VM2Backup: \$(vim-cmd vmsvc/snapshot.get \$I | grep -c 'Name *: VM2Backup-')  папки VM2Backup-*: \$(ls -d $DS/VM2Backup-* $DS/$1/VM2Backup-* 2>/dev/null | wc -l)\"; echo \"tools: \$(vim-cmd vmsvc/get.guest \$I | grep -E 'toolsRunningStatus' | head -1)\"" 2>&1 | tee -a "$RES/cold.txt"
}
ensure_guest() { # гость включён, Tools работают
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' $G ' | cut -d' ' -f1); [ \"\$(vim-cmd vmsvc/power.getstate \$I | tail -1)\" = 'Powered on' ] || vim-cmd vmsvc/power.on \$I >/dev/null 2>&1; rm -rf $DS/VM2Backup-* ; true"
  for i in $(seq 1 240); do $S "vim-cmd vmsvc/get.guest \$(vim-cmd vmsvc/getallvms | grep ' $G ' | cut -d' ' -f1) | grep -q 'toolsRunningStatus = \"guestToolsRunning\"'" && return 0; sleep 10; done
  echo "Tools в госте не запустились"; return 1
}
ensure_on() { $S "I=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-on ' | cut -d' ' -f1); vim-cmd vmsvc/snapshot.removeall \$I >/dev/null 2>&1; [ \"\$(vim-cmd vmsvc/power.getstate \$I | tail -1)\" = 'Powered on' ] || vim-cmd vmsvc/power.on \$I >/dev/null 2>&1; sleep 4; true"; }
clean() { rm -rf "$E/cold/store" "$E/cold/restore"; }
# отладка логики без загрузки гостя: COLD_VM=vm2b-off cold-scenario.sh offvm  (VM_TRACE=<папка> пишет SOAP-обмен продукта)
STAGES=${*:-main cancel crash unlock notools limits}
for ST in $STAGES; do
case $ST in
main)
  fact "ГЛАВНЫЙ СЦЕНАРИЙ: холодный бэкап по умолчанию, пауза, эталон при выключенной ВМ, продолжение, восстановление на хост"
  ensure_guest || exit 1; clean; cfg 0 "$E/cold/ref"
  vmstate $G
  stage esxireal_cold_stage_pause
  fact "ВМ при паузе (должна быть выключена, помечена)"; vmstate $G
  fact "sha256 диска на хосте при выключенной ВМ (эталон)"
  $S "sha256sum $DS/$G/$G-flat.vmdk | cut -d' ' -f1" | tail -1 > "$E/cold/ref"; cat "$E/cold/ref" | tee -a "$RES/cold.txt"
  stage esxireal_cold_stage_resume
  fact "ВМ после копии (включена, метка снята, гость загрузился)"; vmstate $G
  $S "sleep 60; grep -c 'BOOT tools' $DS/vm2b-serial/guest.log; tail -2 $DS/vm2b-serial/guest.log" | tee -a "$RES/cold.txt"
  stage esxireal_cold_stage_restore_host
  fact "восстановленная ВМ vm2b-cold-rt: питание и sha256 диска"
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-cold-rt ' | cut -d' ' -f1); vim-cmd vmsvc/power.getstate \$I | tail -1; sha256sum $DS/vm2b-cold-rt/*-flat.vmdk | cut -d' ' -f1" | tee -a "$RES/cold.txt"
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-cold-rt ' | cut -d' ' -f1); vim-cmd vmsvc/power.off \$I >/dev/null 2>&1; vim-cmd vmsvc/unregister \$I; rm -rf $DS/vm2b-cold-rt" >/dev/null 2>&1
  clean ;;
cancel)
  fact "ОТМЕНА пользователем: пауза, затем явная отмена"
  ensure_guest || exit 1; clean; cfg 0 ""
  stage esxireal_cold_stage_cancel; fact "после отмены (ВМ включена, метка снята)"; vmstate $G; clean ;;
crash)
  fact "СЛУЖБА ПОГИБЛА посреди копии: журнала нет, метка на ВМ осталась"
  ensure_guest || exit 1; clean; cfg 0 ""
  stage esxireal_cold_stage_crash; vmstate $G
  stage esxireal_cold_stage_after_crash; fact "после следующего запуска"; vmstate $G; clean ;;
offvm)
  fact "ВЫКЛЮЧЕННАЯ ВМ ($COLD_VM): удержание без выключения гостя; пауза и продолжение"
  clean; cfg 0 ""
  vmstate ${COLD_VM:-vm2b-off}
  stage esxireal_cold_stage_pause; vmstate ${COLD_VM:-vm2b-off}
  stage esxireal_cold_stage_resume; vmstate ${COLD_VM:-vm2b-off}; clean ;;
offrestore)
  fact "ВЫКЛЮЧЕННАЯ ВМ: полная холодная копия, восстановление на ХОСТ, sha256 диска сверен с эталоном при выключенной ВМ"
  COLD_VM=vm2b-off; export COLD_VM; clean; $S "sha256sum $DS/vm2b-off/vm2b-off-flat.vmdk | cut -d' ' -f1" | tail -1 > "$E/cold/ref" 2>/dev/null || { mkdir -p "$E/cold"; $S "sha256sum $DS/vm2b-off/vm2b-off-flat.vmdk | cut -d' ' -f1" | tail -1 > "$E/cold/ref"; }
  cfg 0 "$E/cold/ref"; $S "vim-cmd vmsvc/power.off \$(vim-cmd vmsvc/getallvms | grep ' vm2b-cold-rt ' | cut -d' ' -f1) >/dev/null 2>&1; rm -rf $DS/vm2b-cold-rt" >/dev/null 2>&1
  stage esxireal_cold_stage_pause; stage esxireal_cold_stage_resume
  stage esxireal_cold_stage_restore_host
  fact "восстановленная ВМ vm2b-cold-rt"
  $S "ls $DS/vm2b-cold-rt | head; sha256sum $DS/vm2b-cold-rt/vm2b-off-flat.vmdk | cut -d' ' -f1; cat /dev/null" | tee -a "$RES/cold.txt"; cat "$E/cold/ref" | tee -a "$RES/cold.txt"
  $S "I=\$(vim-cmd vmsvc/getallvms | grep ' vm2b-cold-rt ' | cut -d' ' -f1); vim-cmd vmsvc/power.getstate \$I | tail -1; vim-cmd vmsvc/power.off \$I >/dev/null 2>&1; vim-cmd vmsvc/unregister \$I; rm -rf $DS/vm2b-cold-rt" 2>&1 | tee -a "$RES/cold.txt"; clean; unset COLD_VM ;;
aftercrash)
  fact "СЛЕДУЮЩИЙ ЗАПУСК после гибели/сбоя: ВМ выключена, на ней осталась метка, журнала нет (состояние как после scenario crash)"
  clean; cfg 0 ""
  vmstate $G
  stage esxireal_cold_stage_after_crash; fact "после следующего запуска (ВМ включена, метка снята)"; vmstate $G; clean ;;
unlock)
  fact "esxi.unlock_vm: администратор возвращает ВМ"
  ensure_guest || exit 1; clean; cfg 0 ""
  stage esxireal_cold_stage_pause; vmstate $G
  stage esxireal_cold_stage_unlock; vmstate $G; clean ;;
notools)
  fact "ВМ БЕЗ VMware Tools (vm2b-on): отказ; затем с force_off"
  ensure_on; clean; cfg 0 ""
  stage esxireal_cold_stage_no_tools; vmstate vm2b-on
  ensure_on; cfg 1 ""
  stage esxireal_cold_stage_no_tools; vmstate vm2b-on; clean ;;
limits)
  fact "ПРЕДЕЛЫ: что можно сделать с удерживаемой ВМ мимо продукта на ОТДЕЛЬНОМ хосте"
  ensure_guest || exit 1; clean; cfg 0 ""
  stage esxireal_cold_stage_pause; vmstate $G
  fact "DisableMethods / EnableMethods / QueryDisabledMethods на этом хосте"
  python3 -I "$D/probe-lock.py" "$(id $G)" lock 2>&1 | grep -o 'faultstring>[^<]*' | tee -a "$RES/cold.txt"
  python3 -I "$D/probe-lock.py" "$(id $G)" list 2>&1 | grep -o 'faultstring>[^<]*' | tee -a "$RES/cold.txt"
  fact "чужой API-вызов ReconfigVM_Task (root, по SOAP) пока ВМ удерживается"
  python3 -I "$D/probe-lock.py" "$(id $G)" foreign ReconfigVM_Task 2>&1 | grep -o 'ReconfigVM_TaskResponse\|faultstring>[^<]*' | head -2 | tee -a "$RES/cold.txt"
  fact "vim-cmd vmsvc/power.on из SSH под удержанием"
  $S "vim-cmd vmsvc/power.on \$(vim-cmd vmsvc/getallvms | grep ' $G ' | cut -d' ' -f1) 2>&1 | tail -3" | tee -a "$RES/cold.txt"; sleep 8; vmstate $G
  fact "продолжение: ВМ включили мимо продукта -> копия не продолжается по устаревшему состоянию, а начинается заново (ВМ выключается снова)"
  stage esxireal_cold_stage_resume; vmstate $G; clean ;;
esac
done
