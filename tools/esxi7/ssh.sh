#!/bin/bash
# SSH на вложенный ESXi (по умолчанию 10.213.77.10; второй хост: ESXI_DIR=esxi2 ESXI_IP=10.213.77.12); пароль читается из .winvm/esxi/secret (sshpass -f), в командной строке его нет.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec sshpass -f "$ROOT/.winvm/${ESXI_DIR:-esxi}/secret" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20 root@${ESXI_IP:-10.213.77.10} "$@"
