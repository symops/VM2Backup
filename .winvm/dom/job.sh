#!/bin/bash
# Одно задание на госте: job.sh <dc|member> <имя> <файл.cmd | -> [таймаут=600]  (- = скрипт со стандартного ввода).  Как .winvm/job.sh, но у каждой ВМ своя папка.
D=/home/symo/Claude/VM2Backup/.winvm/dom
R=$1; N=$2; SRC=$3; TO=${4:-600}; S=$D/shared-$R
[ -n "$R" ] && [ -n "$N" ] && [ -n "$SRC" ] || { echo "usage: job.sh dc|member <name> <file.cmd|-> [timeout]"; exit 2; }
mkdir -p "$S/jobs" "$S/results"; rm -f "$S/results/$N.rc" "$S/results/$N.out"
if [ "$SRC" = - ]; then cat; else cat "$SRC"; fi | sed 's/\r$//' | sed 's/$/\r/' > "$S/jobs/.$N.tmp"
mv "$S/jobs/.$N.tmp" "$S/jobs/$N.cmd"
T0=$(date +%s)
while [ ! -f "$S/results/$N.rc" ]; do
  [ $(( $(date +%s) - T0 )) -gt "$TO" ] && { echo "TIMEOUT after ${TO}s; output so far:"; tr -d '\r' < "$S/results/$N.out" 2>/dev/null | iconv -f CP866 -t UTF-8 -c | tail -30; exit 124; }
  sleep 2
done
sleep 1
tr -d '\r' < "$S/results/$N.out" | iconv -f CP866 -t UTF-8 -c 2>/dev/null || true
RC=$(tr -d '\r\n ' < "$S/results/$N.rc"); echo "[job $R/$N rc=$RC, $(( $(date +%s) - T0 ))s]"
case "$RC" in ''|*[!0-9]*) exit 1;; esac
exit "$RC"
