#!/bin/bash
# SSH на вложенный ESXi (порт 18022 на 127.0.0.1); пароль читается из .winvm/esxi/secret (sshpass -f), в командной строке его нет.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec sshpass -f "$ROOT/.winvm/esxi/secret" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20 root@10.213.77.10 "$@"
