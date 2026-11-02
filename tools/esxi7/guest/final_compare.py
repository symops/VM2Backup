#!/usr/bin/env python3
# final_compare.py <настоящий диск ВМ после всех записей> <образ копии>...: сравнение копий с живым диском, снятым ПОЗЖЕ (отрицательный контроль на настоящих данных, не на генераторе).
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_disk as cd
live = cd.analyse(sys.argv[1])
print('FINAL live disk: M=%d violations=%d torn=%d zero=%d stale=%d inflight=%d (согласован: %s)' % (live['M'], live['violations'], live['torn'], live['zero'], live['stale'], live['inflight'], live['violations'] == 0))
bad = live['violations'] != 0
for p in sys.argv[2:]:
    r = cd.analyse(p)
    diff = int(((r['seq'] != live['seq']) | (r['st'] != live['st']))[1:].sum())
    print('FINAL %-40s M=%d (живой диск позже на %d записей) копия согласована=%s, блоков отличается от живого диска: %d -> %s' % (p.split('/guest-out/')[-1].split('/')[0], r['M'], live['M'] - r['M'], r['violations'] == 0, diff, 'различие найдено' if diff > 0 else 'НЕ ОТЛИЧИЛ'))
    bad |= diff == 0 or r['violations'] != 0
sys.exit(1 if bad else 0)
