#!/usr/bin/env python3
# Независимая (не Си-код писателя) проверка образа диска данных гостя: «диск = ровно состояние после записей 1..M».
#   check_disk.py <flat-образ> [--window lo hi] [--ref <flat-образ эталона, снятый ПОЗЖЕ>] [--json]
# Формат блока и генератор описаны в tools/esxi7/guest/vm2bio.c.  numpy.  Печатает сводку, код возврата 0 = нарушений нет и M в окне (если задано).
import sys, zlib, json
import numpy as np

BS = 4096; G = 0x9E3779B97F4A7C15
U = np.uint64
def fin(z):
    z = z.astype(U, copy=True)
    z = (z ^ (z >> U(30))) * U(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> U(27))) * U(0x94D049BB133111EB)
    return z ^ (z >> U(31))
def mix(x):
    return fin(np.asarray(x, dtype=U) + U(G))

def lastseq(seedU, N, M):
    """для каждого блока: seq последней записи среди 1..M (0 = только заполнение) и блок записи M+1 (он «в полёте»)"""
    k = np.arange(1, M + 2, dtype=U)
    s = k >> U(6)
    seqmode = (mix(seedU ^ s) & U(3)) == U(0)
    bo = np.where(seqmode, U(1) + (s * U(64) + (k & U(63))) % U(N - 1), U(1) + mix(seedU ^ mix(k)) % U(N - 1)).astype(np.int64)
    last = np.zeros(N, np.uint64)
    last[bo[:M]] = k[:M]                    # позднее перезаписывает раннее (порядок k)
    return last, int(bo[M])

def analyse(path):
    with np.errstate(over='ignore'):
        return _analyse(path)

def _analyse(path):
    f = open(path, 'rb')
    sup = f.read(BS)
    if sup[:8] != b'VM2BSUPR':
        return {'error': 'no superblock'}
    seed = int.from_bytes(sup[8:16], 'little'); N = int.from_bytes(sup[16:24], 'little')
    seedU = U(seed)
    st = np.zeros(N, np.uint8); seq = np.zeros(N, np.uint64); tns = np.zeros(N, np.uint64)
    B = 2048
    j = (np.arange(508, dtype=U) + U(1)) * U(G)
    b = 1
    while b < N:
        cnt = min(B, N - b)
        raw = np.frombuffer(f.read(cnt * BS), dtype=np.uint8).reshape(cnt, BS)
        for i in range(cnt):
            blk = raw[i]
            if not blk.any():
                continue                    # st = 0: пусто
            idx = b + i
            hs = int.from_bytes(blk[8:16].tobytes(), 'little'); hi_ = int.from_bytes(blk[16:20].tobytes(), 'little'); hc = int.from_bytes(blk[20:24].tobytes(), 'little')
            ok = blk[:8].tobytes() == b'VM2BBLK1' and hi_ == idx
            if ok:
                c = zlib.crc32(blk[32:].tobytes(), zlib.crc32(blk[:20].tobytes()))
                ok = c == hc
            if ok:
                key = mix(seedU + mix(mix(U(idx)) ^ mix(U(hs) ^ U(0x5851F42D4C957F2D))))
                w = fin(key + j)
                ok = w.tobytes() == blk[32:].tobytes()
            if ok:
                st[idx] = 1; seq[idx] = hs; tns[idx] = int.from_bytes(blk[24:32].tobytes(), 'little')
            else:
                st[idx] = 2
        b += cnt
    valid = st[1:] == 1
    M = int(seq[1:][valid].max()) if valid.any() else 0
    last, infl = lastseq(seedU, N, M)
    ok_ = int(((st == 1) & (seq == last))[1:].sum())
    bad_idx = np.nonzero(~((st == 1) & (seq == last)))[0]; bad_idx = bad_idx[bad_idx >= 1]
    zero = int((st[bad_idx] == 0).sum()); torn = int((st[bad_idx] == 2).sum()); stale = int((st[bad_idx] == 1).sum())
    viol = [int(x) for x in bad_idx if not (x == infl and st[x] != 0)]
    # число «будущих» записей (seq > M) невозможно по определению M; для сравнения с эталоном считаем блоки, у которых seq больше, чем в образе
    return {'M': M, 'blocks': N - 1, 'ok': ok_, 'zero': zero, 'torn': torn, 'stale': stale, 'inflight': len(bad_idx) - len(viol), 'violations': len(viol),
            'viol_blocks': viol[:10], 'seed': '%x' % seed, 'seedU': seedU, 'N': N, 'seq': seq, 'st': st, 'tns_M': int(tns[1:][seq[1:] == U(M)].max()) if M else 0}

def main():
    a = sys.argv[1:]; win = None; ref = None; js = False; path = None
    while a:
        x = a.pop(0)
        if x == '--window': win = (int(a.pop(0)), int(a.pop(0)))
        elif x == '--ref': ref = a.pop(0)
        elif x == '--json': js = True
        else: path = x
    r = analyse(path)
    if 'error' in r: print('ERROR', r['error']); return 2
    out = {k: v for k, v in r.items() if k not in ('seq', 'st', 'seedU', 'N')}
    rc = 1 if r['violations'] else 0
    if win:
        out['window'] = list(win)
        if r['M'] < win[0]: out['verdict'] = 'BAD: M < S_lo (writes before the snapshot are missing)'; rc = 1
        elif r['M'] > win[1]: out['verdict'] = 'BAD: M > S_hi (writes AFTER the snapshot are present)'; rc = 1
        elif rc == 0: out['verdict'] = 'OK: prefix-consistent, M in window'
        else: out['verdict'] = 'BAD: violations'
    if ref:
        # негативный контроль / сравнение: образ против эталона, снятого позже (в нём больше записей): блоков, где seq отличается
        q = analyse(ref)
        diff = int(((r['seq'] != q['seq']) | (r['st'] != q['st']))[1:].sum())
        out['ref'] = {'M': q['M'], 'blocks_differ_from_ref': diff, 'writes_after': q['M'] - r['M']}
    print(json.dumps(out) if js else ' '.join('%s=%s' % kv for kv in out.items()))
    return rc

if __name__ == '__main__':
    sys.exit(main())


def diff_vs_expected(r, M2):
    """сколько блоков образа r НЕ совпадает с эталоном «состояние после записей 1..M2» (отрицательный контроль: эталон снят ПОЗЖЕ снимка, M2 > M)"""
    last, _ = lastseq(r['seedU'], r['N'], M2)
    same = (r['st'] == 1) & (r['seq'] == last)
    return int((~same)[1:].sum())
