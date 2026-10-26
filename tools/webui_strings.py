#!/usr/bin/env python3
"""The translations of the web UI.  English text is the key (like TR("English") in the program); src/web/lang/<code>.txt holds one line per string:
   "English text<TAB>translation"   (\\n = line break, \\t = tab, \\\\ = backslash; lines starting with # are comments; an empty translation = show English).
   python3 tools/webui_strings.py                 print the English strings found in src/web/app.js
   python3 tools/webui_strings.py --update        add the new strings to every src/web/lang/*.txt (empty translation) and drop the dead ones
   python3 tools/webui_strings.py --check         placeholders {0} {1}.. kept, no dead / missing keys, no duplicates; prints the translated count per language; exit 1 on a problem
   python3 tools/webui_strings.py --missing [ru]  print the strings without a translation in that language (default ru); exit 1 when there are any
tools/gen_webui.py embeds the languages (src/web/lang/<code>.js is served on demand)."""
import os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGDIR = os.path.join(root, 'src', 'web', 'lang')
LANGS = [('en', 'English'), ('ru', 'Русский'), ('uk', 'Українська'), ('be', 'Беларуская'), ('de', 'Deutsch'), ('fr', 'Français'), ('es', 'Español'), ('it', 'Italiano'), ('pt', 'Português (Brasil)'),
         ('pl', 'Polski'), ('nl', 'Nederlands'), ('tr', 'Türkçe'), ('id', 'Bahasa Indonesia'), ('vi', 'Tiếng Việt'), ('zh', '中文(简体)'), ('ja', '日本語'), ('ko', '한국어')]
HEADER = '# English text <TAB> translation (\\n line break). Empty = show English. Run tools/webui_strings.py --update after changing src/web/app.js.\n'
PH = re.compile(r'\{\d\}')


def collect():
    app = open(os.path.join(root, 'src', 'web', 'app.js'), encoding='utf-8').read()
    STR = r"'((?:[^'\\\n]|\\.)*)'"
    pats = [r"\bT\(" + STR, r"\bbtn\(" + STR, r"\bfield\(" + STR, r"\bfieldBrowse\(" + STR, r"\bchk\(" + STR, r"title: " + STR, r"label: " + STR, r"\bname: " + STR]
    found = set()
    for p in pats:
        for m in re.finditer(p, app): found.add(m.group(1))
    for m in re.finditer(r"field\(" + STR + r",[^;]*?,\s*" + STR + r"\)", app): found.add(m.group(2))
    for m in re.finditer(r"fieldBrowse\(" + STR + r",[^;]*?,\s*" + STR + r"[,)]", app): found.add(m.group(2))
    for m in re.finditer(r"\['(?:\w+)', '((?:[^'\\]|\\.)*)'\]", app): found.add(m.group(1))
    for m in re.finditer(r"DAYS = \[([^\]]*)\]", app):
        for d in re.findall(r"'(\w+)'", m.group(1)): found.add(d)
    plain = ['Version', 'Data folder', 'Scheduler', 'on', 'off', 'stored', 'last', 'Switch the scheduler off', 'Switch the scheduler on', 'optional', 'required', 'Start', 'Success', 'Warning', 'Failed', 'Abort', 'Disabled', 'Waiting', 'Running', 'Paused',
             'backup', 'task', 'verify', 'restore', 'restore_esxi', 'manual', 'scheduled', 'admin', 'operator', 'viewer', 'minutes', 'hours', 'days', 'encrypted', 'fixed', 'network', 'removable', 'cdrom', 'other', 'server', 'share', 'folder', 'Choose a file', 'Choose a folder', 'Add user', 'Edit user', 'Edit Server', 'Add Server', 'listening', 'down', 'read', 'transferred']
    for s in plain: found.add(s)
    keep = set(plain); found.discard('English')
    out = sorted(x.replace("\\'", "'") for x in found if x and (not re.match(r'^[a-z_.]+$', x) or x in keep))
    return out


def unesc(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]; out.append({'n': '\n', 't': '\t', '\\': '\\'}.get(n, '\\' + n)); i += 2
        else: out.append(c); i += 1
    return ''.join(out)


def esc(s): return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def read_lang(code):
    path = os.path.join(LANGDIR, code + '.txt'); table = {}; dup = []
    if not os.path.exists(path): return table, dup
    for line in open(path, encoding='utf-8').read().split('\n'):
        if not line or line.startswith('#'): continue
        k, _, v = line.partition('\t')
        k = unesc(k)
        if k in table: dup.append(k)
        table[k] = unesc(v)
    return table, dup


def write_lang(code, keys, table):
    os.makedirs(LANGDIR, exist_ok=True)
    with open(os.path.join(LANGDIR, code + '.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(HEADER)
        for k in keys: f.write(esc(k) + '\t' + esc(table.get(k, '')) + '\n')


def codes(): return [c for c, _ in LANGS if c != 'en']


def main():
    a = sys.argv[1:]
    keys = collect()
    if not a:
        print('\n'.join(keys)); return 0
    if a[0] == '--update':
        for c in codes():
            table, _ = read_lang(c); write_lang(c, keys, table)
        print('updated %d languages, %d strings' % (len(codes()), len(keys))); return 0
    if a[0] == '--check':
        bad = 0
        for c in codes():
            table, dup = read_lang(c); path = c + '.txt'
            if not os.path.exists(os.path.join(LANGDIR, path)): print(path, 'missing'); bad += 1; continue
            for k in dup: print(path, 'duplicate key:', k); bad += 1
            dead = [k for k in table if k not in set(keys)]; miss = [k for k in keys if k not in table]
            for k in dead: print(path, 'dead key (run --update):', k[:70]); bad += 1
            for k in miss: print(path, 'key not in the file (run --update):', k[:70]); bad += 1
            for k, v in table.items():
                if v and sorted(PH.findall(k)) != sorted(PH.findall(v)): print(path, 'placeholders differ:', k[:60], '->', v[:60]); bad += 1
            done = sum(1 for k in keys if table.get(k)); print('%s: %d of %d translated' % (c, done, len(keys)))
        print('check:', 'OK' if not bad else '%d problem(s)' % bad)
        return 1 if bad else 0
    if a[0] == '--missing':
        c = a[1] if len(a) > 1 else 'ru'
        table, _ = read_lang(c); miss = [k for k in keys if not table.get(k)]
        for k in miss: print(k)
        return 1 if miss else 0
    print(__doc__); return 2


if __name__ == '__main__': sys.exit(main())
