#!/usr/bin/env python3
"""Раунд 13: архив, сделанный 64-БИТНОЙ службой (x64, под Wine), для чтения 32-битной службой на настоящей Windows (фаза levels, режим foreign).
   python3 -I tools/make_x64_archive.py <выходная папка> [уровень xz, по умолчанию 7]
   В <выходная папка>/store лежит каталог ВМ с manifest и архивом; <выходная папка>/files.sha1 - «путь SHA-1» по каждому исходному файлу (после restore гость сверяет).
   Данные: журналоподобный текст, исполняемые файлы обеих сборок (повторы далеко друг от друга), случайные байты."""
import hashlib, json, os, random, shutil, subprocess, sys, time
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
OUT = os.path.abspath(sys.argv[1]); LEVEL = int(sys.argv[2]) if len(sys.argv) > 2 else 7
shutil.rmtree(OUT, ignore_errors=True); os.makedirs(OUT + '/ctl'); os.makedirs(OUT + '/data'); os.makedirs(OUT + '/src/vmX'); os.makedirs(OUT + '/store')
def wp(p): return 'Z:' + p.replace('/', '\\')
rnd = random.Random(13); words = [''.join(rnd.choice('abcdefghijklmnopqrstuvwxyz') for _ in range(3 + rnd.randrange(9))) for _ in range(3000)]
with open(OUT + '/src/vmX/app.log', 'w') as f:
    t = 0
    for _ in range(300000):
        f.write('2026-11-08 %02d:%02d:%02d INFO %s[%x] %s\n' % (t // 3600 % 24, t // 60 % 60, t % 60, words[rnd.randrange(300)], rnd.randrange(65536), ' '.join(words[rnd.randrange(3000)] for _ in range(6 + rnd.randrange(14)))))
        if rnd.randrange(3) == 0: t += 1
for a in ('x64', 'x86'):
    for n in ('VM2Backup.exe', 'VM2BackupService.exe', 'vm2bautotest.exe'): shutil.copy('%s/build/%s/%s' % (ROOT, a, n), '%s/src/vmX/%s-%s' % (OUT, a, n))
with open(OUT + '/src/vmX/random.bin', 'wb') as f: f.write(rnd.randbytes(24 << 20))
with open(OUT + '/src/vmX/disk-flat.vmdk', 'wb') as f:                      # «диск ВМ»: пустые места и повторяющиеся блоки далеко друг от друга
    blk = rnd.randbytes(1 << 20)
    for i in range(40): f.write(blk if i % 5 == 0 else bytes(1 << 20) if i % 5 == 1 else rnd.randbytes(64 << 10) + bytes((1 << 20) - (64 << 10)))
with open(OUT + '/files.sha1', 'w') as f:
    for n in sorted(os.listdir(OUT + '/src/vmX'), key=str.lower): f.write('%s %s\n' % (n.lower(), hashlib.sha1(open(OUT + '/src/vmX/' + n, 'rb').read()).hexdigest()))
json.dump({'web': {'ctl_dir': wp(OUT + '/ctl'), 'data_dir': wp(OUT + '/data'), 'kdf': 2000}}, open(OUT + '/servers.json', 'w'))
env = dict(os.environ, WINEDEBUG='-all')
SVC = os.environ.get('VM2B_ARCHIVE_ARCH', 'x64')      # раунд 14: архив можно сделать и 32-битной службой (x86 под Wine) для чтения 64-битной на настоящей Windows
host = subprocess.Popen(['wine64' if SVC == 'x64' else 'wine', ROOT + '/build/%s/vm2bautotest.exe' % SVC, '--servers', OUT + '/servers.json', '--only', 'host_for_python_clients'], env=env, stdout=open(OUT + '/wine.log', 'w'), stderr=subprocess.STDOUT)
for _ in range(600):
    if os.path.exists(OUT + '/ctl/ready.txt'): break
    time.sleep(0.2)
else: sys.exit('host did not start')
seq = [0]
def core(cmd, **a):
    seq[0] += 1; rid = 'm%d' % seq[0]
    json.dump({'core': cmd, 'args': a}, open(OUT + '/ctl/tmp-%s.json' % rid, 'w')); os.rename(OUT + '/ctl/tmp-%s.json' % rid, OUT + '/ctl/req-%s.json' % rid)
    for _ in range(36000):
        if os.path.exists(OUT + '/ctl/res-%s.json' % rid): time.sleep(0.05); r = json.load(open(OUT + '/ctl/res-%s.json' % rid)); os.remove(OUT + '/ctl/res-%s.json' % rid); return r
        time.sleep(0.05)
    raise SystemExit('timeout ' + cmd)
core('settings.set', level=LEVEL, threads=0)
sv = core('servers.save', server={'type': 'directory', 'name': 'x64 src', 'path': wp(OUT + '/src')}); sid = sv['result']['id']
el = {'source': {'server': sid, 'vms': ['vmX']}, 'target': {'server': 'local', 'directory': wp(OUT + '/store') + '\\{VM}'}, 'compress': {'codec': 'xz'}, 'encrypt': {'enabled': False}, 'verify': True, 'keep': 0, 'segment_mib': 0}
tk = core('tasks.save', task={'name': 'x64-archive', 'enabled': True, 'elements': [el]}); tid = tk['result']['id']
job = core('tasks.run', id=tid)['result']['job']
for _ in range(3000):
    js = core('jobs.list')['result']; mine = [j for j in js if j['id'] == job]
    if not mine or mine[0]['status'] not in ('Waiting', 'Running'): break
    time.sleep(1)
h = core('history.list', limit=5)['result']; print('history:', [(x['name'], x['status']) for x in h])
core('', quit=True) if False else None
open(OUT + '/ctl/req-quit.json', 'w').write('{"quit":true}')
try: host.wait(timeout=30)
except Exception: host.kill()
print('archive:', [(r, os.path.getsize(os.path.join(r, f))) for r, _, fs in os.walk(OUT + '/store') for f in fs])
