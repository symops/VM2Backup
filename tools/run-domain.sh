#!/bin/bash
# VM2Backup на НАСТОЯЩЕМ домене Active Directory (docs/WINDOWS-TESTS.md, "Домен AD").  Режим tools/run-winvm.sh domain [...] вызывает этот скрипт.
#   tools/run-domain.sh [--keep] [--no-create] [--no-build] [--phases a,b,c] [--seed N]
# Поднимает ДВЕ ВМ Windows Server 2022 (контейнеры vm2b-dc и vm2b-member, чистый qemu внутри dockurr/windows; .winvm/dom/), делает контроллер домена vm2b.test
# (AD DS + DNS + KDS root key + пользователи + шары + gMSA), вводит member в домен, ставит на него VM2Backup и гоняет тесты; в конце копирует результаты в
# .winvm-results/domain-<метка>/ и УДАЛЯЕТ обе ВМ (.winvm/dom/destroy-dom.sh) при любом результате (--keep: оставить).
# Фазы (по умолчанию все по порядку):
#   create   запуск обеих ВМ, установка Windows (25-60 мин), ожидание исполнителей
#   dc       повышение до контроллера (перезагрузка), KDS, OU, пользователи, группа, PSO блокировки, шары
#   join     ввод member в домен (перезагрузка), RSAT, gMSA на member, локальная шара, время/Kerberos; на DC: права MEMBER$ и gMSA
#   install  установка пакета на member (одно-файловый установщик, тихо)
#   domainreal  автотесты domainreal (три формы пользователя x три формы адреса, копия/проверка/восстановление, 5, 1907/1330/1331, обзор сети)
#   domainlock  автотесты domainlock (число попыток при неверном пароле, блокировка 1909) + badPwdCount с контроллера
#   auth     Kerberos или NTLM: события безопасности контроллера за время тестов
#   svc      служба под LocalSystem / VM2B\svcbackup / gMSA (ротация пароля) / без прав на папку данных; DPAPI; права на папку данных (guest/domain-svc.ps1)
#   web      сайт и панель на Server 2022: TLS, вход admin/admin, /selftest, /guitest
#   installer  обновление / удаление / переустановка установщика
set -u
if [ -z "${VM2B_RUNDOM_COPY:-}" ]; then      # скрипт идёт часами: работаем с собственной копией (правка файла посреди прогона ломает разбор)
  SELF_DIR=$(cd "$(dirname "$0")" && pwd); TMPC=$(mktemp /tmp/run-domain.XXXXXX.sh); cp "$0" "$TMPC"
  VM2B_RUNDOM_COPY=1 VM2B_RUNDOM_DIR=$SELF_DIR exec bash "$TMPC" "$@"
fi
R=$(cd "$VM2B_RUNDOM_DIR/.." && pwd); W=$R/.winvm; D=$W/dom
KEEP=0; CREATE=1; BUILD=1; SEED=$RANDOM; PHASES=create,dc,join,install,domainreal,domainlock,auth,svc,web,installer
while [ $# -gt 0 ]; do case "$1" in
  --keep) KEEP=1; shift;; --no-create) CREATE=0; shift;; --no-build) BUILD=0; shift;; --phases) PHASES=$2; shift 2;; --seed) SEED=$2; shift 2;;
  *) echo "unknown argument $1"; exit 2;; esac; done
T0=$(date +%s); SUM=$D/summary.txt; : > "$SUM"; LABEL=$(date +%Y%m%d-%H%M%S)
say() { echo "[$(( $(date +%s) - T0 ))s] $*" | tee -a "$SUM"; }
want() { [[ ",$PHASES," == *",$1,"* ]]; }
declare -A RESULT
RD=$D/shared-dc/results; RM=$D/shared-member/results
res() { [ "$1" = dc ] && echo "$D/shared-dc/results" || echo "$D/shared-member/results"; }
# gj <dc|member> <имя> <таймаут> <строки пакетного файла...>
gj() { local r=$1 n=$2 to=$3; shift 3; printf '%s\r\n' "@echo off" "$@" | "$D/job.sh" "$r" "$n" - "$to"; }
PS='powershell -NoProfile -ExecutionPolicy Bypass -File'
tally() { # tally <фаза> <rc> <роль:имя ...>
  local ph=$1 rc=$2; shift 2; local p=0 f=0 x r n
  for x in "$@"; do r=${x%%:*}; n=${x#*:}; local o; o=$(res $r)/$n.out
    [ -f "$o" ] && { p=$(( p + $(tr -d '\r' < "$o" | grep -ac '^PASS ') )); f=$(( f + $(tr -d '\r' < "$o" | grep -ac '^FAIL ') )); }; done
  RESULT[$ph]="$p pass, $f fail (rc=$rc)"; say "PHASE $ph: ${RESULT[$ph]}"
  [ "$f" -gt 0 ] && for x in "$@"; do r=${x%%:*}; n=${x#*:}; tr -d '\r' < "$(res $r)/$n.out" 2>/dev/null | grep -a '^FAIL ' | sed "s/^/    [$n] /" | tee -a "$SUM"; done
}
reboot_vm() { # reboot_vm <роль> <имя>: перезапускает Windows и ждёт, пока исполнитель вернётся (poller-alive.txt новее метки)
  local r=$1 n=$2 rs; rs=$(res $r); touch "$rs/.reboot-marker"; sleep 1
  gj $r "$n" 60 'shutdown /r /t 3 /f' >/dev/null 2>&1 &
  local t=$(date +%s); sleep 25
  while [ ! "$rs/poller-alive.txt" -nt "$rs/.reboot-marker" ]; do [ $(( $(date +%s) - t )) -gt 1500 ] && { say "$r did not come back after the restart"; return 1; }; sleep 5; done
  sleep 8; say "$r restarted in $(( $(date +%s) - t ))s"; return 0
}
free_check() { say "memory: $(free -m | awk '/Mem:/{print "free " $4 " MB, available " $7 " MB"}'); disk: $(df -BG --output=avail /home | tail -1 | tr -d ' ') free"; }
json_domain() { # servers-domain.json: как тесты видят домен
  cat > "$1" <<'JEOF'
{ "domain": { "unc_fqdn": "\\\\dc.vm2b.test\\Backups", "unc_short": "\\\\dc\\Backups", "unc_ip": "\\\\10.77.0.1\\Backups", "restricted_unc": "\\\\dc.vm2b.test\\Restricted",
    "user_netbios": "VM2B\\bkuser", "user_upn": "bkuser@vm2b.test", "user_short": "bkuser", "password": "Bk-User#2026a", "wrong_password": "Wr0ng-Pa55#not",
    "nouser_name": "VM2B\\nouser", "nouser_password": "No-User#2026a", "locked_user": "VM2B\\lockuser", "locked_password": "Lock-User#2026a",
    "mustchange_user": "VM2B\\chguser", "mustchange_password": "Chg-User#2026a", "expired_user": "VM2B\\expuser", "expired_password": "Exp-User#2026a",
    "disabled_user": "VM2B\\disuser", "disabled_password": "Dis-User#2026a", "badpwd_script": "C:\\vm2b\\guest\\domain-badpwd.ps1" } }
JEOF
}
runtests() { # runtests <группа> <имя задания> [доп. аргументы]
  gj member "$2" 3600 "set TAG=$1" "set ARGS=--seed $SEED --only $1 --servers \\\\10.0.2.4\\qemu\\pkg\\servers-domain.json" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; local rc=$?
  local L=$RM/autotest-$1.txt
  [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  $1: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP' | sed 's/^/    /' | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^\s+(note|NOTE)|^  \[' | head -80 >> "$SUM"; } || say "  $1: NO OUTPUT"
  RESULT[$1]="rc=$rc (число упавших тестов)"
}

# ---------------------------------------------------------------- создание
if want create && [ $CREATE = 1 ]; then
  say "VM2Backup domain test: phases=$PHASES seed=$SEED"; free_check
  for f in "$R/dist/VM2Backup-Setup.exe" "$R/build/x64/vm2bautotest.exe" "$R/build/x64/VM2BackupService.exe"; do [ -f "$f" ] || { say "missing $f: run ./package.sh"; exit 1; }; done
  if [ $BUILD = 1 ] && [ -n "$(find "$R/src" \( -name '*.cpp' -o -name '*.h' \) -newer "$R/dist/VM2Backup-Setup.exe" 2>/dev/null | head -1)" ]; then
    say "sources are newer than dist/VM2Backup-Setup.exe: run ./package.sh first (or --no-build)"; exit 1; fi
  "$W/sync-pkg.sh" w10 > /dev/null || exit 1
  bash "$D/run-dom.sh" dc && bash "$D/run-dom.sh" member || { say "run-dom.sh failed"; "$D/destroy-dom.sh" "$LABEL"; exit 1; }
  rm -rf "$D/shared-member/pkg" "$D/shared-dc/pkg"; cp -a "$W/shared/pkg" "$D/shared-member/pkg"; mkdir -p "$D/shared-dc/pkg"; cp -a "$W/shared/pkg/guest" "$D/shared-dc/pkg/guest"; rm -rf "$W/shared/pkg"
  say "waiting for both Windows installations (25-60 min) ..."
  "$D/wait-dom.sh" dc 7200 | tee -a "$SUM" & PD=$!; "$D/wait-dom.sh" member 7200 | tee -a "$SUM" & PM=$!
  wait $PD; ED=$?; wait $PM; EM=$?
  [ $ED = 0 ] && [ $EM = 0 ] || { say "INSTALL FAILED or too slow (dc=$ED member=$EM)"; "$D/shot-dom.sh" dc "$D/install-fail-dc.png"; "$D/shot-dom.sh" member "$D/install-fail-member.png"; [ $KEEP = 1 ] || "$D/destroy-dom.sh" "$LABEL"; exit 3; }
  say "install time $(( $(date +%s) - T0 ))s"; free_check
fi
sleep 3
[ -d "$D/shared-member/pkg/bin" ] || { echo "no pkg: nothing to run"; exit 1; }
for r in dc member; do mkdir -p "$D/shared-$r/pkg/guest"; cp "$W"/guest/* "$D/shared-$r/pkg/guest/"; done
json_domain "$D/shared-member/pkg/servers-domain.json"
for r in dc member; do gj $r 00-init 300 'if not exist C:\vm2b\guest mkdir C:\vm2b\guest' 'xcopy "%SRC%\pkg\guest\*" C:\vm2b\guest\ /e /y > nul' 'powershell -NoProfile -Command "Set-SmbClientConfiguration -DirectoryCacheLifetime 0 -FileNotFoundCacheLifetime 0 -FileInfoCacheLifetime 0 -Force"' > /dev/null; done
say "executors initialised"

# ---------------------------------------------------------------- контроллер домена
if want dc; then
  gj dc 01-dc1 2400 "$PS C:\\vm2b\\guest\\domain-dc1.ps1" > /dev/null; RC=$?
  tally dc1 $RC dc:01-dc1
  reboot_vm dc 01-dc-reboot || { say "DC did not come back"; }
  gj dc 02-dc2 1800 "$PS C:\\vm2b\\guest\\domain-dc2.ps1" > /dev/null; RC=$?
  tr -d '\r' < "$RD/02-dc2.out" | grep -aE '^INFO' | head -30 | tee -a "$SUM"
  tally dc2 $RC dc:02-dc2
fi

# ---------------------------------------------------------------- рядовой сервер
if want join; then
  gj member 03-member1 900 "$PS C:\\vm2b\\guest\\domain-member1.ps1" > /dev/null; RC=$?
  tally join1 $RC member:03-member1
  reboot_vm member 03-member-reboot || say "member did not come back"
  gj member 04-member2 1200 "$PS C:\\vm2b\\guest\\domain-member2.ps1" > /dev/null; RC2=$?
  tr -d '\r' < "$RM/04-member2.out" | grep -aE '^INFO' | head -30 | tee -a "$SUM"
  gj dc 05-dc3 900 "$PS C:\\vm2b\\guest\\domain-dc3.ps1" > /dev/null; RC3=$?
  gj member 05-member3 900 "$PS C:\\vm2b\\guest\\domain-member2.ps1" > /dev/null    # второй проход: Install-ADServiceAccount после создания gMSA
  tr -d '\r' < "$RM/05-member3.out" | grep -aE '^(INFO Install|PASS|FAIL)' | tee -a "$SUM"
  tally join $(( RC + RC2 + RC3 )) member:04-member2 dc:05-dc3 member:05-member3
fi

# ---------------------------------------------------------------- продукт
if want install; then
  gj member 06-install 900 'call C:\vm2b\guest\install.cmd' > /dev/null
  tr -d '\r' < "$RM/06-install.out" | grep -aE 'INSTALL_RC|RUNNING|STOPPED|LocalSystem' | tee -a "$SUM"
  gj member 06-waitsvc 600 "$PS C:\\vm2b\\guest\\waitsvc.ps1 400" > /dev/null
fi
START=$(date +%s)
if want domainreal; then runtests domainreal 07-at-domainreal; fi
if want domainlock; then runtests domainlock 08-at-domainlock; gj member 08-unlock 300 "$PS C:\\vm2b\\guest\\domain-unlock.ps1" > /dev/null; fi
if want auth; then
  gj member 09-auth 600 "$PS C:\\vm2b\\guest\\domain-auth.ps1 $(( $(date +%s) - START + 120 ))" > /dev/null; RC=$?
  tr -d '\r' < "$RM/09-auth.out" | grep -aE '^INFO' | head -60 | tee -a "$SUM"
  tally auth $RC member:09-auth
fi
if want svc; then
  gj member 10-svc 5400 "$PS C:\\vm2b\\guest\\domain-svc.ps1" > /dev/null; RC=$?
  tr -d '\r' < "$RM/10-svc.out" | grep -aE '^INFO' | head -80 | tee -a "$SUM"
  if tr -d '\r' < "$RM/10-svc.out" | grep -aq 'CLOCKSHIFTED=1'; then      # скрипт сдвинул часы обеих ВМ на +2 суток (ротация gMSA): возвращаем их заданиями с хоста
    say "returning the clocks of both VMs by 2 days"
    for r in dc member; do gj $r 10-clock-$r 120 'powershell -NoProfile -Command "Set-Date ((Get-Date).AddDays(-2)); Start-Service w32time -ErrorAction SilentlyContinue"' 'echo %DATE% %TIME%' > /dev/null; done
    gj member 10-clock-check 120 'w32tm /resync /force' 'echo %DATE% %TIME%' > /dev/null; tr -d '\r' < "$RM/10-clock-check.out" | tail -2 | tee -a "$SUM"
  fi
  tally svc $RC member:10-svc
fi
if want web; then
  gj member 11-web 900 "$PS C:\\vm2b\\guest\\web.ps1" > /dev/null; RC=$?
  gj member 11-panel 900 'call C:\vm2b\guest\panel.cmd 100' > /dev/null; RC2=$?
  tr -d '\r' < "$RM/11-panel.out" | grep -aE 'SELFTEST_RC' | tee -a "$SUM"
  gj member 11-gui 900 "set TAG=gui" "set ARGS=--seed $SEED --only gui" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; echo "  gui autotest rc=$?" | tee -a "$SUM"
  mkdir -p "$RM/shots"; "$D/shot-dom.sh" member "$RM/shots/member-desktop.png" > /dev/null 2>&1
  tally web $(( RC + RC2 )) member:11-web
fi
if want installer; then
  cp "$W/pkg-old/"*.exe "$D/shared-member/pkg/old.exe" 2>/dev/null
  gj member 12-installer 1800 'call C:\vm2b\guest\uninstall-update.cmd' > /dev/null; RC=$?
  tr -d '\r' < "$RM/12-installer.out" | grep -aE '_RC=|service pid|FileVersion|STATE' | tee -a "$SUM"
  tally installer $RC member:12-installer
fi

# ---------------------------------------------------------------- итог
say "================ SUMMARY (domain, seed $SEED, $(( $(date +%s) - T0 ))s) ================"
for k in "${!RESULT[@]}"; do echo "  $k: ${RESULT[$k]}" | tee -a "$SUM"; done
for f in "$RM"/autotest-*.txt; do [ -f "$f" ] && echo "  log $(basename "$f" .txt | sed 's/^autotest-//'): $(tr -d '\r' < "$f" | grep -aE ' passed, ' | tail -1)" | tee -a "$SUM"; done
cp "$SUM" "$RM/summary.txt" 2>/dev/null
if [ $KEEP = 0 ]; then "$D/destroy-dom.sh" "$LABEL"; else say "ВМ оставлены (--keep): удалить - .winvm/dom/destroy-dom.sh <метка>"; fi
