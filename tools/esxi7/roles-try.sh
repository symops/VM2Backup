#!/bin/bash
# Раунд 3, пункт D: один прогон продукта через vCenter от имени пользователя (roles.py user ...): roles-try.sh <пользователь> <тег> '<json esxi_fs>'
# Печатает итог копии и текст ошибки продукта (в нём названо недостающее право).
D=$(cd "$(dirname "$0")" && pwd); U=$1; TAG=$2; J=$3
export VM2B_REAL_KEY=vcenter_real VC_USER="$U@vsphere.local" VC_SECRET="secret-vc-$U"
timeout ${TRY_TIMEOUT:-170} "$D/fs-run.sh" esxireal_guest_fs_copy "$TAG" "$J" 2>&1 | grep -E "FSRUN|PASS|FAIL|ошибка|login|lacks|privilege" | cut -c1-${TRY_COLS:-1500}
