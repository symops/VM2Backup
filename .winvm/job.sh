#!/bin/bash
# Runs one job on the guest and prints its output.   job.sh <name> <file.cmd | -> [timeout-seconds=600]   (- = the script from stdin)
# The job is a cmd batch file (made CRLF); the guest's poller (a scheduled task at logon, in the interactive session) runs it elevated.
# Exit code = the job's (errorlevel of its last command), 124 = timeout.  Output is also kept in shared/results/<name>.out.
cd "$(dirname "$0")" || exit 1
N=$1; SRC=$2; TO=${3:-600}
[ -n "$N" ] && [ -n "$SRC" ] || { echo "usage: job.sh <name> <file.cmd|-> [timeout]"; exit 2; }
mkdir -p shared/jobs shared/results
rm -f "shared/results/$N.rc" "shared/results/$N.out"
if [ "$SRC" = - ]; then cat; else cat "$SRC"; fi | sed 's/\r$//' | sed 's/$/\r/' > "shared/jobs/.$N.tmp"
mv "shared/jobs/.$N.tmp" "shared/jobs/$N.cmd"
T0=$(date +%s)
while [ ! -f "shared/results/$N.rc" ]; do
  [ $(( $(date +%s) - T0 )) -gt "$TO" ] && { echo "TIMEOUT after ${TO}s; output so far:"; tr -d '\r' < "shared/results/$N.out" 2>/dev/null | tail -30; exit 124; }
  sleep 2
done
sleep 1
tr -d '\r' < "shared/results/$N.out" | iconv -f CP866 -t UTF-8 -c 2>/dev/null || true      # the guest's console is CP866 (Russian): readable output; the raw .out stays as it is
RC=$(tr -d '\r\n ' < "shared/results/$N.rc"); echo "[job $N rc=$RC, $(( $(date +%s) - T0 ))s]"
case "$RC" in ''|*[!0-9]*) exit 1;; esac
exit "$RC"
