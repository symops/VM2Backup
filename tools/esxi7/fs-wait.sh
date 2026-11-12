#!/bin/bash
# Ждёт, пока гость vm2b-fs загрузится: Tools работают (guestToolsRunning) и писатель идёт (в serial строка «W 40»).  fs-wait.sh [имя ВМ] [таймаут, с]
D=$(cd "$(dirname "$0")" && pwd); VM=${1:-vm2b-fs}; TO=${2:-1500}; T0=$(date +%s)
ID=$($D/ssh.sh "vim-cmd vmsvc/getallvms | grep ' $VM ' | cut -d' ' -f1" | tail -1)
while [ $(( $(date +%s) - T0 )) -lt $TO ]; do
  OK=$($D/ssh.sh "vim-cmd vmsvc/get.guest $ID | grep -c 'toolsRunningStatus = \"guestToolsRunning\"'; grep -c '^W 40' /vmfs/volumes/datastore1/vm2b-serial/$VM.log" 2>/dev/null | tr '\n' ' ')
  [ "$OK" = "1 1 " ] && { echo "готов ($(( $(date +%s) - T0 )) с)"; exit 0; }
  sleep 10
done
echo "не дождались"; exit 1
