#!/bin/bash
# Сценарий пользователя на настоящем ESXi: ВМ включена -> копия начата и поставлена на паузу -> ВМ выключают -> связь рвётся (qemu-соединение не трогаем, рвёмся паузой) -> ВМ включают ->
# продолжение копии / явная отмена.  Между этапами по SSH печатается состояние снимков и файлов ВМ (факты для docs/ESXI.md).   tools/esxi7/live-scenario.sh
D=$(cd "$(dirname "$0")" && pwd); S="$D/ssh.sh"; export NOPREP=1
st() { echo "--- $1"; $S 'ID=$(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1); vim-cmd vmsvc/power.getstate $ID | tail -1; vim-cmd vmsvc/snapshot.get $ID | grep -E "Name|Description" ; ls /vmfs/volumes/datastore1/vm2b-on | grep -E "vmdk|vmsn|vmsd|VM2Backup" | awk "{printf \"%s \", \$0}"; echo; for d in /vmfs/volumes/datastore1/vm2b-on/VM2Backup-*; do [ -d "$d" ] && echo "папка клонов: $(ls $d | awk "{printf \"%s \", \$0}")"; done; vim-cmd vmsvc/get.summary $ID | grep -i -E "consolidat" ' 2>&1 | cut -c1-200; }
run() { "$D/run-real.sh" --only "$1" 2>&1 | grep -E "этап|возобнов|PASS|FAIL|check" | cut -c1-300; }
$S 'vim-cmd vmsvc/snapshot.removeall $(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1) >/dev/null; vim-cmd vmsvc/power.on $(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1) >/dev/null 2>&1; rm -rf /vmfs/volumes/datastore1/vm2b-on/VM2Backup-*'; sleep 8
st "исходное состояние"
run esxireal_live_stage_pause; st "после паузы (ВМ включена, копия приостановлена)"
$S 'vim-cmd vmsvc/power.off $(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1) | tail -1'; sleep 5; st "после выключения ВМ при снимке"
$S 'vim-cmd vmsvc/power.on $(vim-cmd vmsvc/getallvms | grep " vm2b-on " | cut -d" " -f1) | tail -1'; sleep 8; st "после повторного включения"
run esxireal_live_stage_resume; st "после продолжения"
run esxireal_live_stage_cancel; st "после явной отмены"
