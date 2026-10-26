#!/bin/bash
# Waits (up to N seconds, default 5400) until the guest's executor answers: the install is over, the bootstrap restarted Windows and the poller runs
# again at the autologon (poller-alive.txt newer than executor-ready.txt).
cd "$(dirname "$0")" || exit 1
TO=${1:-5400}; T0=$(date +%s)
while true; do
  if [ -f shared/results/executor-ready.txt ] && [ -f shared/results/poller-alive.txt ] && [ shared/results/poller-alive.txt -nt shared/results/executor-ready.txt ]; then echo "executor ready after $(( $(date +%s) - T0 ))s"; exit 0; fi
  [ $(( $(date +%s) - T0 )) -gt "$TO" ] && { echo "TIMEOUT waiting for the executor"; exit 1; }
  sleep 10
done
