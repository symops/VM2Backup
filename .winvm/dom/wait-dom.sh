#!/bin/bash
# wait-dom.sh <dc|member> [секунды=7200]: ждёт, пока исполнитель гостя снова отвечает (установка закончена, bootstrap перезагрузил Windows, poller запущен при автовходе).
S=/home/symo/Claude/VM2Backup/.winvm/dom/shared-$1/results; TO=${2:-7200}; T0=$(date +%s)
while true; do
  if [ -f $S/executor-ready.txt ] && [ -f $S/poller-alive.txt ] && [ $S/poller-alive.txt -nt $S/executor-ready.txt ]; then echo "$1: executor ready after $(( $(date +%s) - T0 ))s"; exit 0; fi
  [ $(( $(date +%s) - T0 )) -gt "$TO" ] && { echo "$1: TIMEOUT waiting for the executor"; exit 1; }
  sleep 10
done
