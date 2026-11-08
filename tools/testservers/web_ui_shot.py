#!/usr/bin/env python3
"""Снимки экрана страницы «Virtual Machine Backup» (вкладка Snapshot: холодный / горячий режим) в настоящем браузере (Chromium через playwright) против заглушки службы.
   /home/symo/AI/venv/bin/python tools/testservers/web_ui_shot.py <папка для png> [языки через запятую, по умолчанию en,ru]     (playwright берётся из любой установки; в git снимки не кладём)
Сайт отдаётся из src/web как есть, языковые таблицы собираются так же, как tools/gen_webui.py; /api/* отвечает заглушка (вход выполнен, один сервер ESXi, одна ВМ)."""
import sys, os, json, threading, http.server, socketserver, urllib.parse
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')); WEB = os.path.join(ROOT, 'src', 'web')
sys.path.insert(0, os.path.join(ROOT, 'tools')); import webui_strings
OUT = os.path.abspath(sys.argv[1]); os.makedirs(OUT, exist_ok=True)
CSRF = 'c' * 64
API = {'servers.list': [{'id': 'local', 'name': 'Local Computer', 'type': 'local'}, {'id': 'e1', 'name': 'ESX lab', 'type': 'esxi', 'host': '10.213.77.10', 'port': 443}],
       'servers.vms': {'vms': [{'name': 'vm2b-guest', 'state': 'running', 'size': 300 << 20}, {'name': 'vm2b-off', 'state': 'stopped', 'size': 600 << 20}]},
       'state': {'version': '1.0', 'now': 1700000000, 'jobs': [], 'settings': {'level': 5, 'threads': 0}, 'scheduler': True, 'data_dir': 'C:\\ProgramData\\VM2Backup', 'cpus': 8},
       'tasks.list': [], 'history.list': [], 'jobs.list': [], 'backups.list': {'backups': [{'vm': 'vm2b-guest', 'created': 1699990000, 'raw_size': 300 << 20, 'size': 120 << 20, 'compression': 'xz', 'level': 7, 'encrypted': True, 'volumes': 3, 'server': 'local', 'directory': 'D:\\Резервные копии виртуальных машин\\Отдел разработки\\vm2b-guest-with-a-long-name', 'archive': 'vm2b-guest_20231114'}], 'resumable': []}}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, data, ctype):
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        p = urllib.parse.urlsplit(self.path).path
        if p == '/api/me': return self.send(200, json.dumps({'authenticated': True, 'user': 'admin', 'role': 'admin', 'csrf': CSRF}).encode(), 'application/json')
        if p.startswith('/lang/') and p.endswith('.js'):
            code = p[6:-3]; table, _ = webui_strings.read_lang(code); t = {k: v for k, v in table.items() if v}
            return self.send(200, ('window.VM2B_STRINGS=window.VM2B_STRINGS||{};window.VM2B_STRINGS[%s]=%s;\n' % (json.dumps(code), json.dumps(t, ensure_ascii=False))).encode(), 'application/javascript')
        f = os.path.join(WEB, 'index.html' if p == '/' else p.lstrip('/'))
        if os.path.isfile(f) and os.path.commonpath([WEB, os.path.abspath(f)]) == WEB:
            return self.send(200, open(f, 'rb').read(), {'.js': 'application/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.html': 'text/html; charset=utf-8'}.get(os.path.splitext(f)[1], 'application/octet-stream'))
        self.send(404, b'', 'text/plain')
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0); self.rfile.read(n)
        cmd = urllib.parse.urlsplit(self.path).path.replace('/api/', '')
        self.send(200, json.dumps({'ok': True, 'result': API.get(cmd, {})}).encode(), 'application/json')
srv = socketserver.ThreadingTCPServer(('127.0.0.1', 0), H); srv.daemon_threads = True
port = srv.server_address[1]; threading.Thread(target=srv.serve_forever, daemon=True).start()
from playwright.sync_api import sync_playwright
LANGS = (sys.argv[2] if len(sys.argv) > 2 else 'en,ru').split(',')
KEYS = {'btn': 'Backup virtual machines...', 'tab': 'Snapshot', 'hot': 'Back up while the VM is running (hot backup)'}
T = {}
for lg in LANGS:
    tab, _ = webui_strings.read_lang(lg) if lg != 'en' else ({}, None)
    T[lg] = {k: (tab.get(v) or v) for k, v in KEYS.items()}
res = []
tr_nav = {}; tr_show = {}
for lg in LANGS:
    if lg != 'en':
        tb, _ = webui_strings.read_lang(lg); tr_nav[lg] = tb.get('Backup Explorer') or 'Backup Explorer'; tr_show[lg] = tb.get('Show') or 'Show'
with sync_playwright() as pw:
    br = pw.chromium.launch()
    for lang in LANGS:
        ctx = br.new_context(viewport={'width': int(os.environ.get('VM2B_SHOT_W', '1100')), 'height': 900}); pg = ctx.new_page()
        pg.add_init_script("try{localStorage.setItem('vm2b.lang','%s')}catch(e){}" % lang)
        pg.goto('http://127.0.0.1:%d/' % port); pg.wait_for_selector('text=ESX lab', timeout=15000)
        pg.click('text=ESX lab'); pg.click('button:has-text("%s")' % T[lang]['btn'])
        pg.wait_for_selector('.dialog'); pg.click('.tabs a:has-text("%s")' % T[lang]['tab'])
        def state():
            return pg.evaluate("""() => { const d = document.querySelector('.dialog'); const L = Array.from(d.querySelectorAll('label')).map(l => ({t: l.textContent.trim(), c: l.querySelector('input') ? l.querySelector('input').checked : null, dis: l.querySelector('input') ? l.querySelector('input').disabled : null, vis: l.offsetParent !== null}));
                const note = Array.from(d.querySelectorAll('.note')).filter(n => n.offsetParent !== null).map(n => n.textContent.slice(0, 60)); return {labels: L.filter(x => x.c !== null), notes: note}; }""")
        s1 = state(); pg.screenshot(path=os.path.join(OUT, 'snapshot-tab-%s-cold-default.png' % lang)); res.append((lang, 'cold (default)', s1))
        pg.click('label:has-text("%s") input' % T[lang]['hot']); pg.wait_for_timeout(200)
        s2 = state(); pg.screenshot(path=os.path.join(OUT, 'snapshot-tab-%s-hot.png' % lang)); res.append((lang, 'hot checked', s2))
        # the Backup Explorer page with one backup: the table must fit the window (no horizontal scroll)
        pg.keyboard.press('Escape'); pg.wait_for_timeout(200)
        ex = tr_nav.get(lang, 'Backup Explorer')
        pg.click('.nav a:has-text("%s")' % ex); pg.wait_for_selector('button:has-text("%s")' % tr_show.get(lang, 'Show')); pg.click('button:has-text("%s")' % tr_show.get(lang, 'Show')); pg.wait_for_selector('table.grid tr.row', timeout=5000)
        over = pg.evaluate("() => { const c = document.querySelector('.content'); const t = document.querySelector('table.grid'); return {content: c.clientWidth, scroll: c.scrollWidth, table: t.getBoundingClientRect().width, win: window.innerWidth}; }")
        pg.screenshot(path=os.path.join(OUT, 'explorer-%s.png' % lang)); print('explorer', lang, over, 'OVERFLOW' if max(over['scroll'], over['table']) > over['content'] + 1 else 'fits')
        ctx.close()
    br.close()
for lang, name, st in res:
    print('==', lang, name)
    for l in st['labels']: print('   %-78s checked=%s disabled=%s visible=%s' % (l['t'][:78], l['c'], l['dis'], l['vis']))
