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
#   browse   обзор сети на member: WNetOpenEnum, NetServerEnum, DsGetDcName, LDAP - что находит контроллер и серверы домена (от пользователя и от SYSTEM)
#   svc      служба под LocalSystem / VM2B\svcbackup / gMSA / без прав на папку данных; DPAPI; права на папку данных (guest/domain-svc.ps1); два прохода подряд
#   reboot   перезапуск ОБЕИХ ВМ: исполнитель обязан подняться сам (автовход по имени из SID -500), потом третий проход svc (svc3)
#   web      сайт и панель на Server 2022: TLS, вход admin/admin, /selftest, /guitest
#   installer  обновление / удаление / переустановка установщика
#   ftp      ftp_large_streamed 10 раз на member (FTP-серверы на хосте, журнал DEBUG), журналы серверов в результаты
#   rotate   синхронный сдвиг часов ОБЕИХ ВМ на +45 суток: ротация пароля gMSA (способ А: pwdLastSet=0 до сдвига, способ Б: сдвиг), срок пароля expuser (1330), возврат часов
set -u
if [ -z "${VM2B_RUNDOM_COPY:-}" ]; then      # скрипт идёт часами: работаем с собственной копией (правка файла посреди прогона ломает разбор)
  SELF_DIR=$(cd "$(dirname "$0")" && pwd); TMPC=$(mktemp /tmp/run-domain.XXXXXX.sh); cp "$0" "$TMPC"
  VM2B_RUNDOM_COPY=1 VM2B_RUNDOM_DIR=$SELF_DIR exec bash "$TMPC" "$@"
fi
R=$(cd "$VM2B_RUNDOM_DIR/.." && pwd); W=$R/.winvm; D=$W/dom
KEEP=0; CREATE=1; BUILD=1; SEED=$RANDOM; PHASES=create,dc,join,install,domainreal,domainlock,auth,browse,svc,reboot,web,installer,ftp,rotate
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
    "disabled_user": "VM2B\\disuser", "disabled_password": "Dis-User#2026a", "badpwd_script": "C:\\vm2b\\guest\\domain-badpwd.ps1",
    "expired_new_password": "Exp-User#2026b", "chpwd_script": "C:\\vm2b\\guest\\domain-chpwd.ps1" } }
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
run_svc() { # run_svc <N>: один проход domain-svc.ps1 (разбор нестабильных проверок: журнал ответов servers.save без идентификатора)
  local n=$1 nm=10-svc; [ "$n" = 1 ] || nm=10-svc$n
  gj member $nm 5400 "$PS C:\\vm2b\\guest\\domain-svc.ps1" > /dev/null; local rc=$?
  tr -d '\r' < "$RM/$nm.out" | grep -aE '^INFO' | head -80 | tee -a "$SUM"
  tally svc$n $rc member:$nm
}
if want browse; then
  gj member 13-browse 600 "$PS C:\\vm2b\\guest\\domain-browse.ps1" > /dev/null
  tr -d '\r' < "$RM/13-browse.out" | grep -aE '^INFO' | head -120 | tee -a "$SUM"
  gj member 13-browse-sys 600 'del C:\vm2b\browse-sys.txt 2>nul' 'schtasks /create /tn vm2bbrowse /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\domain-browse.ps1 -Out C:\vm2b\browse-sys.txt" /sc once /st 00:00 /ru SYSTEM /f' 'schtasks /run /tn vm2bbrowse' \
    "powershell -NoProfile -Command \"for(\$i=0;\$i -lt 120;\$i++){ if((Test-Path C:\\vm2b\\browse-sys.txt) -and (Select-String -Path C:\\vm2b\\browse-sys.txt -Pattern 'INFO DONE' -Quiet)){break}; Start-Sleep 2 }\"" 'type C:\vm2b\browse-sys.txt' 'schtasks /delete /tn vm2bbrowse /f' > /dev/null
  tr -d '\r' < "$RM/13-browse-sys.out" | iconv -f UTF-8 -t UTF-8 -c | grep -aE '^INFO' | head -120 | tee -a "$SUM"
fi
if want svc; then run_svc 1; run_svc 2; fi
if want reboot; then
  say "restarting both VMs: the executor must come back by itself (autologon), no manual login"
  reboot_vm dc 14-reboot-dc & PR1=$!; reboot_vm member 14-reboot-member & PR2=$!; wait $PR1; E1=$?; wait $PR2; E2=$?
  gj member 14-after-reboot 300 "$PS C:\\vm2b\\guest\\domain-member2.ps1" > /dev/null; tally reboot $(( E1 + E2 )) member:14-after-reboot
  tr -d '\r' < "$RM/14-after-reboot.out" | grep -aE '^INFO w32tm|^INFO time' | head -6 >> "$SUM"
  gj member 14-who 120 'whoami' 'query session' > /dev/null; tr -d '\r' < "$RM/14-who.out" | head -8 | sed 's/^/  member session: /' | tee -a "$SUM"
  gj dc 14-who 120 'whoami' 'query session' > /dev/null; tr -d '\r' < "$RD/14-who.out" | head -8 | sed 's/^/  dc session: /' | tee -a "$SUM"
  run_svc 3
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

if want ftp; then
  GW=$(docker network inspect vm2b-domnet -f '{{(index .IPAM.Config 0).Gateway}}' 2>/dev/null); [ -n "$GW" ] || GW=172.17.0.1
  gj member 15-hosts 120 "findstr /c:\"vm2b-host.test\" %SystemRoot%\\System32\\drivers\\etc\\hosts >nul || echo $GW vm2b-host.test>>%SystemRoot%\\System32\\drivers\\etc\\hosts" 'ipconfig /flushdns' > /dev/null
  VM2B_BIND_ADDR=$GW VM2B_SRV_DIR=$D/shared-member/srv VM2B_SHARED_ROOT=$D/shared-member VM2B_FTP_LOGLEVEL=DEBUG VM2B_SEED=$SEED "$R/tools/winvm-servers.sh" start ftp > /dev/null 2>&1 || say "FTP servers did not start"
  gj member 15-ftp 5400 'for /l %%i in (1,1,10) do ( set TAG=ftpbig%%i& set ARGS=--seed %%i17 --only ftp_large_streamed --servers \\10.0.2.4\qemu\srv\ftp\servers.json& call C:\vm2b\guest\autotest.cmd )' > /dev/null
  for i in 1 2 3 4 5 6 7 8 9 10; do L=$RM/autotest-ftpbig$i.txt; [ -f "$L" ] && tr -d '\r' < "$L" | grep -aE '^(PASS|FAIL)|upload|download' | tr '\n' ' ' | sed "s/^/  ftpbig$i: /" | tee -a "$SUM"; echo | tee -a "$SUM" > /dev/null; done
  mkdir -p "$RM/srv-ftp"; cp "$D/shared-member/srv/ftp/"*.log "$RM/srv-ftp/" 2>/dev/null
  "$R/tools/winvm-servers.sh" stop-all > /dev/null 2>&1; rm -rf "$D/shared-member/srv"
fi

# ---- сдвиг часов: B (ротация пароля gMSA) и C (срок пароля 1330) -------------------------------------------------
set_clocks() { # set_clocks <суток вперёд от времени ХОСТА>: ОБЕ ВМ получают одно и то же абсолютное время (host + N суток); w32time выключен (иначе одна ВМ вернёт время по другой)
  local days=$1 t; t=$(date -d "+$days days" '+%Y-%m-%d %H:%M:%S')
  for r in dc member; do gj $r 16-clock-$r 120 'sc config w32time start= disabled' 'net stop w32time' "powershell -NoProfile -Command \"Set-Date '$t' | Out-Null\"" 'echo %DATE% %TIME%' > /dev/null & done; wait
  say "clocks set to host time +$days days ($t): dc $(tr -d '\r' < "$RD/16-clock-dc.out" | tail -1), member $(tr -d '\r' < "$RM/16-clock-member.out" | tail -1)"
}
shift_clocks() { set_clocks "$1"; }
return_clocks() { # вернуть часы (время хоста), включить w32time, проверить канал домена
  set_clocks 0
  for r in dc member; do gj $r 16-w32on-$r 120 'sc config w32time start= auto' 'net start w32time' > /dev/null & done; wait
  gj member 16-resync 180 'w32tm /resync /force' 'echo %DATE% %TIME%' "powershell -NoProfile -Command \"'secure channel: ' + (Test-ComputerSecureChannel); 'gMSA: ' + (Test-ADServiceAccount vm2bgmsa)\"" > /dev/null
  tr -d '\r' < "$RM/16-resync.out" | tail -4 | tee -a "$SUM"
}
if want rotate; then
  free_check; DAYS=${ROTATE_DAYS:-45}
  gj dc 16-reset-exp 300 "powershell -NoProfile -Command \"Import-Module ActiveDirectory; Set-ADAccountPassword expuser -Reset -NewPassword (ConvertTo-SecureString 'Exp-User#2026a' -AsPlainText -Force); Set-ADUser expuser -ChangePasswordAtLogon \$false -PasswordNeverExpires \$false\"" > /dev/null
  gj member 16-rot1 1800 "$PS C:\\vm2b\\guest\\domain-rot1.ps1" > /dev/null; RC1=$?
  tr -d '\r' < "$RM/16-rot1.out" | grep -aE '^INFO' | tee -a "$SUM"
  gj dc 16-dcinfo-before 300 "$PS C:\\vm2b\\guest\\domain-dcinfo.ps1 before" > /dev/null; tr -d '\r' < "$RD/16-dcinfo-before.out" | grep -aE '^INFO' | tee -a "$SUM"
  shift_clocks $DAYS
  gj dc 16-dcinfo-after 300 "$PS C:\\vm2b\\guest\\domain-dcinfo.ps1 shifted" > /dev/null; tr -d '\r' < "$RD/16-dcinfo-after.out" | grep -aE '^INFO' | tee -a "$SUM"
  gj member 16-rot2 1800 "$PS C:\\vm2b\\guest\\domain-rot2.ps1" > /dev/null; RC2=$?
  tr -d '\r' < "$RM/16-rot2.out" | grep -aE '^INFO' | tee -a "$SUM"
  gj member 16-wneterr 120 "$PS C:\\vm2b\\guest\\domain-wneterr.ps1 \\\\dc.vm2b.test\\Backups VM2B\\expuser Exp-User#2026a" > /dev/null; tr -d '\r' < "$RM/16-wneterr.out" | iconv -f cp866 -t utf8 -c | grep -aE '^(WNET|WNetGetLastError)' | tee -a "$SUM"
  runtests domainexpire 16-at-domainexpire
  gj dc 16-dcinfo-end 300 "$PS C:\\vm2b\\guest\\domain-dcinfo.ps1 end" > /dev/null; tr -d '\r' < "$RD/16-dcinfo-end.out" | grep -aE '^INFO' | tee -a "$SUM"
  return_clocks
  tally rotate $(( RC1 + RC2 )) member:16-rot1 member:16-rot2
fi

# ---------------------------------------------------------------- итог
say "================ SUMMARY (domain, seed $SEED, $(( $(date +%s) - T0 ))s) ================"
for k in "${!RESULT[@]}"; do echo "  $k: ${RESULT[$k]}" | tee -a "$SUM"; done
for f in "$RM"/autotest-*.txt; do [ -f "$f" ] && echo "  log $(basename "$f" .txt | sed 's/^autotest-//'): $(tr -d '\r' < "$f" | grep -aE ' passed, ' | tail -1)" | tee -a "$SUM"; done
cp "$SUM" "$RM/summary.txt" 2>/dev/null
if [ $KEEP = 0 ]; then "$D/destroy-dom.sh" "$LABEL"; else say "ВМ оставлены (--keep): удалить - .winvm/dom/destroy-dom.sh <метка>"; fi
