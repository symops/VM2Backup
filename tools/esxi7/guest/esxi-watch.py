# Наблюдатель НА САМОМ ESXi (python 3 хоста), независимый от часов гостя: с шагом ~50 мс по ЧАСАМ ХОСТА (те же, что у vmware.log / hostd.log) пишет
#   OBS t=<unix> ser=<размер serial-файла> строки, появившиеся в serial-файле гостя («R seq=..», «STALL ..», «FILL..», «BOOT..»);
#   FILES t=<unix> <имя>=<размер> ...  у файлов папки ВМ и папок клонов VM2Backup-*, когда набор или размер изменились (дельта/sesparse снимка, d0-flat.vmdk клона).
# Момент снимка = появление *-sesparse.vmdk / *-delta.vmdk у ВМ; конец копии = исчезновение папки клонов.  Запуск: python3 esxi-watch.py <папка ВМ> <serial-файл> <журнал>
import os, sys, time, glob
vmdir, ser, out = sys.argv[1:4]
f = open(out, 'a', buffering=1)
off = 0; last_files = None; last_t = 0.0
def files():
    r = {}
    for pat in (vmdir + '/*.vmdk', vmdir + '/*.vmsn', vmdir + '/VM2Backup-*/*', vmdir + '/*.vmsd'):
        for p in glob.glob(pat):
            try: r[p[len(vmdir) + 1:]] = os.stat(p).st_size
            except OSError: pass
    return r
while True:
    t = time.time()
    try:
        sz = os.stat(ser).st_size
        if sz < off: off = 0
        if sz > off:
            with open(ser, 'rb') as s:
                s.seek(off); data = s.read(sz - off)
            cut = data.rfind(b'\n')
            if cut >= 0:
                off += cut + 1
                lines = [l.strip() for l in data[:cut].decode('latin1').split('\n') if l.strip()]
                keep = [l for l in lines if l.startswith(('R seq', 'STALL', 'FILL', 'BOOT', 'RUN', 'DISK', 'WRITER', 'VERIFY'))]
                if keep: f.write('OBS t=%.3f ser=%d %s\n' % (t, sz, ' | '.join(keep[-3:] if len(keep) > 3 else keep)))
    except OSError:
        pass
    cur = files()
    if last_files is None:
        changed_names = True; changed_sizes = True
    else:
        changed_names = set(cur) != set(last_files); changed_sizes = cur != last_files
    # появление/исчезновение имён — сразу; рост размеров — не чаще раза в секунду
    if changed_names or (changed_sizes and t - last_t >= 1.0):
        f.write('FILES t=%.3f %s\n' % (t, ' '.join('%s=%d' % kv for kv in sorted(cur.items()))))
        last_files = dict(cur); last_t = t
    time.sleep(0.05)
