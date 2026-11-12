#!/bin/bash
# Раунд 3, пункт A: разбор диска данных ext4 из восстановленной копии (нужен root: loop-монтирование).
#   tools/esxi7/fs-analyse.sh <каталог восстановленной ВМ> [имя диска данных, по умолчанию vm2b-fs_1-flat.vmdk]
# Печатает: признаки ФС (needs_recovery, state), e2fsck -fn, проверку файлов fsw БЕЗ воспроизведения журнала (ro,noload) и ПОСЛЕ него (копия смонтирована rw), строку восстановления журнала из dmesg.
D=$(cd "$(dirname "$0")" && pwd); T=${TMPDIR:-/home/symo/tmp-esxi7f}; SRC=$1; IMG=${2:-vm2b-fs_1-flat.vmdk}
gcc -O2 -static -o "$T/fsw" "$D/guest/fsw.c" 2>/dev/null
[ -f "$SRC/$IMG" ] || { echo "нет $SRC/$IMG"; exit 1; }
echo "meta: $(grep -o '"snapshot_quiesce": *[a-z]*\|"live_clone": *[a-z]*\|"power_state": *"[A-Za-z ]*"' "$SRC"/vm2backup-meta.json 2>/dev/null | tr '\n' ' ')"
dumpe2fs -h "$SRC/$IMG" 2>/dev/null | grep -E "^Filesystem (features|state)|^Last mount|^Journal (features|sequence)" | sed 's/^/sb: /'
echo "needs_recovery: $(dumpe2fs -h "$SRC/$IMG" 2>/dev/null | grep -c 'needs_recovery')   (1 = в копии есть НЕвоспроизведённый журнал: crash-consistent; 0 = ФС чистая)"
echo "fsck -n: $(e2fsck -fn "$SRC/$IMG" 2>&1 | tail -1)   (rc=${PIPESTATUS[0]})"
M=$T/fsmnt; mkdir -p $M
L=$(losetup -f --show -r "$SRC/$IMG") && mount -o ro,noload $L $M && { echo "RAW (без воспроизведения журнала): $($T/fsw verify $M/ )"; umount $M; }; losetup -d $L 2>/dev/null
cp "$SRC/$IMG" "$T/fsimg-copy.raw"; L=$(losetup -f --show $T/fsimg-copy.raw) && dmesg -C >/dev/null 2>&1; mount -o rw $L $M && { echo "dmesg: $(dmesg | grep -iE 'recover|orphan|EXT4-fs \(loop' | tail -3 | tr '\n' '|')"; echo "REPLAYED (после воспроизведения журнала): $($T/fsw verify $M/ )"; umount $M; }; losetup -d $L 2>/dev/null
rm -f "$T/fsimg-copy.raw"
