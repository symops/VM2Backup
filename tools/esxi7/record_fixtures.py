#!/usr/bin/env python3
# Из следов обмена с НАСТОЯЩИМ ESXi (VM2B_ESXI_TRACE=<папка> при прогоне tools/esxi7/run-real.sh) делает образцы для tests/fixtures/esxi7-real/:
# выбирает по одному ответу каждого вида и убирает всё, что не должно попасть в git (идентификаторы сессий, UUID, MAC, серийные номера, имена хостов).
#   tools/esxi7/record_fixtures.py <папка следов> <папка назначения>
import os, re, sys
src, dst = sys.argv[1:3]
os.makedirs(dst, exist_ok=True)
UUID = re.compile(r'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')
HEX16 = re.compile(r'(?<![0-9a-f])(?:[0-9a-f]{2} ){15}[0-9a-f]{2}(?![0-9a-f])')
MAC = re.compile(r'(?<![0-9a-fA-F:])(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}(?![0-9a-fA-F:])')
SERIAL = re.compile(r'[A-Z0-9]{5}(?:-[A-Z0-9]{5}){4}')
def scrub(t):
    t = re.sub(r'session\[[^\]]*\][0-9a-f-]*', 'session[SESSION]', t)
    t = re.sub(r'<key>[0-9a-f-]{36}</key>', '<key>SESSION</key>', t)
    n = {}
    def u(m):
        n.setdefault(m.group(0).lower(), '00000000-0000-4000-8000-%012d' % len(n)); return n[m.group(0).lower()]
    t = UUID.sub(u, t)
    t = HEX16.sub('42 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00', t)
    t = MAC.sub('00:50:56:00:00:01', t)
    t = SERIAL.sub('XXXXX-XXXXX-XXXXX-XXXXX-XXXXX', t)
    t = re.sub(r'(<(?:userName|fullName|loginTime|lastActiveTime)>)[^<]*', r'\1X', t)
    return t
names = sorted(os.listdir(src))
want = {}
for f in names:
    m = re.match(r'(\d+)-(\w+)\.(req|resp)\.xml$', f)
    if not m: continue
    n, meth, kind = m.groups()
    body = open(os.path.join(src, f), encoding='utf-8', errors='replace').read()
    if kind == 'resp':
        tag = meth
        if 'Fault' in body and '<soapenv:Fault>' in body:
            mm = re.search(r'xsi:type="(\w+)"', body.split('<detail>')[-1]) if '<detail>' in body else None
            tag = meth + '-fault-' + (mm.group(1) if mm else 'unknown')
        elif meth == 'RetrievePropertiesEx':
            tag = meth + ('-layoutEx' if '<name>layoutEx</name>' in body else '-layoutEx-list' if 'layoutEx.file' in body else '-snapshot' if 'VirtualMachineSnapshotInfo' in body else
                          '-task' if 'TaskInfo' in body else '-datacenter' if 'Datacenter' in body else '-vmlist')
        want.setdefault(tag, (n, body))
for tag, (n, body) in sorted(want.items()):
    open(os.path.join(dst, tag + '.resp.xml'), 'w', encoding='utf-8').write(scrub(body))
    print(tag)
