#!/usr/bin/env python3
"""One complete compression benchmark: VM2Backup xz (the one compressor of the product) against GNU gzip, pigz, lz4, zstd, xz, 7-Zip (LZMA2), bzip2.

    python3 tools/bench_full.py <sample dir> <out dir> [--jobs=4] [--files=a.bin,b.bin] [--only=substr,substr]

Same method for every row: ONE thread, CPU time (user+sys of the child, from wait4) so that a busy machine does not distort the
speeds; MB/s = megabytes of UNCOMPRESSED data per CPU second; ratio = original / compressed; "dec MB/s" = the same for decompression
(the output goes to /dev/null).  Multi-threaded rows (marked "T<n>") are WALL clock at the end, one at a time.
Writes <out>/results.csv and <out>/results.md.  The VM2Backup rows use the library code itself (src/xz.cpp)
built here with -O2 (no sanitizers).
"""
import csv, os, resource, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..'))
NCPU = os.cpu_count() or 1


def cpu_run(cmd):
    """Runs a shell command; returns (cpu seconds of the children, wall seconds)."""
    t0 = time.time()
    p = subprocess.Popen(cmd, shell=True, stderr=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    wall = time.time() - t0
    if os.waitstatus_to_exitcode(status) != 0:
        raise RuntimeError('failed: %s' % cmd)
    return ru.ru_utime + ru.ru_stime, wall


def build(work):
    """Native helpers: one program for the VM2B xz (compress / decompress, in-process timing = CPU time of the thread)."""
    src = os.path.join(work, 'vm2b_bench.cpp'); exe = os.path.join(work, 'vm2b_bench')
    open(src, 'w').write(r'''
#include "%(r)s/src/xz.h"
#include <time.h>
#include <stdio.h>
using namespace vm2b;
static double cpu(){ timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t); return t.tv_sec+t.tv_nsec*1e-9; }
// usage: vm2b_bench c xz <level> <threads> <in> <out>   |   vm2b_bench d xz <in> <out>
int main(int argc,char**argv){
    Str k=argv[1], codec=argv[2]; Str e;
    if(k=="c"){
        Bytes d; if(!ReadWholeFile(argv[5],d,e,(size_t)1<<31)){puts(e.c_str());return 1;}
        int lv=atoi(argv[3]), th=atoi(argv[4]); MemSink ms; double t0=cpu(), w0=0; (void)w0;
        { Sink* x=MakeXzSink(&ms,lv,th); x->write(d.data(),d.size()); x->finish(); delete x; }
        double t=cpu()-t0; WriteWholeFile(argv[6],ms.data.data(),ms.data.size(),e); printf("%%.4f %%zu\n",t,ms.data.size()); return 0;
    }
    Bytes c; if(!ReadWholeFile(argv[3],c,e,(size_t)1<<31)){puts(e.c_str());return 1;}
    MemSource src(c.data(), c.size()); double t0=cpu(); size_t n=0; static u8 buf[1<<20];
    Source* in = MakeXzSource(&src);
    for(;;){ i64 r=in->read(buf,sizeof buf); if(r<0){puts(in->err.c_str());return 1;} if(r==0)break; n+=(size_t)r; }
    printf("%%.4f %%zu\n", cpu()-t0, n); return 0;
}''' % {'r': ROOT})
    cmd = ['g++', '-std=c++17', '-O2', '-w', '-fno-exceptions', '-fno-rtti', '-pthread', src,
           os.path.join(ROOT, 'src/xz.cpp'), os.path.join(ROOT, 'src/base.cpp'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('cannot build the VM2B helper (MemSource name may differ):\n' + r.stderr[:2000])
    return exe


def rows(vm2b):
    z = lambda c, ext: ('%s -c {i} > {o}' % c, '%s > /dev/null' % ext)
    R = []                                   # (name, compress cmd, decompress cmd, threads)
    for lv in (1, 3, 5, 7, 9): R.append(('VM2B xz %d%s' % (lv, ' (default)' if lv == 5 else ''), '%s c xz %d 1 {i} {o}' % (vm2b, lv), '%s d xz {o}' % vm2b, 1, 'vm2b'))
    for lv in (1, 6, 9): R.append(('gzip -%d' % lv, 'gzip -%d -c {i} > {o}' % lv, 'gzip -dc {o} > /dev/null', 1, ''))
    R.append(('pigz -6 -p1', 'pigz -6 -p1 -c {i} > {o}', 'pigz -dc {o} > /dev/null', 1, ''))
    R.append(('lz4 -1', 'lz4 -1 -c {i} > {o}', 'lz4 -dc {o} > /dev/null', 1, ''))
    R.append(('lz4 -9', 'lz4 -9 -c {i} > {o}', 'lz4 -dc {o} > /dev/null', 1, ''))
    for lv in (1, 3, 9, 19): R.append(('zstd -%d' % lv, 'zstd -%d -T1 -q -c {i} > {o}' % lv, 'zstd -dc -q {o} > /dev/null', 1, ''))
    R.append(('zstd -22 --ultra', 'zstd -22 --ultra -T1 -q -c {i} > {o}', 'zstd -dc -q {o} > /dev/null', 1, ''))
    for lv in (1, 3, 6, 9): R.append(('xz -%d' % lv, 'xz -%d -T1 -c {i} > {o}' % lv, 'xz -dc {o} > /dev/null', 1, ''))
    R.append(('xz -9e', 'xz -9e -T1 -c {i} > {o}', 'xz -dc {o} > /dev/null', 1, ''))
    for mx in (1, 3, 5, 7, 9):
        R.append(('7z lzma2 -mx%d' % mx, 'rm -f {o}; 7z a -t7z -m0=lzma2 -mx=%d -mmt=1 -bd -y {o} {i} >/dev/null' % mx, '7z x -so -bd {o} > /dev/null', 1, ''))
    R.append(('bzip2 -9', 'bzip2 -9 -c {i} > {o}', 'bzip2 -dc {o} > /dev/null', 1, ''))
    return R


def multi(vm2b):
    """Wall-clock rows with all cores (reported separately)."""
    return [
        ('VM2B xz 5 T%d' % NCPU, '%s c xz 5 %d {i} {o}' % (vm2b, NCPU), '%s d xz {o}' % vm2b),
        ('VM2B xz 5 T8', '%s c xz 5 8 {i} {o}' % vm2b, '%s d xz {o}' % vm2b),
        ('xz -6 T%d' % NCPU, 'xz -6 -T%d -c {i} > {o}' % NCPU, 'xz -dc {o} > /dev/null'),
        ('7z lzma2 -mx5 T%d' % NCPU, 'rm -f {o}; 7z a -t7z -m0=lzma2 -mx=5 -mmt=%d -bd -y {o} {i} >/dev/null' % NCPU, '7z x -so -bd {o} > /dev/null'),
        ('pigz -6 -p%d' % NCPU, 'pigz -6 -p%d -c {i} > {o}' % NCPU, 'pigz -dc {o} > /dev/null'),
        ('zstd -3 T%d' % NCPU, 'zstd -3 -T%d -q -c {i} > {o}' % NCPU, 'zstd -dc -q {o} > /dev/null'),
    ]


def measure(name, ccmd, dcmd, vm2bmode, fi, size, work, wall_mode=False):
    o = os.path.join(work, 'o_%d_%s.7z' % (os.getpid(), abs(hash((name, fi)))))
    c = ccmd.replace('{i}', "'%s'" % fi).replace('{o}', "'%s'" % o)
    try:
        if vm2bmode:                                  # the helper prints "<cpu seconds> <bytes>"
            r = subprocess.run(c, shell=True, capture_output=True, text=True)
            if r.returncode: raise RuntimeError(r.stdout + r.stderr)
            ct, csz = float(r.stdout.split()[0]), int(r.stdout.split()[1])
            if wall_mode: raise RuntimeError('n/a')
            d = dcmd.replace('{o}', "'%s'" % o); r = subprocess.run(d, shell=True, capture_output=True, text=True)
            if r.returncode or int(r.stdout.split()[1]) != size: raise RuntimeError('decompress mismatch: ' + r.stdout + r.stderr)
            dt = float(r.stdout.split()[0])
        else:
            cpu, wall = cpu_run(c); ct = wall if wall_mode else cpu; csz = os.path.getsize(o)
            d = dcmd.replace('{o}', "'%s'" % o); dcpu, dwall = cpu_run(d); dt = dwall if wall_mode else dcpu
        return dict(codec=name, mbs=size / 1e6 / ct, ratio=size / csz, dmbs=size / 1e6 / dt, csize=csz)
    finally:
        if os.path.exists(o): os.remove(o)


def main():
    pos = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    sample, out = pos[0], pos[1]; os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='vm2b_benchfull_', dir=os.environ.get('TMPDIR') or '/tmp')
    only0 = opt['only'].split(',') if 'only' in opt else None
    vm2b = build(work) if (not only0 or any('VM2B' in x for x in only0)) else '/bin/false'   # the helper is needed only for the VM2B rows
    files = sorted(f for f in os.listdir(sample) if f.endswith('.bin'))
    if 'files' in opt: files = [f for f in files if f in opt['files'].split(',')]
    only = opt['only'].split(',') if 'only' in opt else None
    jobs = int(opt.get('jobs', '4'))
    res = []
    for f in files:
        fi = os.path.join(sample, f); size = os.path.getsize(fi)
        todo = [r for r in rows(vm2b) if not only or any(s in r[0] for s in only)]
        print('==', f, size, 'bytes,', len(todo), 'codecs', flush=True)
        with ThreadPoolExecutor(jobs) as ex:
            futs = [(r, ex.submit(measure, r[0], r[1], r[2], r[4] == 'vm2b', fi, size, work)) for r in todo]
            for r, fu in futs:
                try: m = fu.result(); m['file'] = f; res.append(m); print('  %-24s %8.2f MB/s  ratio %.3f  dec %8.1f MB/s' % (m['codec'], m['mbs'], m['ratio'], m['dmbs']), flush=True)
                except Exception as e: print('  %-24s FAILED %s' % (r[0], str(e)[:200]), flush=True)
        for name, cc, dc in multi(vm2b):
            if only and not any(s in name for s in only): continue
            try:
                m = measure(name, cc, dc, name.startswith('VM2B'), fi, size, work, wall_mode=not name.startswith('VM2B'))
                if name.startswith('VM2B'):        # the helper reports CPU seconds, so time the multi-thread run by wall clock here
                    o = os.path.join(work, 'mt_out'); t0 = time.time(); subprocess.run(cc.replace('{i}', "'%s'" % fi).replace('{o}', "'%s'" % o), shell=True, capture_output=True); wall = time.time() - t0
                    m['mbs'] = size / 1e6 / wall; os.path.exists(o) and os.remove(o)
                m['file'] = f; res.append(m); print('  %-24s %8.2f MB/s (wall)  ratio %.3f  dec %8.1f MB/s' % (m['codec'], m['mbs'], m['ratio'], m['dmbs']), flush=True)
            except Exception as e: print('  %-24s FAILED %s' % (name, str(e)[:200]), flush=True)
    with open(os.path.join(out, 'results.csv'), 'w', newline='') as fh:
        w = csv.DictWriter(fh, ['file', 'codec', 'mbs', 'ratio', 'dmbs', 'csize']); w.writeheader(); w.writerows(res)
    md = []
    for f in files:
        md.append('**%s** (%d MB)\n' % (f, os.path.getsize(os.path.join(sample, f)) // 1000000))
        md.append('| compressor | compress MB/s | ratio | decompress MB/s |\n|---|---:|---:|---:|')
        for m in sorted((m for m in res if m['file'] == f), key=lambda m: m['ratio']):
            md.append('| %s | %.1f | %.2f | %.0f |' % (m['codec'], m['mbs'], m['ratio'], m['dmbs']))
        md.append('')
    open(os.path.join(out, 'results.md'), 'w').write('\n'.join(md))
    print('written', out)


main()
