#!/usr/bin/env python3
# Скачивает пакеты Alpine v3.24 (main + community) с зеркала вместе с ЗАМЫКАНИЕМ зависимостей и с ПРОВЕРКОЙ:
# подпись APKINDEX.tar.gz (RSA-SHA1, ключ из initramfs образа Alpine), контрольная сумма пакета по индексу (C: = Q1 + base64(sha1 управляющего сегмента)) и размер.
# Результат - готовый локальный репозиторий: <куда>/main/x86_64/{APKINDEX.tar.gz,*.apk} и <куда>/community/x86_64/... (оригинальные подписанные индексы), apk внутри гостя ставит из него без сети.
#   fetch-apks.py <каталог ключей *.rsa.pub> <куда> <пакет> [<пакет> ...]
import sys, os, zlib, hashlib, base64, io, re, tarfile, subprocess, urllib.request
keys, out, *roots = sys.argv[1:]
def get(u):
    with urllib.request.urlopen(u, timeout=120) as r: return r.read()
def streams(b):
    res = []
    while b:
        d = zlib.decompressobj(31); data = d.decompress(b); used = len(b) - len(d.unused_data); res.append((b[:used], data)); b = d.unused_data
    return res
pk = {}; prov = {}; where = {}
for repo in ('main', 'community'):
    base = 'https://dl-cdn.alpinelinux.org/alpine/v3.24/%s/x86_64/' % repo
    idx = get(base + 'APKINDEX.tar.gz'); s = streams(idx)
    t = tarfile.open(fileobj=io.BytesIO(s[0][1])); m = t.getmembers()[0]; assert m.name.startswith('.SIGN.RSA.'), m.name
    open('/tmp/.idx.sig', 'wb').write(t.extractfile(m).read()); open('/tmp/.idx.body', 'wb').write(b''.join(x[0] for x in s[1:]))
    subprocess.check_call(['openssl', 'dgst', '-sha1', '-verify', os.path.join(keys, m.name[len('.SIGN.RSA.'):]), '-signature', '/tmp/.idx.sig', '/tmp/.idx.body'])
    os.remove('/tmp/.idx.sig'); os.remove('/tmp/.idx.body')
    os.makedirs(os.path.join(out, repo, 'x86_64'), exist_ok=True); open(os.path.join(out, repo, 'x86_64', 'APKINDEX.tar.gz'), 'wb').write(idx)
    txt = tarfile.open(fileobj=io.BytesIO(s[1][1])).extractfile('APKINDEX').read().decode()
    for blk in txt.split('\n\n'):
        d = dict(l.split(':', 1) for l in blk.split('\n') if ':' in l)
        if 'P' not in d: continue
        pk[d['P']] = d; where[d['P']] = (repo, base)
        for p in [d['P']] + d.get('p', '').split():
            prov.setdefault(re.split('[=<>~]', p)[0], d['P'])
need = []; seen = set(); todo = list(roots)
while todo:
    n = todo.pop()
    if n in seen: continue
    seen.add(n); need.append(n)
    for dep in pk[n].get('D', '').split():
        if dep.startswith('!'): continue
        nm = re.split('[=<>~]', dep)[0]
        if nm not in prov: print('нет провайдера для', dep, '(у', n + ')'); continue
        todo.append(prov[nm])
tot = 0
for n in sorted(need):
    d = pk[n]; repo, base = where[n]; fn = '%s-%s.apk' % (n, d['V']); b = get(base + fn); st = streams(b)
    ctl = st[1][0]
    assert 'Q1' + base64.b64encode(hashlib.sha1(ctl).digest()).decode() == d['C'], n
    assert len(b) == int(d['S']), n
    open(os.path.join(out, repo, 'x86_64', fn), 'wb').write(b); tot += len(b)
print('OK пакетов %d, %d байт' % (len(need), tot))
