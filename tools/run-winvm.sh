#!/bin/bash
# The whole VM2Backup test plan on a REAL Windows test VM (docs/WINDOWS-TESTS.md).
#   tools/run-winvm.sh domain [...]   - НАСТОЯЩИЙ домен AD: две ВМ Server 2022 (vm2b-dc + vm2b-member), см. tools/run-domain.sh
#   tools/run-winvm.sh <x86|w10|w11> [--keep] [--no-create] [--phases a,b,c] [--seed N] [--no-build]
# Creates the VM (container vm2b-win, .winvm/run-os.sh), waits for the install, runs the phases, prints a summary, copies the results to
# .winvm-results/<os>-<time>/ and DELETES the VM, its storage and the custom ISOs (.winvm/destroy.sh), whatever the result (--keep: leave it).
# Phases (default: all, in this order):
#   sysinfo   Windows version / build / adapters / TLS registry
#   autotest  vm2bautotest on the clean guest: core engine resume restore xz smb web svcweb panel, service (real SCM), gui (interactive session)
#   install   package install, files, SCM registration (sc qc / qfailure / sdshow), service stop / STOP_PENDING / restart / crash recovery, pipe ACL
#   panel     VM2Backup.exe /selftest, /guitest, screenshots at 100 / 125 / 150 % scaling (three restarts of the VM)
#   web       TLS from the Windows clients, login, session, RPC, adapters, bind to the guest address (host curl through the port forward), DPAPI over restarts
#   net       ftp tls http / ssh scp / esxi svcesxi groups against servers on THIS host (Docker bridge address)
#   smb       the REAL SMB share (group smbreal), net use Y: + net.drives
#   win11     Windows 11 specifics (wmic, Smart App Control, SMB client policy, terminal, event source ...), informational
#   defender  Microsoft Defender at the end of the run: real-time ON, no detections, no exclusions (nothing is ever disabled)
#   signing   (w11) the SIGNED Setup with Mark of the Web, four cases (unsigned / signed untrusted / signed trusted / tampered): SmartScreen, UAC, screenshots (docs/SIGNING.md)
#   sac       (w11, ALWAYS LAST: cannot be undone) Smart App Control: try to switch it to On, restart, record what happens to the unsigned Setup and the service
#   smartscreen  the Setup with Mark of the Web started like a double click, screenshots of what the user sees
#   big       the 3 'big' autotests (4 GiB; VM2B_BIG=1)
#   winbig    6 GiB sparse vmdk through the whole pipeline (xz level 1), peak memory
#   winreal   tests that only make sense on REAL Windows (src/tests_winreal.cpp): links, locks, scanner, NTFS names, attributes, DPAPI, unreadable config
#   svcacct   the service under other accounts (admin / unknown to Samba / standard), data-folder rights, DPAPI scope, own-rights SMB (guest/svcacct.ps1)
#   smbsign   SMB signing REQUIRED on both sides (Samba mandatory + Windows RequireSecuritySignature): smbreal over the signed session
#   aclreal   права папки данных (SDDL) и канала службы от обычного пользователя с кириллическим именем (guest/aclreal.ps1)
#   speedlimit  ограничение скорости на реальных данных: время записи без лимита / с лимитом элемента, настройки, окна расписания (guest/speedlimit.ps1)
#   restartjob  shutdown /r in the middle of a long backup: the service resumes the job after the boot, the result verifies
#   verify    the local groups again (core engine resume restore xz smb panel) with the product installed
#   unicode   a local account with a Cyrillic name: native autotests (core) under its profile (C:\Users\<cyrillic>)
#   installer update in place (old package -> new), removal (data kept), reinstall: leaves the NEW package installed for the phases that follow
set -u
# Скрипт идёт часами, а bash читает его по кускам: правка файла посреди прогона ломает разбор ("syntax error near unexpected token"; так упал прогон
# 26.10 раунд 2). Поэтому первым делом запускаем собственную копию.
if [ -z "${VM2B_RUNWINVM_COPY:-}" ]; then
  SELF_DIR=$(cd "$(dirname "$0")" && pwd); TMPC=$(mktemp /tmp/run-winvm.XXXXXX.sh); cp "$0" "$TMPC"
  VM2B_RUNWINVM_COPY=1 VM2B_RUNWINVM_DIR=$SELF_DIR exec bash "$TMPC" "$@"
fi
R=$(cd "$VM2B_RUNWINVM_DIR/.." && pwd); W=$R/.winvm; RES=$W/shared/results
OS=${1:-}; shift || true
[ "$OS" = domain ] && exec bash "$R/tools/run-domain.sh" "$@"      # настоящий домен AD: две ВМ Server 2022 (tools/run-domain.sh)
case "$OS" in x86|w10|w11) ;; *) sed -n 2,30p "$0"; exit 2;; esac
KEEP=0; CREATE=1; BUILD=1; PHASES=sysinfo,autotest,winreal,install,installer,panel,web,net,smb,smbsign,svcacct,aclreal,speedlimit,restartjob,unicode,verify,big,winbig; [ "$OS" = w11 ] && PHASES=sysinfo,win11,autotest,winreal,install,installer,panel,web,net,smb,smbsign,svcacct,aclreal,speedlimit,restartjob,unicode,verify,big,winbig,defender,smartscreen,sac; SEED=$(( (RANDOM << 15 | RANDOM) % 1000000000 ))
while [ $# -gt 0 ]; do case "$1" in
  --keep) KEEP=1; shift;; --no-create) CREATE=0; shift;; --no-build) BUILD=0; shift;; --phases) PHASES=$2; shift 2;; --seed) SEED=$2; shift 2;;
  *) echo "unknown argument $1"; exit 2;; esac; done
case "$OS" in x86) GS='\\10.0.2.4\qemu'; ARCH=x86;; *) GS='\\host.lan\Data'; ARCH=x64;; esac
HOSTIP=172.17.0.1
export VM2B_GUEST_SHARE=$GS
if [ "$OS" = x86 ]; then SMB1=10.0.2.50; SMB2=10.0.2.51; SMBX=10.0.2.77; SMBCTL=10.0.2.2:4456; GUESTIP=
else SMB1=172.30.0.50; SMB2=172.30.0.51; SMBX=172.30.0.77; SMBCTL=172.30.0.1:4456; GUESTIP=172.30.0.2; fi      # dockur: guest 172.30.0.2, gateway = the container
T0=$(date +%s); SUM=$W/summary.txt; : > "$SUM"
say() { echo "[$(( $(date +%s) - T0 ))s] $*" | tee -a "$SUM"; }
want() { [[ ",$PHASES," == *",$1,"* ]]; }
declare -A RESULT
# gj <name> <timeout> <cmd lines...>: one job on the guest (the lines are the batch file); prints the output, returns the job's exit code
gj() { local n=$1 to=$2; shift 2; printf '%s\r\n' "@echo off" "$@" | "$W/job.sh" "$n" - "$to"; }
# phase result from the job's output: counts the PASS / FAIL lines the scripts print
tally() { # tally <phase> <rc> <job names...>
  local ph=$1 rc=$2; shift 2; local p=0 f=0 n
  for n in "$@"; do [ -f "$RES/$n.out" ] && { p=$(( p + $(tr -d '\r' < "$RES/$n.out" | grep -ac '^PASS ') )); f=$(( f + $(tr -d '\r' < "$RES/$n.out" | grep -ac '^FAIL ') )); }; done
  RESULT[$ph]="$p pass, $f fail (rc=$rc)"; say "PHASE $ph: ${RESULT[$ph]}"
  [ "$f" -gt 0 ] && for n in "$@"; do tr -d '\r' < "$RES/$n.out" 2>/dev/null | grep -a '^FAIL ' | sed "s/^/    [$n] /" | tee -a "$SUM"; done
}
reboot_guest() { # reboot_guest <name> : restarts Windows and waits until the executor is back (autologon + logon task)
  touch "$RES/.reboot-marker"; sleep 1
  gj "$1" 60 'shutdown /r /t 3 /f' >/dev/null 2>&1 &
  local t=$(date +%s); sleep 20
  while [ ! "$RES/poller-alive.txt" -nt "$RES/.reboot-marker" ]; do [ $(( $(date +%s) - t )) -gt 900 ] && { say "guest did not come back after the restart"; return 1; }; sleep 5; done
  sleep 5; say "guest restarted in $(( $(date +%s) - t ))s"
  gj "$1-svc" 600 'sc query VM2BackupService >nul 2>&1 && powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\waitsvc.ps1 480' > /dev/null    # the service starts with a delay after boot (DELAYED auto start)
  return 0
}
json_smb() { # json_smb <file> [drive]: servers.json for group smbreal as the guest sees the share
  local drive=${2:-}
  cat > "$1" <<EOF
{ "smb": { "unc": "\\\\\\\\$SMB1\\\\vm2b", "alt_unc": "\\\\\\\\$SMB2\\\\vm2b", "enc_unc": "\\\\\\\\$SMB1\\\\vm2benc", "unreachable_unc": "\\\\\\\\$SMBX\\\\vm2b",
           "cyr_unc": "\\\\\\\\$SMB1\\\\резерв", "cyr_user": "юзер-смб", "cyr_password": "Пар0ль-Смб!42", "ro_user": "vm2bro", "ro_password": "Ro-Only-Pw!42",
           "user": "vm2buser", "password": "Sm8 Real&Pw!42", "wrong_password": "Wr0ng-Pa55-not", "control": "$SMBCTL"${drive:+, \"drive\": \"$drive\"} } }
EOF
}

# ---------------------------------------------------------------- build + create
if [ $CREATE = 1 ]; then
  say "VM2Backup Windows test: os=$OS seed=$SEED phases=$PHASES"
  for f in "$R/dist/VM2Backup-Setup.exe" "$R/build/$ARCH/vm2bautotest.exe" "$R/build/$ARCH/VM2BackupService.exe"; do [ -f "$f" ] || { say "missing $f: run ./package.sh"; exit 1; }; done
  if [ $BUILD = 1 ] && [ -n "$(find "$R/src" -newer "$R/dist/VM2Backup-Setup.exe" -name '*.cpp' -o -newer "$R/dist/VM2Backup-Setup.exe" -name '*.h' 2>/dev/null | head -1)" ]; then
    say "sources are newer than dist/VM2Backup-Setup.exe: run ./package.sh first (or pass --no-build to test the package as it is)"; exit 1; fi
  "$W/sync-pkg.sh" "$OS" > /dev/null || exit 1
  "$W/run-os.sh" "$OS" > /dev/null || { say "run-os.sh failed"; exit 1; }
  # smb-real (a password-protected Samba + its control port) lives in the container
  say "waiting for the Windows installation (30-60 min) ..."
  "$W/wait-ready.sh" 7200 | tee -a "$SUM"; [ "${PIPESTATUS[0]}" = 0 ] || { say "INSTALL FAILED or too slow: $("$W/shot.sh" "$W/install-fail.png")"; [ $KEEP = 1 ] || "$W/destroy.sh" "$OS"; exit 3; }
  "$W/shot.sh" "$RES/desktop-after-install.png" >/dev/null
  say "install time $(( $(date +%s) - T0 ))s"
fi
sleep 5
[ -d "$W/shared/pkg" ] || "$W/sync-pkg.sh" "$OS" >/dev/null
mkdir -p "$W/shared/pkg/guest"; cp "$W"/guest/* "$W/shared/pkg/guest/"      # always the current scripts (also with --no-create)
docker exec -d vm2b-win python3 /shared/smb-real.py 2>/dev/null
gj 00-init 300 'if not exist C:\vm2b\guest mkdir C:\vm2b\guest' 'powershell -NoProfile -Command "Set-SmbClientConfiguration -DirectoryCacheLifetime 0 -FileNotFoundCacheLifetime 0 -FileInfoCacheLifetime 0 -Force"' 'xcopy "%SRC%\pkg\guest\*" C:\vm2b\guest\ /e /y /i /q' 'reg add "HKLM\SYSTEM\CurrentControlSet\Control\Network\NewNetworkWindowOff" /f' 'reg add "HKLM\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters" /v UtilizeNtCaching /t REG_DWORD /d 0 /f' 'sc config wuauserv start= disabled' 'sc stop wuauserv' 'reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoUpdate /t REG_DWORD /d 1 /f' 'echo SRC=%SRC%' 'echo arch: ' 'type "%SRC%\pkg\arch.txt"' > /dev/null
say "executor initialised"

# ---------------------------------------------------------------- phases
if want win11; then
  gj 01-win11 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\win11.ps1' > /dev/null; RC=$?
  RESULT[win11]="rc=$RC (informational: 01-win11.out)"
fi

if want sysinfo; then
  gj 01-sysinfo 300 'call C:\vm2b\guest\sysinfo.cmd' > /dev/null; RC=$?
  tr -d '\r' < "$RES/01-sysinfo.out" | grep -aE 'ProductName|DisplayVersion|CurrentBuildNumber|UBR|PROCESSOR_ARCH|OS Name|OS Version|System Type|IPv4|Total Physical' | head -12 | tee -a "$SUM"
  RESULT[sysinfo]="rc=$RC"
fi

if want autotest; then
  # the local groups on the clean guest (the service tests use the real service name and the default pipe: no product installed yet)
  gj 02-at-core 5400 "set TAG=core" "set ARGS=--seed $SEED --only core,engine,resume,restore,xz,smb,web,svcweb,panel" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC1=$?
  gj 02-at-service 1800 "set TAG=service" "set ARGS=--seed $SEED --only service" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC2=$?
  gj 02-at-gui 1800 "set TAG=gui" "set ARGS=--seed $SEED --only gui" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC3=$?
  for t in core service gui; do
    L=$RES/autotest-$t.txt
    [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  autotest $t: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP' | sed 's/^/    /' | tee -a "$SUM"; } || say "  autotest $t: NO OUTPUT"
  done
  RESULT[autotest]="core rc=$RC1, service rc=$RC2, gui rc=$RC3 (rc = number of failed tests)"
fi

if want winreal; then
  gj 02b-at-winreal 3600 "set TAG=winreal" "set ARGS=--seed $SEED --only winreal" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
  L=$RES/autotest-winreal.txt
  [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  winreal: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP' | sed 's/^/    /' | tee -a "$SUM"; } || say "  winreal: NO OUTPUT"
  RESULT[winreal]="rc=$RC (number of failed tests)"
fi

if want install; then
  gj 03-install 900 'call C:\vm2b\guest\install.cmd' > /dev/null; RC=$?
  tr -d '\r' < "$RES/03-install.out" | grep -aE 'INSTALL_RC|RUNNING|STOPPED|AUTO_START|LocalSystem' | tee -a "$SUM"
  gj 03-service 1800 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\service.ps1' > /dev/null; RC2=$?
  gj 03-pipeacl 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\pipeacl.ps1' > /dev/null; RC3=$?
  tally install $(( RC + RC2 + RC3 )) 03-service 03-pipeacl
  grep -aE 'idle stop|busy stop|recovery restart|service pid' "$RES/03-service.out" | tr -d '\r' | tee -a "$SUM"
fi

if want unicode; then
  gj 10-unicode 1500 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\unicode-user.ps1' > /dev/null; RC=$?
  tally unicode $RC 10-unicode
fi

if want installer; then
  cp "$W/pkg-old/"*.exe "$W/shared/pkg/old.exe" 2>/dev/null
  gj 08-installer 1800 'call C:\vm2b\guest\uninstall-update.cmd' > /dev/null; RC=$?
  tr -d '\r' < "$RES/08-installer.out" | grep -aE '_RC=|service pid|FileVersion|^[0-9]+\.[0-9]+\.[0-9]+|STATE|^INFO \[old\]' | tee -a "$SUM"
  tally installer-acl $RC 08-installer
  RESULT[installer]="rc=$RC"
fi

if want panel; then
  gj 04-panel-100 900 'call C:\vm2b\guest\panel.cmd 100' > /dev/null; RC=$?
  tr -d '\r' < "$RES/04-panel-100.out" | grep -aE 'SELFTEST_RC' | tee -a "$SUM"
  # the window test (in-process test service on its own pipe: it never touches the installed service) at every scale
  gj 04-gui-100 900 "set TAG=gui100" "set ARGS=--seed $SEED --only gui" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; echo "  100%: gui autotest rc=$?" | tee -a "$SUM"
  for dpi in 125:120 150:144; do
    pct=${dpi%%:*}; px=${dpi##*:}
    gj 04-dpi-$pct 120 "reg add \"HKCU\\Control Panel\\Desktop\" /v LogPixels /t REG_DWORD /d $px /f" "reg add \"HKCU\\Control Panel\\Desktop\" /v Win8DpiScaling /t REG_DWORD /d 1 /f" > /dev/null
    reboot_guest 04-reboot-$pct || break
    gj 04-panel-$pct 900 "call C:\\vm2b\\guest\\panel.cmd $pct" > /dev/null
    tr -d '\r' < "$RES/04-panel-$pct.out" | grep -aE 'SELFTEST_RC' | sed "s/^/  $pct%: /" | tee -a "$SUM"
    gj 04-gui-$pct 900 "set TAG=gui$pct" "set ARGS=--seed $SEED --only gui" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; echo "  $pct%: gui autotest rc=$?" | tee -a "$SUM"
  done
  gj 04-dpi-reset 120 'reg add "HKCU\Control Panel\Desktop" /v LogPixels /t REG_DWORD /d 96 /f' 'reg add "HKCU\Control Panel\Desktop" /v Win8DpiScaling /t REG_DWORD /d 0 /f' > /dev/null
  reboot_guest 04-reboot-100 >/dev/null
  RESULT[panel]="screenshots: $(ls "$RES/shots" 2>/dev/null | tr '\n' ' ')"
fi

if want web; then
  gj 05-resetweb 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\waitsvc.ps1 60 || call C:\vm2b\guest\resetweb.cmd' > /dev/null    # the site must be up (the recovery /reset-web runs only if it is not)
  gj 05-web 900 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\web.ps1' > /dev/null; RC=$?
  grep -aE 'negotiated|FAILED:|^curl \[' "$RES/05-web.out" | tr -d '\r' | tee -a "$SUM"
  gj 05-bind 300 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\web-bind.ps1 set' > /dev/null; RC2=$?
  # from the host side: through the qemu port forward (container :18443 -> guest :8443 on its real address 10.0.2.15)
  if [ -n "$GUESTIP" ]; then HC=$(docker exec vm2b-win sh -c "curl -sk -o /dev/null -w '%{http_code}' --max-time 15 https://$GUESTIP:8443/api/me" 2>&1)     # dockur: the container is on the guest's subnet
  else HC=$(docker exec vm2b-win sh -c 'curl -sk -o /dev/null -w "%{http_code}" --max-time 15 https://127.0.0.1:18443/api/me' 2>&1); fi
  say "host curl through the forward to the guest's real address: HTTP $HC"
  echo "HOSTCURL=$HC" > "$RES/05-hostcurl.out"; [ "$HC" = 200 ] && echo "PASS  host reaches the site on the guest's real address (HTTP 200)" >> "$RES/05-hostcurl.out" || echo "FAIL  host cannot reach the site on the guest's real address: $HC" >> "$RES/05-hostcurl.out"
  gj 05-unbind 300 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\web-bind.ps1 reset' > /dev/null
  # DPAPI: a server with a password (the real SMB share), through a service restart and a restart of Windows
  gj 05-dpapi-create 300 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\dpapi.ps1 create \\\\$SMB1\\vm2b vm2buser \"Sm8 Real&Pw!42\"" > /dev/null; RC3=$?
  gj 05-dpapi-svc 300 'net stop VM2BackupService' 'net start VM2BackupService' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\dpapi.ps1 check' > /dev/null
  reboot_guest 05-reboot-dpapi && gj 05-dpapi-reboot 300 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\dpapi.ps1 check' > /dev/null
  tally web $(( RC + RC2 + RC3 )) 05-web 05-bind 05-hostcurl 05-dpapi-create 05-dpapi-svc 05-dpapi-reboot
fi

if want net; then
  JS=$W/shared/srv
  gj 06-hosts 120 'findstr /c:"vm2b-host.test" %SystemRoot%\System32\drivers\etc\hosts >nul || echo 172.17.0.1 vm2b-host.test>>%SystemRoot%\System32\drivers\etc\hosts' 'ipconfig /flushdns' > /dev/null    # the certificates of the ftp / tls servers are made for this name
  for set in ${NETSETS:-ftp ssh esxi}; do
    if [ $set = esxi ]; then      # эмулятор ESXi работает ВНУТРИ гостя (локальный NTFS): клиент SMB в Windows кешировал листинги и содержимое датастора на хосте, 5 тестов падали ложно
      gj 06-at-esxi 10800 "set TAG=esxi" 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\esxilocal.ps1 start' "set ARGS=--seed $SEED --only esxi,svcesxi --servers C:\\vm2b\\esxi\\servers.json" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' 'set RC=%ERRORLEVEL%' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\esxilocal.ps1 stop' 'exit /b %RC%' > /dev/null; RC=$?
      L=$RES/autotest-esxi.txt
      [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  net esxi (mocks inside the guest): /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL' | sed 's/^/    /' | tee -a "$SUM"; } || say "  net esxi: NO OUTPUT"
      RESULT[net-esxi]="rc=$RC"; continue
    fi
    "$R/tools/winvm-servers.sh" start $set > /dev/null 2>&1 || { say "servers $set did not start"; continue; }
    case $set in
      ftp)  A="--only ftp,tls,http"; F=ftp;;
      ssh)  A="--only ssh,scp"; F=ssh;;
      esxi) A="--only esxi,svcesxi"; F=esxi;;
    esac
    gj 06-at-$set 7200 "set TAG=$set" "set ARGS=--seed $SEED $A --servers $GS\\srv\\$F\\servers.json" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
    L=$RES/autotest-$set.txt
    [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  net $set: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL' | sed 's/^/    /' | tee -a "$SUM"; } || say "  net $set: NO OUTPUT"
    # журналы тестовых серверов (для разбора нестабильных тестов, например ftp_large_streamed / ftp_abort_and_idle_connection): уровень журнала FTP-сервера задаёт VM2B_FTP_LOGLEVEL
    mkdir -p "$RES/srv-$set"; cp "$JS/$F"/*.log "$RES/srv-$set/" 2>/dev/null
    "$R/tools/winvm-servers.sh" stop-all > /dev/null
    RESULT[net-$set]="rc=$RC"
  done
fi

if want smb; then
  if false; then :
  else
    json_smb "$W/shared/pkg/servers-smb.json"; json_smb "$W/shared/pkg/servers-smb-drive.json" 'Y:'
    gj 07-at-smb 3600 "set TAG=smbreal" "set ARGS=--seed $SEED --only smbreal --skip real_net_drives_lists_the_mapped_drive --servers $GS\\pkg\\servers-smb.json" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
    gj 07-at-drive 600 "net use Y: \\\\$SMB1\\vm2b \"Sm8 Real&Pw!42\" /user:vm2buser /persistent:yes" 'net use' "set TAG=smbdrive" "set ARGS=--seed $SEED --only real_net_drives_lists_the_mapped_drive --servers $GS\\pkg\\servers-smb-drive.json" 'call C:\vm2b\guest\autotest.cmd' 'set RC=%ERRORLEVEL%' 'net use Y: /delete /y' 'exit /b %RC%' > /dev/null; RC2=$?
    for t in smbreal smbdrive; do L=$RES/autotest-$t.txt; [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  $t: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP' | sed 's/^/    /' | tee -a "$SUM"; }; done
    gj 07-smbinfo 300 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vm2b\\guest\\smbinfo.ps1 \\\\$SMB1\\vm2b \\\\$SMB1\\vm2benc vm2buser \"Sm8 Real&Pw!42\" \\\\$SMB2\\vm2b vm2bro \"Ro-Only-Pw!42\"" > /dev/null; RC3=$?
    tr -d '\r' < "$RES/07-smbinfo.out" | grep -aE '^(PASS|FAIL|INFO)|Dialect|SMB' | tee -a "$SUM"
    tally smbinfo $RC3 07-smbinfo
    RESULT[smb]="smbreal rc=$RC, drives rc=$RC2, smbinfo rc=$RC3"
  fi
fi

if want smbsign; then
  json_smb "$W/shared/pkg/servers-smb.json"
  gj 14-smbsign-on 900 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vm2b\\guest\\smbsign.ps1 on \\\\$SMB1\\vm2b vm2buser \"Sm8 Real&Pw!42\" $SMBCTL" > /dev/null; RC=$?
  gj 14-at-smbsign 3600 "set TAG=smbsign" "set ARGS=--seed $SEED --only smbreal --skip real_net_drives_lists_the_mapped_drive,real_unreachable_server_is_transient --servers $GS\\pkg\\servers-smb.json" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC2=$?
  gj 14-smbsign-off 900 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vm2b\\guest\\smbsign.ps1 off \\\\$SMB1\\vm2b vm2buser \"Sm8 Real&Pw!42\" $SMBCTL" > /dev/null; RC3=$?
  L=$RES/autotest-smbsign.txt; [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  smbsign (signing required): /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL' | sed 's/^/    /' | tee -a "$SUM"; }
  tr -d '\r' < "$RES/14-smbsign-on.out" | grep -aE '^(INFO|PASS|FAIL)|Signed|signed' | head -20 | tee -a "$SUM"; tr -d '\r' < "$RES/14-smbsign-off.out" | grep -aE '^(INFO|PASS|FAIL)' | head -10 | tee -a "$SUM"
  tally smbsign $(( RC + RC2 + RC3 )) 14-smbsign-on 14-smbsign-off
fi

if want svcacct; then
  gj 13-svcacct 3600 "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\vm2b\\guest\\svcacct.ps1 \\\\$SMB1\\vm2b vm2buser \"Sm8 Real&Pw!42\" \\\\$SMB2\\vm2b" > /dev/null; RC=$?
  tr -d '\r' < "$RES/13-svcacct.out" | grep -aE '^INFO' | head -40 | tee -a "$SUM"
  tally svcacct $RC 13-svcacct
fi

if want aclreal; then
  gj 13b-aclreal 1800 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\aclreal.ps1' > /dev/null; RC=$?
  tr -d '\r' < "$RES/13b-aclreal.out" | grep -aE '^INFO' | head -20 | tee -a "$SUM"
  tally aclreal $RC 13b-aclreal
fi

if want speedlimit; then
  gj 13c-speedlimit 3600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\speedlimit.ps1' > /dev/null; RC=$?
  tr -d '\r' < "$RES/13c-speedlimit.out" | grep -aE '^INFO case' | tee -a "$SUM"
  tally speedlimit $RC 13c-speedlimit
fi

if want restartjob; then
  gj 15-rj-start 1800 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\restartjob.ps1 start' > /dev/null; RC=$?
  reboot_guest 15-rj-reboot
  gj 15-rj-check 3600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\restartjob.ps1 check' > /dev/null; RC2=$?
  tally restartjob $(( RC + RC2 )) 15-rj-start 15-rj-check
fi

if want big; then
  gj 12-at-big 10800 "set TAG=big" "set VM2B_BIG=1" "set ARGS=--seed $SEED --only big" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
  L=$RES/autotest-big.txt; [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  big: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP|level|backup:|peak' | sed 's/^/    /' | tee -a "$SUM"; }
  RESULT[big]="rc=$RC"
fi

if want winbig; then
  gj 12b-at-winbig 10800 "set TAG=winbig" "set ARGS=--seed $SEED --only winbig" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
  L=$RES/autotest-winbig.txt; [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  winbig: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP|backup of|peak' | sed 's/^/    /' | tee -a "$SUM"; }
  RESULT[winbig]="rc=$RC"
fi

if want verify; then
  # the local groups again with the installed product present (no service / web groups: they would fight the installed service for its pipe and port 8443)
  gj 09-at-verify 5400 "set TAG=verify" "set ARGS=--seed $SEED --only core,engine,resume,restore,xz,smb,panel" "set VM2B_WEB_DEFAULT=0" 'call C:\vm2b\guest\autotest.cmd' > /dev/null; RC=$?
  L=$RES/autotest-verify.txt
  [ -f "$L" ] && { tr -d '\r' < "$L" | grep -aE ' passed, ' | tail -1 | sed "s/^/  verify: /" | tee -a "$SUM"; tr -d '\r' < "$L" | grep -aE '^FAIL|^SKIP' | sed 's/^/    /' | tee -a "$SUM"; }
  RESULT[verify]="rc=$RC"
fi

if want defender; then
  gj 16-defender 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\defender.ps1' > /dev/null; RC=$?
  tr -d '\r' < "$RES/16-defender.out" | grep -aE '^INFO' | head -10 | tee -a "$SUM"
  tally defender $RC 16-defender
fi

if want smartscreen; then
  gj 11-smartscreen 300 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\smartscreen.ps1' > /dev/null; RC=$?
  mkdir -p "$RES/shots"; "$W/shot.sh" "$RES/shots/smartscreen-host.png" >/dev/null 2>&1
  gj 11-smartscreen-kill 60 'taskkill /f /im VM2Backup-Setup.exe' 'taskkill /f /im smartscreen.exe' 'exit /b 0' > /dev/null
  RESULT[smartscreen]="rc=$RC (screenshots in shots/: what the user sees)"
fi

if want signing; then
  # ПОДПИСАННЫЙ установщик (docs/SIGNING.md, .winvm/guest/signing.ps1): четыре сочетания, что видит пользователь. Файл всегда с Mark of the Web (ZoneId=3).
  #   c) без подписи            a) подписан, корень НЕ доверен       b) подписан, корень в Root + издатель в TrustedPublisher      d) подписан + доверен, файл ИЗМЕНЁН после подписи
  # Снимки делает ХОСТ (qemu screendump): так виден и запрос UAC на безопасном рабочем столе. Тестовый сертификат: tools/make-test-cert.sh (.winvm/shared/pki).
  [ -f "$W/shared/pki/test-sign.cer" ] || bash "$R/tools/make-test-cert.sh" >/dev/null
  mkdir -p "$RES/shots/signing" "$W/shared/pki/variants"
  SG='powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\signing.ps1'
  sg() { local n=$1; shift; gj "20-sign-$n" 300 "$SG $*"; }
  sgshot() { "$W/shot.sh" "$RES/shots/signing/$1.png" >/dev/null 2>&1; }
  sg prep prep > /dev/null
  sg info-before info > /dev/null
  sg untrust untrust > /dev/null
  sg uac-on uac on > /dev/null; reboot_guest 20-sign-reboot-uac-on || say "the guest did not return after the restart (UAC on)"
  signing_case() { # signing_case <метка> <вариант>: файл с Mark of the Web, двойной щелчок; окно SmartScreen -> «Подробнее» -> «Выполнить в любом случае» -> окно «Открыть файл» -> «Запустить» -> UAC; состояние, события, уборка
    local tag=$1 var=$2
    sg "$tag-launch" launch $var > /dev/null
    sleep 20; sgshot "$tag-1-smartscreen"
    sg "$tag-more" click more 8 > /dev/null; sleep 2; sgshot "$tag-2-more-info"
    sg "$tag-run" click run 6 > /dev/null; sleep 5; sgshot "$tag-3-after-run-anyway"
    sg "$tag-runfile" click runfile 12 > /dev/null; sleep 12; sgshot "$tag-4-uac-or-setup"
    sg "$tag-state" state > /dev/null
    sg "$tag-events" events 8 > /dev/null
    sg "$tag-kill" kill > /dev/null; sleep 4; sgshot "$tag-5-cleaned"
    for k in more run runfile; do echo "  $tag $k: $(tr -d '\r' < "$RES/20-sign-$tag-$k.out" | grep -aE '^INFO' | head -1)" >> "$RES/signing-observed.txt"; done
  }
  signing_case c-unsigned unsigned
  signing_case a-signed-untrusted signed
  sg trust trust > /dev/null; sg info-after-trust info > /dev/null
  signing_case b-signed-trusted signed
  signing_case d-tampered-trusted tampered
  sg uac-reset uac reset > /dev/null; reboot_guest 20-sign-reboot-uac-off || true
  for f in 20-sign-info-before 20-sign-info-after-trust 20-sign-trust; do tr -d '\r' < "$RES/$f.out" | grep -aE '^INFO' | tee -a "$SUM"; done
  for t in c-unsigned a-signed-untrusted b-signed-trusted d-tampered-trusted; do echo "--- $t" | tee -a "$SUM"; tr -d '\r' < "$RES/20-sign-$t-launch.out" | grep -aE '^INFO' | tee -a "$SUM"; for k in state events; do tr -d '\r' < "$RES/20-sign-$t-$k.out" 2>/dev/null | grep -aE '^INFO' | head -12 | tee -a "$SUM"; done; done
  RESULT[signing]="informational (20-sign-*.out, shots/signing/*.png)"
fi

if want sac; then
  # Smart App Control: ПОСЛЕДНЯЯ фаза (включённый SAC нельзя выключить без сброса Windows; ВМ удаляется сразу после). Включение - через настоящий интерфейс
  # «Безопасность Windows» (переключатель On, UI Automation), потом проверка вариантов установщика и продукта (guest/sac.ps1), без доверия к тестовому корню и с ним.
  mkdir -p "$RES/shots"
  gj 99-sac-info 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 info' > /dev/null
  gj 99-sac-enable 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 enable' > /dev/null
  tr -d '\r' < "$RES/99-sac-enable.out" | grep -aE '^INFO' | tee -a "$SUM"
  "$W/shot.sh" "$RES/shots/sac-host-after-enable.png" >/dev/null 2>&1
  reboot_guest 99-sac-reboot || "$W/shot.sh" "$RES/shots/sac-no-return.png" >/dev/null 2>&1
  gj 99-sac-check 1800 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 check' > /dev/null; RC=$?
  "$W/shot.sh" "$RES/shots/sac-host.png" >/dev/null 2>&1
  tr -d '\r' < "$RES/99-sac-check.out" | grep -aE '^(INFO|PASS|FAIL)' | tee -a "$SUM"
  gj 99-sac-trust 600 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 trust' > /dev/null
  reboot_guest 99-sac-reboot2 || true
  gj 99-sac-check-trusted 1800 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 check-trusted' > /dev/null
  "$W/shot.sh" "$RES/shots/sac-host-trusted.png" >/dev/null 2>&1
  tr -d '\r' < "$RES/99-sac-check-trusted.out" | grep -aE '^(INFO|PASS|FAIL)' | tee -a "$SUM"
  # пробы: сеть у гостя, служба после перезагрузок, свежесобранные неподписанные exe (облачное решение SAC не всегда одинаково), неподписанные exe сборки, события блокировки
  gj 99-sac-probe 900 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 probe' > /dev/null
  gj 99-sac-probe2 900 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sac.ps1 probe2' > /dev/null
  gj 99-sac-service 300 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\vm2b\guest\sacscm.ps1' > /dev/null
  for f in 99-sac-probe 99-sac-probe2 99-sac-service; do tr -d '\r' < "$RES/$f.out" | grep -aE '^INFO' | grep -v 'no events' | cut -c1-300 | tee -a "$SUM"; done
  RESULT[sac]="rc=$RC (informational: 99-sac-*.out, shots/sac-*.png)"
fi

# ---------------------------------------------------------------- summary
say "================ SUMMARY ($OS, seed $SEED, $(( $(date +%s) - T0 ))s) ================"
for k in "${!RESULT[@]}"; do echo "  $k: ${RESULT[$k]}" | tee -a "$SUM"; done
# код возврата задания не говорит о числе упавших тестов (последняя команда пакетного файла): итог берём из самих журналов autotest-*.txt
for f in "$RES"/autotest-*.txt; do [ -f "$f" ] && echo "  log $(basename "$f" .txt | sed 's/^autotest-//'): $(tr -d '\r' < "$f" | grep -aE ' passed, ' | tail -1)" | tee -a "$SUM"; done
cp "$SUM" "$RES/summary.txt" 2>/dev/null
if [ $KEEP = 0 ]; then "$W/destroy.sh" "$OS"; else say "VM kept (--keep): delete it with .winvm/destroy.sh $OS"; fi
