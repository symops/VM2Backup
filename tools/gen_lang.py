#!/usr/bin/env python3
"""Interface strings in 17 languages.

In the sources every string of the interface is written as TR("English text") (on one line, no concatenation). This script
 1. collects those strings from src/*.cpp / src/*.h,
 2. reads src/lang/<code>.txt  - one line per string: "English text<TAB>translation"  (\\n = a line break, \\t = a tab, \\\\ = a backslash;
    lines starting with # are comments; a missing or empty translation means "show the English text"),
 3. checks that a translation keeps the printf placeholders (%s %d %lu ...) of the English text in the same order,
 4. writes src/lang_table.inc (sorted English texts + a translation per language) that src/lang.cpp includes.

    python3 tools/gen_lang.py            check and generate; prints what is missing per language
    python3 tools/gen_lang.py --update   also rewrite src/lang/*.txt: add the new English texts (empty translation), drop the ones no longer used
    python3 tools/gen_lang.py --check        only check the translation files (placeholders, coverage); writes nothing
    python3 tools/gen_lang.py --missing ru   print the English texts that have no translation yet in the language `ru`
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src')
LANGS = [   # (file code, native name)
    ('en', 'English'), ('ru', 'Русский'), ('uk', 'Українська'), ('be', 'Беларуская'), ('de', 'Deutsch'),
    ('fr', 'Français'), ('es', 'Español'), ('it', 'Italiano'), ('pt', 'Português (Brasil)'), ('pl', 'Polski'),
    ('nl', 'Nederlands'), ('tr', 'Türkçe'), ('id', 'Bahasa Indonesia'), ('vi', 'Tiếng Việt'), ('zh', '中文(简体)'),
    ('ja', '日本語'), ('ko', '한국어'),
]
PLACE = re.compile(r'%%|%(?:[-+ 0#]*)(?:\d+|\*)?(?:\.\d+)?(?:l|ll|h|I64)?[sdiuxXcf]')
TR = re.compile(r'\bTRK?\("((?:[^"\\]|\\.)*)"\)')


def unescape_c(s):
    """The English text as it is in the C++ literal -> the real string."""
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append({'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}.get(n, '\\' + n))
            i += 2
        else:
            out.append(c); i += 1
    return ''.join(out)


def esc_file(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')


def unesc_file(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]; out.append({'n': '\n', 't': '\t', '\\': '\\'}.get(n, n)); i += 2
        else:
            out.append(c); i += 1
    return ''.join(out)


def collect():
    found = {}
    for name in sorted(os.listdir(ROOT)):
        if name.endswith(('.cpp', '.h')):
            text = open(os.path.join(ROOT, name), encoding='utf-8').read()
            for m in TR.finditer(text):
                found.setdefault(unescape_c(m.group(1)), name)
    return found


def read_lang(code):
    path = os.path.join(ROOT, 'lang', code + '.txt')
    d = {}
    if not os.path.exists(path):
        return d
    for n, line in enumerate(open(path, encoding='utf-8').read().split('\n'), 1):
        if not line or line.startswith('#'):
            continue
        if '\t' not in line:
            print(f'{code}.txt:{n}: no TAB, line ignored'); continue
        k, v = line.split('\t', 1)
        d[unesc_file(k)] = unesc_file(v)
    return d


def wide(s):
    """C++ wide string literal; every non-ASCII char as \\xNNNN in its own literal piece (hex escapes are greedy)."""
    parts, cur = [], ''
    def flush():
        nonlocal cur
        if cur:
            parts.append('L"' + cur + '"'); cur = ''
    for ch in s:
        o = ord(ch)
        if ch == '\n': cur += '\\n'
        elif ch == '\t': cur += '\\t'
        elif ch == '"': cur += '\\"'
        elif ch == '\\': cur += '\\\\'
        elif 32 <= o < 127: cur += ch
        else:
            flush()
            if o > 0xFFFF:
                o -= 0x10000
                parts.append('L"\\x%04x"' % (0xD800 + (o >> 10))); parts.append('L"\\x%04x"' % (0xDC00 + (o & 0x3FF)))
            else:
                parts.append('L"\\x%04x"' % o)
    flush()
    return ' '.join(parts) if parts else 'L""'


def cstr(s):
    """The English text as a UTF-8 C string literal (compared with the TR() argument at run time)."""
    b = s.encode('utf-8'); out = ''
    for ch in b:
        c = chr(ch)
        if c == '\n': out += '\\n'
        elif c == '\t': out += '\\t'
        elif c == '"': out += '\\"'
        elif c == '\\': out += '\\\\'
        elif 32 <= ch < 127: out += c
        else: out += '\\%03o' % ch
    return '"' + out + '"'


def main():
    update = '--update' in sys.argv
    if '--missing' in sys.argv:
        code = sys.argv[sys.argv.index('--missing') + 1]
        d = read_lang(code)
        for k in sorted(collect()):
            if not d.get(k): print(k)
        return 0
    used = collect()
    keys = sorted(used, key=lambda s: s.encode('utf-8'))      # the same order strcmp gives at run time
    bad = 0
    langs = {}
    for code, _ in LANGS:
        d = read_lang(code)
        langs[code] = d
        if code == 'en':
            continue
        miss = [k for k in keys if not d.get(k)]
        extra = [k for k in d if k not in used]
        for k, v in d.items():
            if k in used and v and [x for x in PLACE.findall(k) if x != '%%'] != [x for x in PLACE.findall(v) if x != '%%']:
                print(f'{code}: placeholders differ: {k!r} -> {v!r}'); bad += 1
        print(f'{code}: {len(keys) - len(miss)}/{len(keys)} translated' + (f', {len(extra)} unused' if extra else ''))
        if update:
            lines = ['# English text <TAB> translation (\\n line break). Empty = show English. Run tools/gen_lang.py --update after changing the sources.']
            for k in keys:
                lines.append(esc_file(k) + '\t' + esc_file(d.get(k, '')))
            os.makedirs(os.path.join(ROOT, 'lang'), exist_ok=True)
            open(os.path.join(ROOT, 'lang', code + '.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    out = ['// generated by tools/gen_lang.py from src/lang/*.txt and the TR("...") strings of the sources - do not edit',
           'static const int kLangCount = %d;' % len(LANGS), 'static const wchar_t* const kLangNames[kLangCount] = {']
    out += ['    %s,' % wide(name) for _, name in LANGS]
    out.append('};')
    out.append('struct LangEntry { const char* en; const wchar_t* tr[kLangCount]; };')
    out.append('static const int kStringCount = %d;' % len(keys))
    out.append('static const LangEntry kStrings[] = {')
    for k in keys:
        tr = ['nullptr' if code == 'en' or not langs[code].get(k) else wide(langs[code][k]) for code, _ in LANGS]
        out.append('    { %s, { %s } },' % (cstr(k), ', '.join(tr)))
    out.append('};')
    if '--check' not in sys.argv: open(os.path.join(ROOT, 'lang_table.inc'), 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print(f'{len(keys)} strings' + ('' if '--check' in sys.argv else ', lang_table.inc written') + (f', {bad} placeholder problem(s)' if bad else ''))
    return 1 if bad else 0


sys.exit(main())
