#!/bin/bash
# SSH на VCSA (10.213.77.11): пароль root читается из .winvm/esxi/secret-vc-root (sshpass -f), в командной строке его нет.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
exec sshpass -f "$ROOT/.winvm/esxi/secret-vc-root" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20 root@10.213.77.11 "$@"
