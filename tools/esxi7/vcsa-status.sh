#!/bin/bash
# Ход первой загрузки VCSA: число выполненных шагов firstbootStatus.json (список {имя: время}) и последние имена; vcsa-status.sh
D=$(cd "$(dirname "$0")" && pwd)
timeout 40 $D/vcsa-ssh.sh 'python3 -c "
import json
d=json.load(open(\"/var/log/firstboot/firstbootStatus.json\"))
ks=[list(x.keys())[0] for x in d.get(\"firstboot\",d) if isinstance(x,dict)] if not isinstance(d,list) else [list(x.keys())[0] for x in d]
print(len(ks), ks[-2:], d.get(\"status\") if isinstance(d,dict) else \"\")"; ls /var/log/firstboot/*stderr.log | wc -l; (service-control --status --all 2>/dev/null | head -4)' 2>&1 | tail -4
