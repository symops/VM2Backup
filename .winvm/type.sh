#!/bin/bash
# type.sh "text" [enter]: types ASCII text into the guest through the qemu monitor (debug helper for a stuck executor)
T=$1
M='S=/tmp/m.sock; [ -S $S ] || S=/run/shm/monitor.sock; '
cmds=""
for ((i=0;i<${#T};i++)); do c=${T:$i:1}
  case "$c" in
    [a-z0-9]) k=$c;; [A-Z]) k="shift-$(echo $c | tr A-Z a-z)";; ' ') k=spc;; '\') k=backslash;; '.') k=dot;; ':') k=shift-semicolon;; '/') k=slash;; '-') k=minus;; '_') k=shift-minus;; '"') k=shift-apostrophe;; '>') k=shift-dot;; '%') k=shift-5;; '*') k=shift-8;; '&') k=shift-7;; '=') k=equal;; ',') k=comma;; *) k=;;
  esac
  [ -n "$k" ] && cmds+="echo 'sendkey $k' | nc -U -q0 \$S >/dev/null; sleep 0.08; "
done
[ "${2:-}" = enter ] && cmds+="echo 'sendkey ret' | nc -U -q0 \$S >/dev/null; "
docker exec vm2b-win sh -c "$M $cmds"
