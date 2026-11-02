#!/usr/bin/env python3
# Негативный контроль детектора (check_disk.py): он обязан ловить несогласованные образы, иначе «0 нарушений» ничего не доказывает.
#   negative.py <образ A (снят раньше)> <образ B (снят позже)> <рабочая папка>
# Проверки: 1) A и B по отдельности согласованы; 2) «склейка во времени» (первая половина A + вторая B) ловится; 3) один испорченный бит / обнулённый блок / потерянная
# (старая) запись в блоке ловятся поштучно; 4) одна «просочившаяся» запись ПОСЛЕ снимка (блок из B с seq > M_A + 3) ловится: пропущенные до неё записи видны как потерянные;
# 5) образ A против эталона «состояние в конце копии» (M_B): число несовпавших блоков.  Печатает строки NEG ... и итог; код 0 = детектор не пропускает ничего из этого.
import sys, os, shutil, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import check_disk as cd
BS = 4096
a, b, w = sys.argv[1:4]
os.makedirs(w, exist_ok=True)
fails = 0
def rep(name, r, expect):
    global fails
    ok = expect(r); fails += 0 if ok else 1
    print('NEG %-46s M=%d violations=%d torn=%d zero=%d stale=%d inflight=%d -> %s' % (name, r['M'], r['violations'], r['torn'], r['zero'], r['stale'], r['inflight'], 'caught' if ok and 'must' in name else ('ok' if ok else 'MISSED/WRONG')))
def patch(src, dst, edits):
    shutil.copyfile(src, dst)
    with open(dst, 'r+b') as f:
        for off, data in edits:
            f.seek(off); f.write(data)
ra = cd.analyse(a); rb = cd.analyse(b)
rep('A alone', ra, lambda r: r['violations'] == 0); rep('B alone', rb, lambda r: r['violations'] == 0)
print('NEG M_A=%d M_B=%d (B снят позже на %d записей)' % (ra['M'], rb['M'], rb['M'] - ra['M']))
half = (ra['blocks'] // 2) * BS
with open(a, 'rb') as fa, open(b, 'rb') as fb: da = fa.read(half); fb.seek(half); db = fb.read()
open(w + '/splice.img', 'wb').write(da + db)
rep('must: splice A[0:half]+B[half:]', cd.analyse(w + '/splice.img'), lambda r: r['violations'] > 0)
# блок, отличающийся у A и B (в B перезаписан после снимка A)
differ = np.nonzero((ra['seq'] != rb['seq']))[0]; differ = differ[differ >= 1]
X = int(differ[len(differ) // 2])
fa = open(a, 'rb'); fb = open(b, 'rb')
fa.seek(X * BS); blkA = fa.read(BS); fb.seek(X * BS); blkB = fb.read(BS)
flipped = bytearray(blkA); flipped[1000] ^= 0x01
patch(a, w + '/bitflip.img', [(X * BS, bytes(flipped))]); rep('must: one flipped bit in block %d of A' % X, cd.analyse(w + '/bitflip.img'), lambda r: r['torn'] == 1 and r['violations'] == 1)
patch(a, w + '/zero.img', [(X * BS, bytes(BS))]); rep('must: block %d of A zeroed' % X, cd.analyse(w + '/zero.img'), lambda r: r['zero'] == 1 and r['violations'] == 1)
patch(b, w + '/stale.img', [(X * BS, blkA)]); rep('must: B with block %d reverted to its older state (lost write)' % X, cd.analyse(w + '/stale.img'), lambda r: r['stale'] >= 1 and r['violations'] >= 1)
cand = [int(i) for i in differ if int(rb['seq'][i]) > ra['M'] + 3 and rb['st'][i] == 1]
Y = min(cand, key=lambda i: int(rb['seq'][i]))
fb.seek(Y * BS); blkY = fb.read(BS)
patch(a, w + '/leak.img', [(Y * BS, blkY)]); r = cd.analyse(w + '/leak.img')
print('NEG leaked write: block %d, seq %d (M_A=%d)' % (Y, int(rb['seq'][Y]), ra['M']))
rep('must: A + ONE write made after the snapshot (seq M_A+%d)' % (int(rb['seq'][Y]) - ra['M']), r, lambda r: r['violations'] > 0)
d = cd.diff_vs_expected(ra, rb['M'])
print('NEG A against the expected state at M_B (reference taken later): %d blocks differ -> %s' % (d, 'caught' if d > 0 else 'MISSED'))
fails += 0 if d > 0 else 1
for f in os.listdir(w): os.remove(os.path.join(w, f))
print('NEG result: %s' % ('detector OK (all %s caught)' % 'bad cases' if fails == 0 else '%d checks failed' % fails))
sys.exit(1 if fails else 0)
