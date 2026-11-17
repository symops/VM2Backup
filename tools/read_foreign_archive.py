#!/usr/bin/env python3
"""Раунд 16: шифрованный архив V2BENC4, сделанный 32-битной службой на настоящей Windows (guest/links.ps1 crypt, копия в .winvm-results/.../x86enc),
   читает 64-битная служба под Wine: list, verify, restore, сверка SHA-1, неверный пароль.
   python3 -I tools/read_foreign_archive.py <рабочая папка> <папка x86enc с store, password.txt, files.sha1> [x64|x86]"""
import hashlib, json, os, shutil, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OUT = os.path.abspath(sys.argv[1]); SRC = os.path.abspath(sys.argv[2]); SVC = sys.argv[3] if len(sys.argv) > 3 else 'x64'
shutil.rmtree(OUT, ignore_errors=True); os.makedirs(OUT + '/ctl'); os.makedirs(OUT + '/data'); shutil.copytree(SRC, OUT + '/arch')
def wp(p): return 'Z:' + p.replace('/', '\\')
pw = open(OUT + '/arch/password.txt', encoding='utf-8-sig').read().strip()
json.dump({'web': {'ctl_dir': wp(OUT + '/ctl'), 'data_dir': wp(OUT + '/data'), 'kdf': 2000}}, open(OUT + '/servers.json', 'w'))
env = dict(os.environ, WINEDEBUG='-all')
host = subprocess.Popen(['wine64' if SVC == 'x64' else 'wine', ROOT + '/build/%s/vm2bautotest.exe' % SVC, '--servers', OUT + '/servers.json', '--only', 'host_for_python_clients'], env=env, stdout=open(OUT + '/wine.log', 'w'), stderr=subprocess.STDOUT)
for _ in range(600):
    if os.path.exists(OUT + '/ctl/ready.txt'): break
    time.sleep(0.2)
else: sys.exit('host did not start')
seq = [0]
def core(cmd, **a):
    seq[0] += 1; rid = 'r%d' % seq[0]
    json.dump({'core': cmd, 'args': a}, open(OUT + '/ctl/tmp-%s.json' % rid, 'w')); os.rename(OUT + '/ctl/tmp-%s.json' % rid, OUT + '/ctl/req-%s.json' % rid)
    for _ in range(6000):
        if os.path.exists(OUT + '/ctl/res-%s.json' % rid): time.sleep(0.05); r = json.load(open(OUT + '/ctl/res-%s.json' % rid)); os.remove(OUT + '/ctl/res-%s.json' % rid); return r
        time.sleep(0.05)
    raise SystemExit('timeout ' + cmd)
def wait(job):
    for _ in range(1200):
        mine = [j for j in core('jobs.list')['result'] if j['id'] == job]
        if not mine or mine[0]['status'] not in ('Waiting', 'Running'): return mine[0]['status'] if mine else 'Gone'
        time.sleep(0.5)
    return 'timeout'
fails = 0
def check(name, ok, detail=''):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + name + ('' if ok else '  :: ' + str(detail)))
    if not ok: fails += 1
d = wp(OUT + '/arch/store/vmE')
bl = core('backups.list', server='local', directory=d); b = (bl.get('result', {}).get('backups') or [None])[0]
check('x86 encrypted archive is listed by the 64-bit service', b is not None, bl)
if b:
    print('INFO entry:', json.dumps(b)[:300])
    a = dict(server='local', directory=b['directory'], archive=b['archive'], password=pw)
    s = wait(core('backups.verify', **a)['result']['job']); check('verify on 64 bits', s in ('Success', 'Gone'), s)
    s = wait(core('backups.restore', dest=wp(OUT + '/rest'), **a)['result']['job']); check('restore on 64 bits', s in ('Success', 'Gone'), s)
    for line in open(OUT + '/arch/files.sha1', encoding='utf-8-sig').read().split('\n'):
        if not line.strip(): continue
        n, h = line.split(); p = OUT + '/rest/vmE/' + n
        if not os.path.exists(p):
            c = [x for x in os.listdir(OUT + '/rest/vmE') if x.lower() == n]; p = OUT + '/rest/vmE/' + c[0] if c else p
        check('restored %s equals the source' % n, os.path.exists(p) and hashlib.sha1(open(p, 'rb').read()).hexdigest() == h, p)
    a['password'] = 'wrong-' + pw; s = wait(core('backups.verify', **a)['result']['job']); check('wrong password refused', s not in ('Success', 'Gone'), s)
open(OUT + '/ctl/req-quit.json', 'w').write('{"quit":true}')
try: host.wait(timeout=30)
except Exception: host.kill()
print('FAILED=%d' % fails); sys.exit(fails)
