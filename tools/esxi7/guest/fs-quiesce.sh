#!/bin/sh
# Скрипт quiesce для open-vm-tools (вызывается vmbackup: freeze / freezeFail / thaw).  Пишет события на serial и в /data/q.log (тот же ext4: видно, в каком состоянии ФС).
# Флаг guestinfo.vm2b_failfreeze=1 (его ставит с хоста tools/esxi7/vm-guestinfo.py через ReconfigVM, гость не трогают): скрипт при freeze завершается с ошибкой (проверка отказа quiesce).
L=/dev/ttyS0
case "$1" in
freeze) echo "QUIESCE freeze $(date +%s.%N)" > $L; [ "$(vmware-rpctool "info-get guestinfo.vm2b_failfreeze" 2>/dev/null)" = 1 ] && { echo "QUIESCE freeze FAILED (guestinfo.vm2b_failfreeze=1)" > $L; exit 1; }; sync ;;
freezeFail) echo "QUIESCE freezeFail $(date +%s.%N)" > $L; grep -iE "vmbackup|sync|freez|quiesc|fail|error" /var/log/vmsvc.log 2>/dev/null | tail -12 | cut -c1-230 | sed "s/^/QLOG /" > $L ;;
thaw) echo "QUIESCE thaw $(date +%s.%N)" > $L ;;
*) echo "QUIESCE arg=$1" > $L ;;
esac
exit 0
