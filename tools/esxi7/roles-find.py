#!/usr/bin/env python3
"""Раунд 3, пункт D: ЭКСПЕРИМЕНТАЛЬНЫЙ поиск минимального набора привилегий.  Роль растёт по одной привилегии за прогон: прогон продукта от имени пользователя, из сообщения об ошибке
("the user lacks the privilege "X"") берётся недостающее право, оно добавляется к роли (и роль заново назначается на нужную сущность), повтор до успеха или до ошибки другого вида.
   roles-find.py <роль> <пользователь> <тег> '<json esxi_fs>' <начальные привилегии через запятую> [сущность назначения: корень|dc|ds:..|vm:..]  [TRY_TIMEOUT=секунд]
Печатает хронологию: прогон, ошибка, добавленное право.  Итоговый набор - в последней строке."""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
role, user, tag, js, init = sys.argv[1:6]; ent = sys.argv[6] if len(sys.argv) > 6 else 'корень'
privs = [p for p in init.split(',') if p]
def sh(*a, **kw): return subprocess.run(list(a), capture_output=True, text=True, **kw)
for it in range(40):
    print(sh(os.path.join(HERE, 'roles.py'), 'role', role, ','.join(privs)).stdout.strip()); print(sh(os.path.join(HERE, 'roles.py'), 'grant', user, role, ent).stdout.strip())
    out = sh(os.path.join(HERE, 'roles-try.sh'), user, '%s-%d' % (tag, it), js).stdout
    ok = 'PASS' in out and 'НЕ УДАЛАСЬ' not in out and 'ОТКАЗ' not in out and 'lacks the privilege' not in out
    err = ' | '.join(l.strip() for l in out.splitlines() if 'ошибка' in l or 'ОТКАЗ' in l or 'НЕ УДАЛАСЬ' in l or 'lacks the privilege' in l)[:900]
    print('прогон %d: %s %s' % (it, 'УСПЕХ' if ok else 'отказ', err))
    if ok: break
    m = re.findall(r'lacks the privilege "([^"]+)"', out)
    if not m: print('ошибка не про привилегию: стоп'); print(out[-1200:]); break
    if m[0] in privs: print('право', m[0], 'уже в роли: стоп'); break
    privs.append(m[0]); print('  + добавлено', m[0])
print('ИТОГ роль %s: %s' % (role, ','.join(privs)))
