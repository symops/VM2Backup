#!/usr/bin/env python3
# Разбор прогона «гость пишет во время копии» (tools/esxi7/guest-run.sh): сопоставляет хронологию ХОСТА (esxi-watch.py: появление дельты снимка, клона, serial гостя),
# журнал гостя (serial) и восстановленный образ диска (check_disk.py — независимая от писателя проверка).
#   analyse.py <каталог результатов> <тег> <образ flat> [--watch watch.log] [--serial serial.log] [--autotest autotest-guest.txt] [--json out.json]
# Критерии (все с числами в выводе):
#  1. образ = ровно состояние после записей 1..M (violations = 0, допустимо только «окно» единственной незавершённой записи M+1 в её же блоке);
#  2. M совпадает с моментом снимка: гость в момент снимка стоял (stun: пауза между записями, STALL gap_ms) — M = номер последней записи ДО паузы (допуск -2..0, в полёте одна запись);
#     вторичная проверка по часам: M внутри окна [seq(T_снимка-Δ), seq(T_снимка+Δ)], T_снимка = появление *-sesparse.vmdk в папке ВМ по часам хоста;
#  3. записи ПОСЛЕ снимка есть у гостя (E - M, рост дельты), но в копии их нет.
import sys, re, json, os, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_disk as cd

def parse_serial(path):
    R = []; ST = []
    for l in open(path, errors='replace'):
        m = re.match(r'R seq=(\d+) real=(\d+)', l)
        if m: R.append((int(m.group(1)), int(m.group(2)) / 1e9)); continue
        m = re.match(r'STALL seq=(\d+) lat_ms=(\d+) gap_ms=(\d+) real=(\d+)', l)
        if m: ST.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)) / 1e9))
    return R, ST

def parse_watch(path):
    files = []; obs = []
    for l in open(path, errors='replace'):
        if l.startswith('FILES '):
            m = re.match(r'FILES t=([\d.]+) (.*)', l)
            d = {}
            for kv in m.group(2).split():
                k, _, v = kv.rpartition('=')
                d[k] = int(v)
            files.append((float(m.group(1)), d))
        elif l.startswith('OBS '):
            m = re.match(r'OBS t=([\d.]+) ser=(\d+) (.*)', l)
            rs = re.findall(r'R seq=(\d+) real=(\d+)', m.group(3))
            if rs: obs.append((float(m.group(1)), int(rs[-1][0]), int(rs[-1][1]) / 1e9))
    return files, obs

def is_delta(k): return '/' not in k and ('sesparse' in k or '-delta' in k)

def seq_at(R, t):
    """интерполяция номера записи по времени гостя"""
    ts = [x[1] for x in R]; i = bisect.bisect_left(ts, t)
    if i <= 0: return R[0][0]
    if i >= len(R): return R[-1][0]
    (s0, t0), (s1, t1) = R[i - 1], R[i]
    return int(s0 + (s1 - s0) * (t - t0) / (t1 - t0)) if t1 > t0 else s1

def main():
    a = sys.argv[1:]; opts = {}
    pos = []
    while a:
        x = a.pop(0)
        if x.startswith('--'): opts[x[2:]] = a.pop(0)
        else: pos.append(x)
    resdir, tag, image = pos[:3]
    watch = opts.get('watch'); serial = opts.get('serial'); at = opts.get('autotest')
    R, ST = parse_serial(serial); files, obs = parse_watch(watch)
    # границы запуска из вывода теста
    t0 = t1 = None
    if at:
        for l in open(at, errors='replace'):
            m = re.search(r'GUESTRUN %s: начало копии unix=([\d.]+)' % re.escape(tag), l)
            if m: t0 = float(m.group(1))
            m = re.search(r'GUESTRUN %s: (?:копия готова|продолжение готово).*?unix=(\d+\.\d+)(?:\.\.(\d+\.\d+))?' % re.escape(tag), l)
            if m: t1 = float(m.group(2) or m.group(1))
    # хронология хоста: имя снимка берётся из мета-файла копии (VM2Backup-<10 hex>) = имя папки клонов; момент снимка = последнее появление дельты (*-sesparse.vmdk)
    # ПЕРЕД появлением этой папки; готовность клона = файл ready; снимок снят = ни дельты, ни папок VM2Backup-* в папке ВМ
    snap = None
    mp = os.path.join(os.path.dirname(os.path.dirname(image)), 'vm2backup-meta.json')
    if os.path.exists(mp): snap = json.load(open(mp)).get('snapshot')
    t_clone = None
    for t, d in files:
        if snap and any(k.startswith(snap + '/') for k in d): t_clone = t; break
    rise = ready = fall = None; dsz0 = dszmax = 0; prevhas = False; cand_rise = None
    for t, d in files:
        has = [k for k in d if is_delta(k)]
        if t_clone is not None and t > t_clone: break
        if has and not prevhas: cand_rise = (t, max(d[k] for k in has))
        prevhas = bool(has)
    if cand_rise: rise, dsz0 = cand_rise
    for t, d in files:
        if rise is None or t < rise: continue
        has = [k for k in d if is_delta(k)]
        if has: dszmax = max(dszmax, max(d[k] for k in has))
        if ready is None and snap and (snap + '/ready') in d: ready = t
        if fall is None and not has and not any(k.startswith('VM2Backup-') for k in d) and t > rise + 1: fall = t; break
    r = cd.analyse(image)
    out = {'tag': tag}
    out.update({k: v for k, v in r.items() if k in ('M', 'blocks', 'ok', 'zero', 'torn', 'stale', 'inflight', 'violations', 'viol_blocks', 'seed')})
    # скелет хронологии
    out['t_snapshot_host'] = rise; out['t_clone_ready_host'] = ready; out['t_snapshot_removed_host'] = fall
    out['delta_bytes_at_snapshot'] = dsz0; out['delta_bytes_max'] = dszmax
    # смещение часов гость/хост: минимум (t_obs - real) за минуту до снимка (включает задержку serial >= 0)
    offs = [to - tg for to, s, tg in obs if rise and rise - 90 <= to <= rise]
    off_min = min(offs) if offs else 0.0
    out['clock_offset_min_s'] = round(off_min, 2); out['clock_offset_median_s'] = round(sorted(offs)[len(offs) // 2], 2) if offs else None
    tg = (rise - off_min) if rise else None            # момент снимка в часах гостя (оценка)
    ok = r['violations'] == 0
    out['prefix_consistent'] = ok
    # stun: самая длинная пауза гостя около снимка
    if tg:
        cand = [s for s in ST if -1.0 < s[3] - tg < 2.5]
        if cand:
            stun = max(cand, key=lambda s: s[1] + s[2])
            out['stun'] = {'seq': stun[0], 'lat_ms': stun[1], 'gap_ms': stun[2], 'guest_time_vs_snapshot_s': round(stun[3] - tg, 2)}
            out['M_vs_stun'] = r['M'] - (stun[0] - 1)
            out['stun_match'] = (stun[0] - 3) <= r['M'] <= stun[0]
        for d in (2.0, 4.0):
            lo = seq_at(R, tg - d); hi = seq_at(R, tg + d)
            out['clock_window_%gs' % d] = [lo, hi, lo <= r['M'] <= hi]
        # время записи M по самому диску (метка в блоке) против момента снимка
        out['M_write_time_vs_snapshot_s'] = round(r['tns_M'] / 1e9 - tg, 2) if r['tns_M'] else None
    # записи гостя ПОСЛЕ снимка
    if fall:
        E = seq_at(R, fall - off_min - 1.5)
        out['guest_seq_at_copy_end'] = E; out['writes_after_snapshot'] = E - r['M']
        out['blocks_differ_from_state_at_copy_end'] = cd.diff_vs_expected(r, E)         # отрицательный контроль: эталон позже снимка
    if opts.get('idle'):
        # гость в момент снимка не писал (ВМ была выключена и загружалась): M = последняя запись до выключения (>= последнего R в serial до выключения, не больше чем на окно между строками R)
        lastR = R[-1][0] if R else 0
        out['idle_snapshot'] = True; out['last_R_seq_before_poweroff'] = lastR
        out['idle_match'] = lastR <= r['M'] <= lastR + 100
    out['copy_seconds'] = round((t1 - t0), 1) if t0 and t1 else None
    if opts.get('idle'): out['verdict'] = 'OK' if ok and out.get('idle_match') else 'BAD'
    else:
        out['verdict'] = 'OK' if ok and out.get('stun_match', True) and out.get('clock_window_4s', [0, 0, True])[2] else 'BAD'
        # Часы гостя уходят от часов хоста на секунды, если перед снимком гость долго стоял (удаление СТАРОГО снимка = консолидация: «запись 4,2 с, пауза 12,9 с»; так бывает при перезапуске копии):
        # окно по часам и выбор stun по ним тогда смещены. Запасная проверка без часов: самая длинная пауза гостя в окне ±12 с вокруг оценки момента снимка, и M ровно перед ней.
        if out['verdict'] == 'BAD' and ok and tg:
            wide = [x for x in ST if -12.0 < x[3] - tg < 12.0 and x[1] + x[2] >= 1000]
            if wide:
                w = max(wide, key=lambda x: x[1] + x[2])
                if (w[0] - 3) <= r['M'] <= w[0]:
                    out['stun_wide'] = {'seq': w[0], 'lat_ms': w[1], 'gap_ms': w[2], 'guest_time_vs_snapshot_s': round(w[3] - tg, 2)}
                    out['M_vs_stun_wide'] = r['M'] - (w[0] - 1); out['clock_suspect'] = True; out['verdict'] = 'OK'
    print(json.dumps(out, ensure_ascii=False))
    if opts.get('json'): json.dump(out, open(opts['json'], 'w'), ensure_ascii=False, indent=1)
    return 0 if out['verdict'] == 'OK' else 1

if __name__ == '__main__':
    sys.exit(main())
