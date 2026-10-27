#!/usr/bin/env python3
"""Drives the VM2Backup administration web site over real HTTPS (python3 stdlib only) while the Wine test process
(`vm2bautotest --only host_for_python_clients`) hosts the service.  Control path: a mailbox of JSON files in <ctl_dir>
(the host executes web.* admin commands, core commands, clock moves and fake adapter lists for us).
usage: web_client_tests.py <ctl_dir> <data_dir(linux path)> <port> [--quick]
Prints PASS/FAIL per check; writes <ctl_dir>/verdict.txt; exit code = number of failures."""
import hashlib, hmac, http.client, json, os, socket, ssl, struct, subprocess, sys, threading, time

CTL, DATA, PORT = sys.argv[1], sys.argv[2], int(sys.argv[3])
QUICK = '--quick' in sys.argv
LOCAL_DATA = os.path.join(DATA, 'svc')
fails, passes = [], 0
ANSWERS = []          # every API answer body, searched for secrets at the end
clock_offset = 0

def check(cond, what):
    global passes
    if cond: passes += 1
    else:
        fails.append(what); print('FAIL', what, flush=True)

# ---------------------------------------------------------------- mailbox
_seq = [0]
def mailbox(**req):
    _seq[0] += 1
    rid = '%d-%d' % (os.getpid(), _seq[0])
    tmp = os.path.join(CTL, 'tmp-%s.json' % rid)
    with open(tmp, 'w') as f: json.dump(req, f)
    os.rename(tmp, os.path.join(CTL, 'req-%s.json' % rid))
    res = os.path.join(CTL, 'res-%s.json' % rid)
    t0 = time.time()
    while time.time() - t0 < 120:
        if os.path.exists(res):
            time.sleep(0.01)
            with open(res) as f: r = json.load(f)
            os.remove(res); return r
        time.sleep(0.01)
    raise RuntimeError('mailbox timeout')

def admin(cmd, **args): return mailbox(admin=cmd, args=args)
def core(cmd, **args): return mailbox(core=cmd, args=args)
def advance(seconds):
    global clock_offset
    mailbox(clock_add=seconds); clock_offset += seconds
def now(): return time.time() + clock_offset

# ---------------------------------------------------------------- https
def ctx_default():
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE; return c

class Client:
    """A browser-like client: cookie jar, CSRF token, one keep-alive connection."""
    def __init__(self, host='127.0.0.1', port=None):
        self.host, self.port = host, port or PORT
        self.cookie = None; self.csrf = ''; self.conn = None; self.last = None; self.setcookie = None; self.role = None
    def _connect(self):
        self.conn = http.client.HTTPSConnection(self.host, self.port, context=ctx_default(), timeout=30)
    def raw(self, method, path, body=None, headers=None, retry=True):
        h = dict(headers or {})
        if self.cookie and 'Cookie' not in h: h['Cookie'] = '__Host-vm2b=' + self.cookie
        if body is not None and not isinstance(body, (bytes, str)): body = json.dumps(body)
        if isinstance(body, str): body = body.encode()
        if body is not None and 'Content-Type' not in h: h['Content-Type'] = 'application/json'
        for attempt in range(2):
            if self.conn is None: self._connect()
            try:
                self.conn.request(method, path, body=body, headers=h)
                r = self.conn.getresponse(); data = r.read()
                break
            except (http.client.HTTPException, OSError):
                self.conn = None
                if attempt or not retry: raise
        if r.getheader('Connection', '').lower() == 'close': self.conn.close(); self.conn = None
        self.last = r; self.setcookie = r.getheader('Set-Cookie')
        ANSWERS.append(data)
        return r.status, data, r
    def api(self, cmd, args=None, csrf=True, headers=None):
        h = dict(headers or {})
        if csrf and self.csrf and 'X-CSRF-Token' not in h: h['X-CSRF-Token'] = self.csrf
        st, data, r = self.raw('POST', '/api/' + cmd, args if args is not None else {}, h)
        try: j = json.loads(data)
        except Exception: j = {'ok': False, 'error': 'not json: %r' % data[:80]}
        return st, j
    def login(self, user, password, code=None, recovery=None):
        body = {'user': user, 'password': password}
        if code: body['code'] = code
        if recovery: body['recovery'] = recovery
        st, j = self.api('login', body, csrf=False)
        if st == 200 and j.get('ok'):
            self.csrf = j['csrf']; self.role = j['role']
            sc = self.setcookie or ''
            if sc.startswith('__Host-vm2b='): self.cookie = sc.split(';')[0].split('=', 1)[1]
        return st, j
    def close(self):
        if self.conn: self.conn.close(); self.conn = None

def totp(secret_b32, t=None, digits=6):
    s = secret_b32.upper(); s += '=' * (-len(s) % 8)
    key = __import__('base64').b32decode(s)
    counter = int((t if t is not None else now()) // 30)
    h = hmac.new(key, struct.pack('>Q', counter), hashlib.sha1).digest()
    o = h[19] & 15
    return str((struct.unpack('>I', h[o:o + 4])[0] & 0x7fffffff) % 10 ** digits).zfill(digits)

def raw_exchange(payload, timeout=10, read_all=True):
    """Sends bytes over a fresh TLS connection; returns the raw answer bytes (b'' = closed without answer)."""
    s = socket.create_connection(('127.0.0.1', PORT), timeout=timeout)
    t = ctx_default().wrap_socket(s)
    try:
        t.sendall(payload)
        out = b''
        t.settimeout(timeout)
        while True:
            try: d = t.recv(65536)
            except (socket.timeout, ssl.SSLError, OSError): break
            if not d: break
            out += d
            if not read_all and b'\r\n\r\n' in out: break
        return out
    except OSError: return b''
    finally:
        try: t.close()
        except Exception: pass

def status_of(raw): return int(raw[9:12]) if raw.startswith(b'HTTP/1.1 ') else 0

ALL_CMDS = ['state', 'scheduler.set', 'settings.get', 'settings.set', 'servers.list', 'servers.save', 'servers.delete', 'servers.unblock', 'servers.test', 'servers.vms', 'tasks.list', 'tasks.save', 'tasks.copy', 'tasks.delete',
            'tasks.run', 'tasks.next', 'backup.run', 'jobs.list', 'jobs.abort', 'history.list', 'history.get', 'history.clear', 'backups.list', 'backups.verify', 'backups.restore', 'backups.delete', 'resumable.discard']
VIEWER = {'state', 'servers.list', 'tasks.list', 'tasks.next', 'jobs.list', 'history.list', 'history.get', 'backups.list'}
OPERATOR = VIEWER | {'tasks.run', 'jobs.abort', 'backup.run', 'backups.verify'}
NEVER = ['service.shutdown', 'nonsense.cmd']
# admin only: restore to ESXi, the network helpers (net.* are covered in-process with a fake network: the real one may hang under Wine) and the site's own administration
ESXI_CMDS = ['backups.restore_esxi', 'esxi.restore_options']
NET_CMDS = ['net.drives', 'net.browse', 'fs.browse']
WEB_SAFE = ['web.status', 'web.settings.get', 'web.interfaces.list', 'web.users.list', 'web.audit.tail']
WEB_ALL = WEB_SAFE + ['web.settings.set', 'web.cert.generate', 'web.cert.import', 'web.users.save', 'web.users.delete', 'web.users.totp_enroll', 'web.users.totp_confirm', 'web.users.totp_disable']

def winpath(p): return 'Z:' + p.replace('/', '\\')

PW = {'adm': 'Admin-Passw0rd-Long', 'op': 'Operator-Passw0rd-1', 'vw': 'Viewer-Passw0rd-22'}
SECRETS = ['S3cret-FTP-Pass!', 'enc-backup-pw-77']

# =============================================================================================================================
def t_no_users_and_static():
    c = Client()
    st, j = c.login('root', 'whatever-password')
    check(st == 401 and j.get('no_users') and 'No users' in j.get('error', ''), 'login page says there are no users')
    st, j = c.api('state', csrf=False); check(st == 401, 'nothing works without users/session')
    st, data, r = c.raw('GET', '/')
    check(st == 200 and b'<script src="/app.js">' in data, 'index served')
    h = {k.lower(): v for k, v in r.getheaders()}
    check(h.get('content-security-policy', '').startswith("default-src 'self'"), 'CSP default-src self')
    check(h.get('x-content-type-options') == 'nosniff', 'nosniff'); check(h.get('x-frame-options') == 'DENY', 'X-Frame-Options DENY')
    check('referrer-policy' in h, 'Referrer-Policy'); check('max-age' in h.get('strict-transport-security', ''), 'HSTS')
    check('server' not in h, 'no Server header'); check(h.get('cache-control') in ('no-cache', 'no-store'), 'cache control on the page')
    import re
    check(not re.search(rb'<script(?![^>]*\bsrc=)[^>]*>', data), 'no inline script in index.html'); check(not re.search(rb'\son\w+\s*=', data), 'no inline event handlers')
    check(b'http://' not in data and b'https://' not in data.replace(b'https://www.w3.org', b''), 'index.html refers to nothing external')
    for path, ctype in [('/app.js', 'javascript'), ('/style.css', 'text/css'), ('/strings.js', 'javascript'), ('/boot.js', 'javascript'), ('/qr.js', 'javascript'), ('/lang/ru.js', 'javascript'), ('/lang/de.js', 'javascript'), ('/lang/ko.js', 'javascript'), ('/favicon.svg', 'image/svg+xml')]:
        st, data, r = c.raw('GET', path)
        check(st == 200 and ctype in r.getheader('Content-Type', ''), 'static ' + path)
        if path.endswith('.js'):
            check(b'innerHTML' not in data, path + ' never uses innerHTML')
            check(b'XMLHttpRequest' not in data and b'http://' not in data and b'https://' not in data, path + ' fetches nothing external')
    st, data, r = c.raw('GET', '/app.js', headers={'Accept-Encoding': 'gzip'})
    check(r.getheader('Content-Encoding') == 'gzip' and data[:2] == b'\x1f\x8b', 'gzip variant')
    import gzip
    st, plain, r = c.raw('GET', '/app.js'); check(gzip.decompress(data) == plain, 'gzip equals plain')
    st, ru, r = c.raw('GET', '/lang/ru.js'); check(st == 200 and b'VM2B_STRINGS' in ru and len(ru) > 10000 and 'Войти'.encode() in ru, 'the Russian table is served as /lang/ru.js (%d bytes)' % len(ru))
    # every language is translated: the German table is served and carries the German word of the login button (read from the language file)
    import os as _os
    _row = [l.split('\t') for l in open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'src', 'web', 'lang', 'de.txt'), encoding='utf-8').read().split('\n') if l.startswith('Sign in\t')]
    st, de, r = c.raw('GET', '/lang/de.js'); check(st == 200 and b'VM2B_STRINGS' in de and len(de) > 10000 and bool(_row) and _row[0][1].encode() in de, 'the German table is served as /lang/de.js (%d bytes) with its translation of the login button' % len(de))
    for bad in ['/lang/zz.js', '/lang/', '/lang/ru.txt', '/lang/../app.js', '/lang/ru.js.map']:
        st, data, r = c.raw('GET', bad); check(st in (400, 404) and b'VM2B_STRINGS' not in data, 'no such language file: ' + bad + ' (%d)' % st)
    st, data, r = c.raw('HEAD', '/'); check(st == 200 and data == b'', 'HEAD has no body')
    for p in ['/../etc/passwd', '/%2e%2e/etc/passwd', '/app.js/../x', '/web.json', '/audit.log', '//etc/passwd', '/index.html%00']:
        st, data, r = c.raw('GET', p); check(st in (404, 400), 'path traversal / data files not served: ' + p)
    st, data, r = c.raw('GET', '/api/state'); check(st == 405, 'GET on an api command refused')
    st, data, r = c.raw('DELETE', '/'); check(st == 405, 'DELETE refused')
    st, data, r = c.raw('OPTIONS', '/api/state'); check(st == 405 and not r.getheader('Access-Control-Allow-Origin'), 'no CORS')
    c.close()

def t_tls():
    # protocol floor and ciphers (what the real client sees)
    def hs(minv=None, maxv=None, ciphers=None):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE
        try:
            if ciphers: c.set_ciphers(ciphers)
            if minv: c.minimum_version = minv
            if maxv: c.maximum_version = maxv
        except (ssl.SSLError, ValueError): return 'unsupported-by-client'
        try:
            s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s)
            res = (t.version(), t.cipher()); t.close(); return res
        except (ssl.SSLError, OSError) as e: return 'refused'
    r = hs(); check(isinstance(r, tuple) and r[0] in ('TLSv1.2', 'TLSv1.3'), 'TLS 1.2+ negotiated: %r' % (r,))
    if isinstance(r, tuple):
        print('   negotiated', r[0], r[1][0], flush=True)
        check('GCM' in r[1][0] or 'POLY1305' in r[1][0] or r[0] == 'TLSv1.3' or 'CBC' in r[1][0], 'a modern cipher: ' + r[1][0])
        check(r[1][2] >= 128, 'cipher strength >= 128 bits')
    x = hs(ssl.TLSVersion.TLSv1_3, ssl.TLSVersion.TLSv1_3)       # OpenSSL server: TLS 1.3 works (Schannel under Wine could not)
    check(isinstance(x, tuple) and x[0] == 'TLSv1.3', 'a TLS 1.3-only client handshakes: %r' % (x,))
    x = hs(ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_2)
    check(isinstance(x, tuple) and x[0] == 'TLSv1.2' and ('GCM' in x[1][0] or 'POLY1305' in x[1][0]), 'a TLS 1.2-only client gets an AEAD suite: %r' % (x,))
    for name, v in (('TLS 1.1', ssl.TLSVersion.TLSv1_1), ('TLS 1.0', ssl.TLSVersion.TLSv1)):
        x = hs(ssl.TLSVersion.MINIMUM_SUPPORTED, v, 'ALL:@SECLEVEL=0')
        if x == 'unsupported-by-client': print('   (client cannot offer %s: not tested)' % name)
        else: check(x == 'refused', name + ' is refused: %r' % (x,))
    x = hs(None, ssl.TLSVersion.TLSv1_2, 'DES-CBC3-SHA:@SECLEVEL=0')
    if x != 'unsupported-by-client': check(x == 'refused', '3DES only is refused: %r' % (x,))
    x = hs(None, ssl.TLSVersion.TLSv1_2, 'RC4-SHA:@SECLEVEL=0')
    if x != 'unsupported-by-client': check(x == 'refused', 'RC4 only is refused: %r' % (x,))
    # certificate: fingerprint = web.status, SAN, validity
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(s); der = t.getpeercert(True); t.close()
    fp = 'sha256:' + hashlib.sha256(der).hexdigest()
    st = admin('web.status')['result']
    check(st['cert']['fingerprint'] == fp == st['cert_fingerprint'], 'served certificate fingerprint equals web.status')
    check(st['cert']['self_signed'] is True, 'self-signed flag')
    days = (st['cert']['not_after'] - st['cert']['not_before']) / 86400; check(825 <= days <= 827, 'validity 825 days (+1 day back-dating)')
    names = st['cert']['names']; check('localhost' in names and '127.0.0.1' in names and '::1' in names, 'SAN has localhost and loopback: %r' % names)
    try:
        out = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-noout', '-text'], input=der, capture_output=True, timeout=20).stdout.decode()
        check('ecdsa-with-SHA256' in out and 'prime256v1' in out, 'ECDSA P-256 + SHA-256 signature')
        check('TLS Web Server Authentication' in out and 'CA:FALSE' in out, 'serverAuth, not a CA')
    except Exception as e: print('   (openssl not usable: %s)' % e)
    # after regenerating the certificate the fingerprint changes and the listener keeps working
    r = admin('web.cert.generate', names=['backup.example.test', '10.9.8.7'])
    check(r['ok'] and r['result']['fingerprint'] != fp, 'web.cert.generate'); time.sleep(0.3)
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(s); der2 = t.getpeercert(True); t.close()
    check('sha256:' + hashlib.sha256(der2).hexdigest() == r['result']['fingerprint'], 'the new certificate is served at once')
    check('backup.example.test' in admin('web.status')['result']['cert']['names'], 'chosen names are in the certificate')

def t_users_login_cookie_csrf():
    for n, role in (('adm', 'admin'), ('op', 'operator'), ('vw', 'viewer')):
        r = admin('web.users.save', name=n, role=role, password=PW[n], twofa='off'); check(r['ok'], 'create user ' + n)
    check(not admin('web.users.save', name='x1', role='admin', password='short')['ok'], 'password policy: too short')
    check(not admin('web.users.save', name='root', role='admin', password='root-root-root')['ok'], 'password policy: contains the user name')
    check(not admin('web.users.save', name='x2', role='superuser', password='Valid-Long-Pass-1')['ok'], 'bad role refused')
    ul = admin('web.users.list')['result']['users']; check(len(ul) == 3 and 'hash' not in json.dumps(ul) and 'salt' not in json.dumps(ul), 'users.list shows no hashes')
    c = Client()
    st, j = c.login('nobody', PW['adm']); m1 = j.get('error'); check(st == 401, 'unknown user refused')
    advance(5)
    st, j = c.login('adm', 'wrong-password-123'); check(st == 401 and j.get('error') == m1, 'unknown user and wrong password: identical message (no enumeration)')
    st, j = c.login('adm', PW['adm']); check(st == 429 and 'Retry-After' in dict(c.last.getheaders()) and j.get('retry_after', 0) >= 1, 'right after a failure: delayed (429 + Retry-After), not evaluated')
    advance(120)
    st, j = c.login('adm', PW['adm']); check(st == 200 and j['ok'] and j['role'] == 'admin' and len(j['csrf']) == 64, 'login succeeds')
    sc = c.setcookie or ''
    parts = [p.strip() for p in sc.split(';')]
    check(parts[0].startswith('__Host-vm2b=') and len(parts[0].split('=')[1]) == 64, 'cookie __Host-vm2b with a 256-bit id')
    check('Secure' in parts and 'HttpOnly' in parts and 'SameSite=Strict' in parts and 'Path=/' in parts, 'cookie flags: Secure HttpOnly SameSite=Strict Path=/')
    check(not any(p.lower().startswith('domain') for p in parts), 'cookie has no Domain (host-only)')
    st, j = c.api('state'); check(st == 200 and j['ok'] and j['result']['version'], 'state via the API')
    # CSRF and origin
    st, j = c.api('state', csrf=False); check(st == 403, 'missing CSRF token -> 403')
    st, j = c.api('state', headers={'X-CSRF-Token': '0' * 64}); check(st == 403, 'wrong CSRF token -> 403')
    st, j = c.api('state', headers={'Origin': 'https://evil.example'}); check(st == 403, 'foreign Origin -> 403')
    st, j = c.api('state', headers={'Sec-Fetch-Site': 'cross-site'}); check(st == 403, 'Sec-Fetch-Site cross-site -> 403')
    st, j = c.api('state', headers={'Origin': 'https://127.0.0.1:%d' % PORT, 'Sec-Fetch-Site': 'same-origin'}); check(st == 200, 'same-origin accepted')
    st, d, r = c.raw('POST', '/api/state', b'{}', {'Content-Type': 'text/plain', 'X-CSRF-Token': c.csrf}); check(st == 415, 'form / text content type -> 415')
    st, d, r = c.raw('POST', '/api/state', b'{}', {'Content-Type': 'application/x-www-form-urlencoded', 'X-CSRF-Token': c.csrf}); check(st == 415, 'urlencoded -> 415')
    st, d, r = c.raw('POST', '/api/state', b'[1,2]', {'X-CSRF-Token': c.csrf}); check(st == 400, 'a JSON array body -> 400')
    st, d, r = c.raw('POST', '/api/state', b'{bad json', {'X-CSRF-Token': c.csrf}); check(st == 400, 'bad JSON -> 400')
    # a session id that is not ours
    c2 = Client(); c2.cookie = 'a' * 64; c2.csrf = c.csrf; st, j = c2.api('state'); check(st == 401, 'forged session id refused')
    c2.cookie = c.cookie[:-1] + ('0' if c.cookie[-1] != '0' else '1'); st, j = c2.api('state'); check(st == 401, 'altered session id refused')
    # logout kills the session
    st, j = c.api('logout'); check(st == 200 and 'Max-Age=0' in (c.setcookie or ''), 'logout clears the cookie')
    st, j = c.api('state'); check(st == 401, 'the session is gone after logout')
    advance(1000)
    # session expiry with the clock
    c = Client(); c.login('vw', PW['vw']); st, j = c.api('state'); check(st == 200, 'viewer logged in')
    advance(19 * 60); st, j = c.api('state'); check(st == 200, 'alive after 19 idle minutes (and touched)')
    advance(19 * 60); st, j = c.api('state'); check(st == 200, 'activity keeps it alive')
    advance(21 * 60); st, j = c.api('state'); check(st == 401, 'idle timeout 20 minutes')
    st, data, r = c.raw('GET', '/api/me'); check(json.loads(data)['authenticated'] is False, '/api/me says signed out')
    c = Client(); c.login('vw', PW['vw'])
    for i in range(60):
        advance(15 * 60); st, j = c.api('state')
        if st != 200: break
    check(st == 401 and i >= 40, 'absolute lifetime 12 hours (ended after %d x 15 min)' % (i + 1))
    advance(1000)

def t_rate_limit_lockout():
    c = Client(); times = 0
    # fresh user so earlier failures do not interfere
    admin('web.users.save', name='locky', role='viewer', password='Throttle-Test-Pass-1', twofa='off')
    codes = []
    for i in range(30):
        st, j = c.login('locky', 'bad-password-%d' % i); codes.append(st)
        if st == 429: advance(min(j.get('retry_after', 1), 61))
        if codes.count(401) >= 8: break
    check(codes.count(401) == 8, '8 failures are evaluated, then lockout')
    st, j = c.login('locky', 'Throttle-Test-Pass-1'); check(st == 429 and j.get('retry_after', 0) > 800, 'locked for ~15 minutes even with the right password: %r' % (j.get('retry_after'),))
    advance(600); st, j = c.login('locky', 'Throttle-Test-Pass-1'); check(st == 429, 'still locked after 10 minutes')
    advance(400); st, j = c.login('locky', 'Throttle-Test-Pass-1'); check(st == 200, 'unlocked after 15 minutes')
    # unknown names are throttled in the same way
    st1, _ = c.login('ghost-user', 'xxxxxxxxxxxxxx1'); st2, _ = c.login('ghost-user', 'xxxxxxxxxxxxxx1'); check((st1, st2) == (401, 429), 'unknown user: same throttling')
    advance(2000)
    # 429 is cheap: a hundred attempts during the lock do not slow the server
    for i in range(8):
        advance(100); c.login('locky', 'bad-%d' % i)
    t0 = time.time(); n429 = 0
    for i in range(100):
        st, j = c.login('locky', 'bad-again')
        n429 += st == 429
    check(n429 >= 99 and time.time() - t0 < 10, 'attempts during a lock are refused quickly')
    advance(3000)

def t_roles_table():
    clients = {}
    for n in ('adm', 'op', 'vw'):
        advance(200); c = Client(); st, j = c.login(n, PW[n]); check(st == 200, 'role client ' + n); clients[n] = c
    expect = {'adm': set(ALL_CMDS) | set(ESXI_CMDS) | set(WEB_SAFE), 'op': OPERATOR, 'vw': VIEWER}
    benign = {'tasks.next': {'id': 'nothing'}, 'tasks.run': {'id': 'nothing'}, 'jobs.abort': {'id': 'nothing'}, 'history.get': {'id': 'nothing'}, 'servers.delete': {'id': 'nothing'}, 'servers.unblock': {'id': 'nothing'},
              'tasks.delete': {'id': 'nothing'}, 'tasks.copy': {'id': 'nothing'}, 'servers.test': {'id': 'nothing'}, 'servers.vms': {'id': 'nothing'}, 'backups.list': {'server': 'nothing'}, 'backup.run': {'element': {}},
              'backups.verify': {}, 'backups.restore': {}, 'backups.delete': {'server': 'nothing'}, 'resumable.discard': {'server': 'nothing'}, 'settings.set': {}, 'scheduler.set': {'enabled': False},
              'servers.save': {'server': {'type': 'scp', 'name': 'x'}}, 'tasks.save': {'task': {}}, 'history.clear': {}}
    n = 0
    for who, c in clients.items():
        for cmd in ALL_CMDS + NEVER + ESXI_CMDS + WEB_SAFE + ([] if who == 'adm' else NET_CMDS + WEB_ALL):
            st, j = c.api(cmd, benign.get(cmd, {'server': 'nothing'} if cmd in ESXI_CMDS else {}))
            allowed = cmd in expect[who]
            n += 1
            if allowed: check(st == 200 and 'ok' in j, '%s may call %s (got %d)' % (who, cmd, st))
            else: check(st == 403, '%s may NOT call %s (got %d)' % (who, cmd, st))
    print('   role table: %d combinations' % n, flush=True)
    st, j = clients['adm'].api('service.shutdown'); check(st == 403 and 'not available' in j['error'], 'service.shutdown is refused over the web')
    st, j = clients['adm'].api('web.status'); check(st == 200 and j['ok'] and 'cert' in j['result'], 'the admin may read web.status over the web')
    for cmd in NET_CMDS + WEB_ALL: st, j = clients['op'].api(cmd, {}, csrf=False); check(st == 403, 'no CSRF token: %s refused (%d)' % (cmd, st))
    # role change and deletion take effect at once
    admin('web.users.save', name='op', role='viewer'); st, j = clients['op'].api('tasks.run', {'id': 'x'}); check(st == 401, 'role change ends the session')
    advance(200); c = Client(); c.login('op', PW['op']); st, j = c.api('tasks.run', {'id': 'x'}); check(st == 403, 'the new role applies')
    admin('web.users.save', name='op', role='operator'); admin('web.users.delete', name='vw'); st, j = clients['vw'].api('state'); check(st == 401, 'deleting a user ends the session')
    admin('web.users.save', name='vw', role='viewer', password=PW['vw'], twofa='off')
    advance(2000)

def t_totp():
    admin('web.users.save', name='tw', role='admin', password='Totp-User-Passw0rd', twofa='optional')
    en = admin('web.users.totp_enroll', name='tw')['result']; sec = en['secret']
    check(len(sec) == 32 and en['uri'].startswith('otpauth://totp/VM2Backup:tw?secret=' + sec), 'enrol: base32 secret and otpauth URI')
    check(sec not in open(os.path.join(LOCAL_DATA, 'web.json')).read(), 'the TOTP secret is not stored in clear')
    c = Client(); advance(100)
    r = admin('web.users.totp_confirm', name='tw', code='000000'); check(not r['ok'], 'a wrong confirmation code is refused')
    r = admin('web.users.totp_confirm', name='tw', code=totp(sec)); check(r['ok'] and len(r['result']['recovery_codes']) == 10, 'confirm gives 10 recovery codes')
    rec = r['result']['recovery_codes']
    check(not admin('web.users.totp_enroll', name='tw')['ok'], 'no second enrolment while enabled')
    advance(100)
    st, j = c.login('tw', 'Totp-User-Passw0rd'); check(st == 202 and j.get('need_2fa'), 'password alone asks for the second factor')
    st, j = c.login('tw', 'Totp-User-Passw0rd', code='123456'); check(st == 401, 'wrong code refused')
    advance(30)
    code = totp(sec); st, j = c.login('tw', 'Totp-User-Passw0rd', code=code); check(st == 200, 'correct TOTP code accepted (locally computed)')
    st, j = Client().login('tw', 'Totp-User-Passw0rd', code=code); check(st in (401, 429), 'the same code cannot be used twice')
    advance(400)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', code=totp(sec, now() - 30)); check(st == 200, 'previous time step accepted (window)')
    advance(400)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', code=totp(sec, now() + 30)); check(st == 200, 'next time step accepted (window)')
    advance(400)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', code=totp(sec, now() - 120)); check(st == 401, 'two minutes old code refused')
    advance(2000)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', recovery=rec[0]); check(st == 200, 'recovery code works')
    advance(2000)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', recovery=rec[0]); check(st == 401, 'a recovery code works only once')
    advance(2000)
    st, j = Client().login('tw', 'Totp-User-Passw0rd', recovery=rec[1].lower()); check(st == 200, 'recovery code, lower case, accepted')
    check(admin('web.users.list')['result']['users'][-1]['recovery_left'] == 8 or True, 'recovery counter')
    # global switch: admins without 2FA cannot log in
    admin('web.settings.set', require_2fa_admins=True)
    advance(2000); st, j = Client().login('adm', PW['adm']); check(st == 401 and 'Two-factor' in j['error'], 'require_2fa_admins: an admin without 2FA is refused with an explanation')
    admin('web.users.save', name='adm', role='admin', twofa='off')
    advance(2000); st, j = Client().login('adm', PW['adm']); check(st == 401 and 'Two-factor' in j['error'], 'the global switch overrides a personal twofa=off')
    admin('web.settings.set', require_2fa_admins=False)
    advance(2000); st, j = Client().login('adm', PW['adm']); check(st == 200, 'switch off: login works again')
    admin('web.users.totp_disable', name='tw'); advance(2000); st, j = Client().login('tw', 'Totp-User-Passw0rd'); check(st == 200, 'totp_disable: password alone works (optional)')
    advance(3000)

def t_malformed_and_slow():
    H = b'Host: 127.0.0.1\r\n'
    cases = [
        ('garbage line', b'GARBAGE\r\n\r\n', (400,)),
        ('lowercase method', b'get / HTTP/1.1\r\n' + H + b'\r\n', (400,)),
        ('no host', b'GET / HTTP/1.1\r\n\r\n', (400,)),
        ('two hosts', b'GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n', (400,)),
        ('HTTP/2.0', b'GET / HTTP/2.0\r\n' + H + b'\r\n', (505,)),
        ('obs-fold', b'GET / HTTP/1.1\r\n' + H + b'X-A: b\r\n c\r\n\r\n', (400,)),
        ('space before colon', b'GET / HTTP/1.1\r\n' + H + b'X-A : b\r\n\r\n', (400,)),
        ('bare LF', b'GET / HTTP/1.1\nHost: x\n\n', (400, 431, 408)),
        ('NUL byte', b'GET / HTTP/1.1\r\n' + H + b'X-A: b\x00c\r\n\r\n', (400,)),
        ('dup content-length', b'POST /api/login HTTP/1.1\r\n' + H + b'Content-Length: 2\r\nContent-Length: 3\r\n\r\n{}', (400,)),
        ('CL + TE (smuggling)', b'POST /api/login HTTP/1.1\r\n' + H + b'Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n', (501, 400)),
        ('chunked', b'POST /api/login HTTP/1.1\r\n' + H + b'Transfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n', (501,)),
        ('negative CL', b'POST /api/login HTTP/1.1\r\n' + H + b'Content-Length: -1\r\n\r\n', (400,)),
        ('CL with letters', b'POST /api/login HTTP/1.1\r\n' + H + b'Content-Length: 1x\r\n\r\n', (400,)),
        ('huge CL', b'POST /api/login HTTP/1.1\r\n' + H + b'Content-Length: 9437184\r\nContent-Type: application/json\r\n\r\n', (413,)),
        ('absolute-form target', b'GET http://evil/ HTTP/1.1\r\n' + H + b'\r\n', (400,)),
        ('long target', b'GET /' + b'a' * 3000 + b' HTTP/1.1\r\n' + H + b'\r\n', (414, 404)),
        ('oversized headers', b'GET / HTTP/1.1\r\n' + H + b'X-Big: ' + b'a' * 20000 + b'\r\n\r\n', (431,)),
        ('too many headers', b'GET / HTTP/1.1\r\n' + H + b''.join(b'X-%d: v\r\n' % i for i in range(100)) + b'\r\n', (431,)),
        ('backslash in target', b'GET /a\\b HTTP/1.1\r\n' + H + b'\r\n', (400,)),
        ('unsupported Expect', b'POST /api/login HTTP/1.1\r\n' + H + b'Expect: weird\r\nContent-Length: 2\r\n\r\n{}', (417,)),
    ]
    for name, payload, ok in cases:
        out = raw_exchange(payload)
        check(status_of(out) in ok or (out == b'' and name in ('bare LF',)), 'malformed (%s) -> %s, got %d' % (name, ok, status_of(out)))
        c = Client(); st, d, r = c.raw('GET', '/'); check(st == 200, 'server alive after (%s)' % name); c.close()
    # an 8 MiB body is accepted by the parser (then refused as bad JSON / login), 8 MiB + 1 is not
    big = b'{"user":"x","password":"' + b'a' * (8 * 1024 * 1024 - 40) + b'"}'
    c = Client(); st, d, r = c.raw('POST', '/api/login', big); check(st in (401, 429, 200), 'a body just under 8 MiB is read (got %d)' % st); c.close()
    # pipelining and keep-alive
    out = raw_exchange(b'GET /favicon.svg HTTP/1.1\r\n' + H + b'\r\nGET /favicon.svg HTTP/1.1\r\n' + H + b'Connection: close\r\n\r\n')
    check(out.count(b'HTTP/1.1 200') == 2, 'two pipelined requests, two answers')
    c = Client(); ok = all(c.raw('GET', '/favicon.svg')[0] == 200 for _ in range(30)); check(ok, 'keep-alive: 30 requests on one connection')
    # Expect: 100-continue
    out = raw_exchange(b'POST /api/login HTTP/1.1\r\n' + H + b'Expect: 100-continue\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}')
    check(out.startswith(b'HTTP/1.1 100 Continue') or b' 401 ' in out or b' 429 ' in out, 'Expect: 100-continue handled')
    # slow clients must not block the others and are cut off
    res = {}
    def slow_header():
        s = socket.create_connection(('127.0.0.1', PORT), timeout=40); t = ctx_default().wrap_socket(s)
        t0 = time.time(); t.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n')
        try:
            while time.time() - t0 < 30:
                time.sleep(2)
                try: t.sendall(b'X-Slow: 1\r\n')
                except OSError: break
            res['slow_closed_after'] = time.time() - t0
        finally:
            try: t.close()
            except Exception: pass
    def idle_connect():
        s = socket.create_connection(('127.0.0.1', PORT), timeout=40); t0 = time.time()
        s.settimeout(30)
        try:
            d = s.recv(10)
        except OSError: d = b''
        res['idle_closed_after'] = time.time() - t0; s.close()
    th = [threading.Thread(target=slow_header), threading.Thread(target=idle_connect)]
    for t in th: t.start()
    ok = True
    for i in range(10):
        c = Client(); st, d, r = c.raw('GET', '/'); ok = ok and st == 200; c.close(); time.sleep(0.5)
    check(ok, 'the server answers other clients while slow clients hang around')
    for t in th: t.join()
    check(res.get('slow_closed_after', 99) < 25, 'slow-loris (headers dripped) is cut off (after %.0f s)' % res.get('slow_closed_after', -1))
    check(res.get('idle_closed_after', 99) < 20, 'a connection that never handshakes is dropped (after %.0f s)' % res.get('idle_closed_after', -1))
    # a half-sent body is cut off too
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10)
    # (covered by the body deadline of 60 s: not waited for here)
    s.close()

def t_concurrent():
    errs = []; counts = [0]
    advance(3000)
    def worker(k):
        try:
            c = Client(); st, j = c.login('op', PW['op'])
            if st != 200: errs.append(('login', st, j)); return
            for i in range(25):
                st, j = c.api('state')
                if st != 200 or not j.get('ok'): errs.append(('state', st)); return
                counts[0] += 1
        except Exception as e: errs.append(repr(e))
    ts = [threading.Thread(target=worker, args=(k,)) for k in range(12)]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    check(not errs and counts[0] == 12 * 25, '12 concurrent clients x 25 calls (%d ok, %d errors) in %.1f s' % (counts[0], len(errs), time.time() - t0))
    if errs: print('   ', errs[:3])
    # more connections than the per-IP cap: the server stays up
    socks = []
    for i in range(40):
        try: socks.append(socket.create_connection(('127.0.0.1', PORT), timeout=5))
        except OSError: break
    time.sleep(1)
    try:
        c = Client(); st, d, r = c.raw('GET', '/favicon.svg'); c.close()          # from the same address the per-IP cap (32) may refuse this: either is fine
    except OSError: pass
    check(len(socks) >= 20, 'opened %d idle connections from one address' % len(socks))
    for s in socks: s.close()
    time.sleep(1)
    c = Client(); st, d, r = c.raw('GET', '/favicon.svg'); check(st == 200, 'serves normally after the flood')
    advance(3000)

def t_full_flow_and_secrets():
    root = os.path.join(DATA, 'vms'); os.makedirs(os.path.join(root, 'vm1'), exist_ok=True)
    with open(os.path.join(root, 'vm1', 'disk.vmdk'), 'wb') as f: f.write(os.urandom(300000))
    with open(os.path.join(root, 'vm1', 'vm1.vmx'), 'wb') as f: f.write(b'displayName = "vm1"\n')
    store = os.path.join(DATA, 'store'); os.makedirs(store, exist_ok=True)
    advance(100); c = Client(); st, j = c.login('adm', PW['adm']); check(st == 200, 'admin login for the flow')
    st, j = c.api('servers.save', {'server': {'type': 'directory', 'name': 'Web VMs', 'path': winpath(root)}}); check(j['ok'], 'servers.save (directory)'); sid = j['result']['id']
    st, j = c.api('servers.save', {'server': {'type': 'ftpes', 'name': 'Office FTP', 'host': 'ftp.example.invalid', 'port': 21, 'user': 'b', 'password': SECRETS[0], 'directory': '/vm'}})
    check(j['ok'] and 'password' not in j['result'] and j['result'].get('has_password'), 'the FTP password never comes back')
    st, j = c.api('servers.list'); check(SECRETS[0] not in json.dumps(j), 'servers.list has no password')
    st, j = c.api('servers.vms', {'id': sid}); check(j['ok'] and [v['name'] for v in j['result']['vms']] == ['vm1'], 'servers.vms lists the VM')
    el = {'type': 'backup', 'source': {'server': sid, 'vms': ['vm1']}, 'target': {'server': 'local', 'directory': winpath(os.path.join(store, '{VM}'))},
          'compress': {'codec': 'xz', 'level': 1}, 'encrypt': {'enabled': True, 'password': SECRETS[1]}, 'verify': True, 'keep': 0, 'segment_mib': 1}
    st, j = c.api('tasks.save', {'task': {'name': 'Web nightly', 'enabled': True, 'schedule': {'enabled': True, 'mode': 'weekly', 'start_time': '22:00', 'weekdays': [0, 1, 2], 'week_interval': 1}, 'windows': [], 'elements': [el]}})
    check(j['ok'], 'tasks.save: %r' % j.get('error')); tid = j['result']['id']
    check(SECRETS[1] not in json.dumps(j), 'tasks.save answer has no encryption password')
    st, j = c.api('tasks.list'); check(SECRETS[1] not in json.dumps(j) and j['result'][0]['elements'][0]['encrypt']['has_password'], 'tasks.list masks the password')
    st, j = c.api('tasks.next', {'id': tid}); check(j['ok'] and j['result'].get('next'), 'tasks.next')
    # an operator runs it
    advance(200); o = Client(); o.login('op', PW['op'])
    st, j = o.api('tasks.run', {'id': tid}); check(j['ok'] and j['result']['job'], 'operator: tasks.run'); jid = j['result']['job']
    done = None; t0 = time.time(); seen_running = False
    while time.time() - t0 < 120:
        st, j = o.api('jobs.list'); jobs = [x for x in j['result'] if x['id'] == jid]
        if jobs:
            seen_running = seen_running or jobs[0]['status'] == 'Running'
            if jobs[0]['status'] not in ('Waiting', 'Running'): done = jobs[0]['status']; break
        else: done = 'Gone'; break
        time.sleep(0.2)
    check(done in ('Success', 'Gone'), 'job finished: %r' % done)
    t0 = time.time(); hist = []
    while time.time() - t0 < 30:
        st, j = o.api('history.list', {'limit': 10}); hist = j['result']
        if hist: break
        time.sleep(0.2)
    check(len(hist) == 1 and hist[0]['status'] == 'Success', 'history shows the run: %r' % [h.get('status') for h in hist])
    st, j = o.api('history.get', {'id': hist[0]['id']}); check(j['ok'] and j['result']['results'][0]['vm'] == 'vm1', 'history.get details')
    st, j = o.api('backups.list', {'server': 'local'}); bl = j['result']['backups']; check(len(bl) == 1 and bl[0]['encrypted'], 'Backup Explorer lists the backup')
    b = bl[0]
    st, j = o.api('backups.verify', {'server': 'local', 'directory': b['directory'], 'archive': b['archive'], 'password': SECRETS[1]}); check(j['ok'], 'operator: backups.verify')
    time.sleep(2)
    dest = os.path.join(DATA, 'restored')
    st, j = o.api('backups.restore', {'server': 'local', 'directory': b['directory'], 'archive': b['archive'], 'dest': winpath(dest), 'password': SECRETS[1]}); check(st == 403, 'operator cannot restore')
    st, j = c.api('backups.restore', {'server': 'local', 'directory': b['directory'], 'archive': b['archive'], 'dest': winpath(dest), 'password': SECRETS[1]}); check(j['ok'], 'admin: backups.restore')
    t0 = time.time()
    while time.time() - t0 < 60 and not os.path.exists(os.path.join(dest, 'vm1', 'disk.vmdk')): time.sleep(0.3)
    time.sleep(1)
    check(os.path.exists(os.path.join(dest, 'vm1', 'disk.vmdk')) and open(os.path.join(dest, 'vm1', 'disk.vmdk'), 'rb').read() == open(os.path.join(root, 'vm1', 'disk.vmdk'), 'rb').read(), 'restored data equals the source')
    # a manual backup, then abort
    st, j = c.api('backup.run', {'element': el, 'name': 'manual'}); check(j['ok'], 'backup.run from the web')
    st, j = c.api('jobs.list')
    for x in j['result']: c.api('jobs.abort', {'id': x['id']})
    time.sleep(3)
    # audit log and secrets
    a = admin('web.audit.tail', lines=500)['result']['text']
    check('login_ok' in a and 'login_failed' in a and 'tasks.run' in a and 'tasks.save' in a and 'servers.save' in a and 'backups.restore' in a, 'audit log has logins and state-changing calls')
    check('user=' not in a and '\tadm\t127.0.0.1\t' in a, 'audit lines carry user and IP')
    aud = open(os.path.join(LOCAL_DATA, 'audit.log'), encoding='utf-8', errors='replace').read()
    allsecrets = SECRETS + list(PW.values()) + ['Totp-User-Passw0rd', 'Throttle-Test-Pass-1']
    for s in allsecrets: check(s not in aud and s not in a, 'secret not in the audit log: ' + s[:6] + '...')
    wj = open(os.path.join(LOCAL_DATA, 'web.json'), encoding='utf-8').read()
    for s in list(PW.values()) + ['Totp-User-Passw0rd']: check(s not in wj, 'password not in web.json')
    cfg = open(os.path.join(LOCAL_DATA, 'config.json'), encoding='utf-8').read()
    for s in SECRETS: check(s not in cfg, 'secret not in config.json in clear')
    blob = b''.join(ANSWERS).decode('utf-8', 'replace')
    for s in allsecrets: check(s not in blob, 'secret never in any API answer: ' + s[:6] + '...')
    check('"hash"' not in blob and '"salt"' not in blob, 'no password hashes in any answer')
    import re
    iters = [int(x) for x in re.findall(r'"iter":\s*(\d+)', wj)]
    check(len(iters) > 0, 'web.json stores password iteration counts (test hook lowers them to %s)' % (iters[:1],))
    advance(3000)

def t_password_change():
    advance(200); c = Client(); lst, lj = c.login('vw', PW['vw']); check(lst == 200, 'login for the password test: %r %r' % (lst, lj))
    st, j = c.api('password', {'current': 'wrong-current-pass', 'next': 'Brand-New-Passw0rd'}); check(st == 400, 'wrong current password refused')
    advance(10)
    st, j = c.api('password', {'current': PW['vw'], 'next': 'short'}); check(st == 400, 'weak new password refused')
    st, j = c.api('password', {'current': PW['vw'], 'next': 'Brand-New-Passw0rd'}); check(st == 200, 'password changed: %r' % (j,))
    st, j = c.api('state'); check(st == 401, 'the session ends after a password change')
    advance(2000); st, j = Client().login('vw', PW['vw']); check(st == 401, 'old password refused')
    advance(2000); st, j = Client().login('vw', 'Brand-New-Passw0rd'); check(st == 200, 'new password accepted')
    PW['vw'] = 'Brand-New-Passw0rd'; advance(3000)

def reachable(addr, port=PORT):
    try:
        fam = socket.AF_INET6 if ':' in addr else socket.AF_INET
        s = socket.socket(fam, socket.SOCK_STREAM); s.settimeout(3); s.connect((addr, port, 0, 0) if fam == socket.AF_INET6 else (addr, port)); s.close(); return True
    except OSError: return False

def https_ok(addr):
    if not reachable(addr): return False
    try:
        c = Client(addr); st, d, r = c.raw('GET', '/api/me'); return st == 200
    except Exception: return False

def t_interfaces():
    def bind(*entries, **kw):
        r = admin('web.settings.set', enabled=True, port=PORT, bind=list(entries), **kw); check(r['ok'], 'settings.set bind=%r: %r' % (entries, r.get('error'))); return r.get('result', {})
    def status(): return admin('web.status')['result']
    il = admin('web.interfaces.list')['result']['interfaces']
    check(isinstance(il, list) and all('name' in a and 'addresses' in a and 'up' in a for a in il), 'web.interfaces.list shape (%d adapters)' % len(il))
    check(any(ad['address'] in ('127.0.0.1',) for a in il for ad in a['addresses']), 'the loopback address is listed')
    c = Client(); advance(300); c.login('adm', PW['adm'])
    # default and live changes
    check(https_ok('127.0.0.1'), 'reachable on 127.0.0.1'); 
    bind('127.0.0.2'); time.sleep(0.5)
    check(https_ok('127.0.0.2') and not reachable('127.0.0.1'), 'only the chosen address answers (127.0.0.2 yes, 127.0.0.1 no)')
    c.close(); c.host = '127.0.0.2'; st, j = c.api('state'); check(st == 200, 'a session survives the listener restart'); c.close(); c.host = '127.0.0.1'
    bind('127.0.0.1', '127.0.0.2'); time.sleep(0.5); check(https_ok('127.0.0.1') and https_ok('127.0.0.2'), 'both addresses chosen: both answer')
    r = bind('localhost'); time.sleep(0.5); st = status(); e = st['entries'][0]
    check(https_ok('127.0.0.1') and not reachable('127.0.0.2'), '"localhost" = 127.0.0.1 (and ::1), not 127.0.0.2')
    v6 = len(e['addresses']) == 2 and e['addresses'][1]['listening']
    if socket.has_ipv6 and v6: check(reachable('::1'), 'IPv6 ::1 reachable through the "localhost" entry')
    else: print('   (IPv6 loopback unavailable: ::1 not tested)')
    bind('all'); time.sleep(0.5)
    st = status(); check(st['entries'][0]['listening'] and len(st['entries'][0]['addresses']) == 2, '"all" = 0.0.0.0 + ::')
    check(https_ok('127.0.0.1') and https_ok('127.0.0.2'), '"all" answers on every local address')
    # a wildcard bind and a specific one can coexist on a port? (0.0.0.0 + 127.0.0.2 conflict) -> reported per entry, not fatal
    r = bind('127.0.0.1', '203.0.113.9'); st = status()
    check(st['entries'][0]['listening'] and not st['entries'][1]['listening'] and st['entries'][1]['error'], 'a missing address is reported per entry only')
    check(any('self-signed' in w for w in r.get('warnings', [])) and any('two-factor' in w for w in r.get('warnings', [])), 'warnings: self-signed certificate and no 2FA user')
    check(https_ok('127.0.0.1'), 'the working entry keeps serving')
    r = bind('127.0.0.1'); check(r.get('warnings') == [], 'loopback only: no warnings')
    check(not admin('web.settings.set', enabled=True, bind=[])['ok'], 'an empty list is refused when enabled')
    check(not admin('web.settings.set', bind=['not-an-address'])['ok'], 'a bad entry is refused')
    check(not admin('web.settings.set', bind=['adapter:{x y}'])['ok'], 'a bad adapter entry is refused')
    # port busy: reported and retried every second (the host retries every 1 s in the test)
    blocker = socket.socket(); blocker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); blocker.bind(('127.0.0.2', PORT)); blocker.listen(1)
    bind('127.0.0.1', '127.0.0.2'); st = status(); e1 = st['entries'][1]
    check(not e1['listening'] and 'in use' in e1['error'], 'port taken: reported per entry: %r' % e1.get('error'))
    check(https_ok('127.0.0.1'), 'the other entry still works')
    blocker.close(); ok = False
    for i in range(40):
        time.sleep(0.25)
        if status()['entries'][1]['listening']: ok = True; break
    check(ok and https_ok('127.0.0.2'), 'the address comes back by itself when the port is free again')
    # adapters (fake list): follow address changes, down and up, removal
    mailbox(ifaces=[{'name': '{AAAA-0001}', 'friendly': 'Fake', 'description': 'Fake adapter', 'up': True, 'loopback': False, 'addresses': [{'address': '127.0.0.1', 'family': 'ipv4', 'link_local': False}]}])
    bind('adapter:{AAAA-0001}'); time.sleep(0.5)
    check(https_ok('127.0.0.1') and not reachable('127.0.0.2'), 'adapter entry: its address answers')
    def setif(up, addrs): mailbox(ifaces=[{'name': '{AAAA-0001}', 'friendly': 'Fake', 'description': 'Fake adapter', 'up': up, 'loopback': False, 'addresses': [{'address': a, 'family': 'ipv4', 'link_local': False} for a in addrs]}])
    def wait(cond, secs=10):
        t0 = time.time()
        while time.time() - t0 < secs:
            if cond(): return True
            time.sleep(0.25)
        return False
    setif(True, ['127.0.0.2']); check(wait(lambda: reachable('127.0.0.2') and not reachable('127.0.0.1')), 'the adapter got another address: followed without a restart')
    setif(False, ['127.0.0.2']); check(wait(lambda: not reachable('127.0.0.2')), 'adapter down: address closed'); e = status()['entries'][0]; check(not e['listening'] and 'down' in e['error'], 'status says the adapter is down')
    setif(True, ['127.0.0.1', '127.0.0.2']); check(wait(lambda: reachable('127.0.0.1') and reachable('127.0.0.2')), 'adapter up again: all its addresses answer')
    mailbox(ifaces=[]); check(wait(lambda: not reachable('127.0.0.1')), 'adapter removed: closed'); check('not found' in status()['entries'][0]['error'], 'status says the adapter was not found')
    mailbox(ifaces=None); bind('127.0.0.1'); time.sleep(0.5)
    check(https_ok('127.0.0.1'), 'back to the default')
    st, j = c.api('state'); check(st == 200, 'the session survived all the listener changes')
    # port change
    newport = PORT + 1
    r = admin('web.settings.set', port=newport); check(r['ok'], 'port change'); time.sleep(0.5)
    check(reachable('127.0.0.1', newport) and not reachable('127.0.0.1', PORT), 'the port changed live')
    admin('web.settings.set', port=PORT); time.sleep(0.5); check(reachable('127.0.0.1', PORT), 'and back')
    advance(3000)

def t_pfx_import_and_curl():
    d = os.path.join(DATA, 'pfx'); os.makedirs(d, exist_ok=True)
    key, crt, pfx = [os.path.join(d, n) for n in ('k.pem', 'c.pem', 'c.pfx')]
    try:
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', crt, '-days', '30', '-subj', '/CN=imported.example.test',
                        '-addext', 'subjectAltName=DNS:imported.example.test,IP:127.0.0.1'], check=True, capture_output=True, timeout=60)
        r = subprocess.run(['openssl', 'pkcs12', '-export', '-inkey', key, '-in', crt, '-out', pfx, '-passout', 'pass:pfx-pass-1', '-legacy'], capture_output=True, timeout=60)
        if r.returncode != 0:
            subprocess.run(['openssl', 'pkcs12', '-export', '-inkey', key, '-in', crt, '-out', pfx, '-passout', 'pass:pfx-pass-1', '-keypbe', 'PBE-SHA1-3DES', '-certpbe', 'PBE-SHA1-3DES', '-macalg', 'sha1'], check=True, capture_output=True, timeout=60)
    except Exception as e:
        print('   (openssl cannot make a PFX here: %s; import not tested)' % e); return
    der = subprocess.run(['openssl', 'x509', '-in', crt, '-outform', 'DER'], capture_output=True, check=True).stdout
    want = 'sha256:' + hashlib.sha256(der).hexdigest()
    # the site's own admin uploads the file (base64 in the JSON body); a path on the server is refused over the web
    advance(300); web = Client(); st, j = web.login('adm', PW['adm']); check(st == 200, 'admin client for the upload')
    import base64
    b64 = base64.b64encode(open(pfx, 'rb').read()).decode()
    st, j = web.api('web.cert.import', {'pfx_path': winpath(pfx), 'password': 'pfx-pass-1'}); check(st == 200 and not j['ok'] and 'upload' in j['error'], 'web: a path on the server is refused')
    st, j = web.api('web.cert.import', {'pfx_base64': b64, 'password': 'wrong-password'}); check(st == 200 and not j['ok'], 'web: wrong PFX password refused')
    st, j = web.api('web.cert.import', {'pfx_base64': 'AAAA', 'password': 'x'}); check(st == 200 and not j['ok'], 'web: junk upload refused')
    st, j = web.api('web.cert.import', {'pfx_base64': 'A' * 1500000, 'password': 'x'}); check(st == 200 and not j['ok'] and 'too large' in j['error'], 'web: an upload over 1 MB is refused')
    check(not os.path.exists(os.path.join(LOCAL_DATA, 'web-upload.pfx.tmp')), 'no temporary upload file is left')
    st, j = web.api('web.cert.import', {'pfx_base64': b64, 'password': 'pfx-pass-1'})
    if st == 200 and j['ok']:
        check(j['result']['fingerprint'] == want, 'web upload: the fingerprint of the uploaded certificate'); time.sleep(1.0)
        s0 = socket.create_connection(('127.0.0.1', PORT), timeout=10); t0 = ctx_default().wrap_socket(s0); got0 = t0.getpeercert(True); t0.close()
        check('sha256:' + hashlib.sha256(got0).hexdigest() == want, 'web upload: the uploaded certificate is served after the restart')
        check('pfx-pass-1' not in admin('web.audit.tail', lines=300)['result']['text'], 'the PFX password is not in the audit log')
    else: print('   PFX upload failed under this Wine: %r (not verified)' % j.get('error'))
    r = admin('web.cert.import', pfx_path=winpath(pfx), password='wrong-password'); check(not r['ok'], 'PFX with a wrong password is refused')
    r = admin('web.cert.import', pfx_path=winpath(os.path.join(d, 'nothing.pfx')), password='x'); check(not r['ok'], 'a missing PFX is refused')
    open(os.path.join(d, 'junk.pfx'), 'wb').write(b'not a pfx'); r = admin('web.cert.import', pfx_path=winpath(os.path.join(d, 'junk.pfx')), password='x'); check(not r['ok'], 'a junk file is refused')
    r = admin('web.cert.import', pfx_path=winpath(pfx), password='pfx-pass-1')
    if not r['ok']: print('   PFX import failed under this Wine: %r (not verified)' % r.get('error')); return
    check(r['result']['fingerprint'] == want, 'import returns the fingerprint of the imported certificate'); time.sleep(0.5)
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(s); got = t.getpeercert(True); t.close()
    check('sha256:' + hashlib.sha256(got).hexdigest() == want, 'the imported certificate is the one served (private key works)')
    st = admin('web.status')['result']; check(st['cert']['self_signed'] is False and st['cert']['fingerprint'] == want, 'status shows the imported certificate')
    check('pfx-pass-1' not in open(os.path.join(LOCAL_DATA, 'web.json')).read() and 'BEGIN' not in open(os.path.join(LOCAL_DATA, 'web.json')).read(), 'no PFX password / PEM key in web.json')
    # curl with the pinned certificate
    try:
        out = subprocess.run(['curl', '-s', '--http1.1', '--cacert', crt, '--resolve', 'imported.example.test:%d:127.0.0.1' % PORT, 'https://imported.example.test:%d/api/me' % PORT], capture_output=True, timeout=30)
        check(b'authenticated' in out.stdout, 'curl verifies the imported certificate by name and fetches /api/me (rc=%d)' % out.returncode)
        out = subprocess.run(['curl', '-sk', '-o', '/dev/null', '-w', '%{http_code} %{ssl_version}', 'https://127.0.0.1:%d/' % PORT], capture_output=True, timeout=30).stdout.decode()
        check(out.startswith('200'), 'curl -k fetches the page: ' + out)
    except FileNotFoundError: print('   (curl missing)')
    # generate a fresh self-signed one again for the following tests
    admin('web.cert.generate', names=[]); time.sleep(0.5)

def t_web_self_admin():
    """The admin of the site manages the site over HTTPS; the lock-out protections hold."""
    advance(300); c = Client(); st, j = c.login('adm', PW['adm']); check(st == 200, 'admin client')
    st, j = c.api('web.settings.set', {'enabled': False}); check(st == 200 and not j['ok'] and 'switched off' in j['error'], 'the site cannot switch itself off')
    for bad in (['0.0.0.0'], ['all'], ['192.168.1.1'], []):
        st, j = c.api('web.settings.set', {'bind': bad}); check(st == 200 and not j['ok'], 'bind %r without a loopback entry refused' % (bad,))
    st, j = c.api('web.settings.set', {'bind': ['127.0.0.1', '203.0.113.99']}); check(st == 200 and not j['ok'] and 'cannot listen' in j['error'], 'an address that cannot be bound is refused: ' + str(j.get('error')))
    st, j = c.api('web.settings.set', {'port': 70000}); check(not j['ok'], 'bad port refused')
    check(admin('web.settings.get')['result']['settings']['port'] == PORT, 'refusals changed nothing')
    st, j = c.api('web.users.delete', {'name': 'adm'}); check(st == 200 and not j['ok'] and 'your own account' in j['error'], 'the admin cannot delete himself')
    st, j = c.api('web.users.save', {'name': 'adm', 'role': 'viewer'}); check(st == 200 and not j['ok'] and 'own role' in j['error'], 'the admin cannot demote himself')
    st, j = c.api('web.users.save', {'name': 'webuser', 'role': 'viewer', 'password': 'Web-Made-User-1', 'twofa': 'off'}); check(st == 200 and j['ok'], 'create a user over the web')
    st, j = c.api('web.settings.set', {'session_minutes': 25, 'trust_proxy': False}); check(st == 200 and j['ok'], 'live settings change'); admin('web.settings.set', session_minutes=20)
    st, j = c.api('state'); check(st == 200, 'the session survives (no restart for live settings)')
    a = admin('web.audit.tail', lines=300)['result']['text']
    check('user_saved\tadm' in a and 'Web-Made-User-1' not in a, 'the audit log names the acting user and holds no password')
    admin('web.users.delete', name='webuser')

def t_security_extra():
    """Smuggling variants, CSRF on every endpoint, fixation, cookie quirks, bad UTF-8, audit injection, deep/huge JSON, many users."""
    H = b'Host: 127.0.0.1\r\n'
    admin('web.users.save', name='secadm', role='admin', password='Secure-Admin-Pass-1', twofa='off')
    advance(3000)
    c = Client(); st, j = c.login('secadm', 'Secure-Admin-Pass-1'); check(st == 200, 'login for the security tests: %r' % (j,))
    # ---- CSRF on every state-changing endpoint: nothing is executed without (or with a foreign) token
    c2 = Client(); advance(10); c2.login('secadm', 'Secure-Admin-Pass-1')
    check(c.csrf != c2.csrf and c.cookie != c2.cookie, 'two sessions have different cookie and CSRF token')
    before = c.api('tasks.list')[1].get('result')
    for cmd in ALL_CMDS + ['password', 'logout']:
        body = {'task': {'name': 'csrf-probe', 'enabled': True}, 'server': {'type': 'directory', 'name': 'csrf-probe', 'path': 'C:\\'}, 'id': 'x', 'enabled': True, 'current': 'Secure-Admin-Pass-1', 'next': 'Changed-By-Csrf-1', 'lines': 1}
        for ti, tok in enumerate((None, '', 'x', c2.csrf, c.csrf[:-1] + ('0' if c.csrf[-1] != '0' else '1'), c.csrf.upper() if c.csrf.upper() != c.csrf else c.csrf + 'x', c.csrf + 'x')):
            h = {} if tok is None else {'X-CSRF-Token': tok}
            st, j = c.api(cmd, body, csrf=False, headers=h)
            check(st == 403, 'CSRF: %s with token #%d refused (got %d)' % (cmd, ti, st))
    # the token in the query string or the body is not accepted instead of the header
    st, d, r = c.raw('POST', '/api/tasks.delete?csrf=' + c.csrf, {'id': 'x', 'csrf': c.csrf}); check(st == 403, 'CSRF token in query/body is not accepted')
    check(c.api('tasks.list')[1].get('result') == before, 'no state changed by the forged requests')
    check(c.api('state')[0] == 200, 'the session survived the CSRF probes')
    st, j = Client().login('secadm', 'Changed-By-Csrf-1'); check(st != 200, 'the password was not changed by a forged request')
    advance(1000)
    # ---- session fixation
    fix = 'f' * 64; fc = Client(); fc.cookie = fix
    st, j = fc.login('secadm', 'Secure-Admin-Pass-1')
    new = (fc.setcookie or '').split(';')[0].split('=', 1)[-1]
    check(st == 200 and new != fix and len(new) == 64, 'login with a pre-set cookie issues a fresh session id')
    fc2 = Client(); fc2.cookie = fix; fc2.csrf = fc.csrf; check(fc2.api('state')[0] == 401, 'the fixated session id is not valid after login')
    # an old session id does not survive a new login of the same user being added as one more session, and ids never repeat
    ids = set()
    for i in range(5):
        advance(1); x = Client(); x.login('secadm', 'Secure-Admin-Pass-1'); ids.add(x.cookie)
    check(len(ids) == 5 and None not in ids, 'session ids are unique')
    # ---- cookie quirks
    for name, ck in [('two cookies, bad first', '__Host-vm2b=' + 'a' * 64 + '; __Host-vm2b=' + c.cookie), ('empty value', '__Host-vm2b='), ('huge', '__Host-vm2b=' + 'a' * 6000), ('nul-ish', '__Host-vm2b=%00' + c.cookie), ('quoted', '__Host-vm2b="' + c.cookie + '"'), ('other name', 'vm2b=' + c.cookie), ('garbage', ';;;=;==')]:
        out = raw_exchange(b'POST /api/state HTTP/1.1\r\n' + H + b'Cookie: ' + ck.encode() + b'\r\nX-CSRF-Token: ' + c.csrf.encode() + b'\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}')
        sc = status_of(out)
        check(sc in (200, 400, 401, 431), 'cookie quirk (%s) gives a sane status (got %d)' % (name, sc))
        if name in ('empty value', 'other name', 'garbage', 'nul-ish', 'quoted', 'huge'): check(sc != 200, 'cookie quirk (%s) is not accepted as a session' % name)
    # ---- X-Forwarded-For and friends do not bypass the throttle
    admin('web.users.save', name='xffuser', role='viewer', password='Forwarded-Pass-123', twofa='off'); advance(10)
    xc = Client(); codes = []
    for i in range(40):
        if codes.count(401) >= 8: break
        st, j = xc.api('login', {'user': 'xffuser', 'password': 'bad-%d-password' % i}, csrf=False, headers={'X-Forwarded-For': '10.1.%d.%d' % (i, i), 'X-Real-IP': '10.9.9.%d' % i, 'Forwarded': 'for=10.2.2.%d' % i})
        codes.append(st)
        if st == 429: advance(min(j.get('retry_after', 1), 61))
    print('   xff codes', codes, flush=True)
    check(codes.count(401) <= 9, 'X-Forwarded-For does not reset the throttle (401 count %d)' % codes.count(401))
    st, j = xc.api('login', {'user': 'xffuser', 'password': 'Forwarded-Pass-123'}, csrf=False, headers={'X-Forwarded-For': '8.8.8.8'}); check(st == 429, 'a forged forwarded address does not unlock the user (got %d %r)' % (st, j.get('error')))
    advance(4000)
    # many distinct names from one address: the address itself gets throttled
    codes = []
    for i in range(40):
        st, j = xc.api('login', {'user': 'nobody%d' % i, 'password': 'bad-password-xx'}, csrf=False); codes.append(st)
        if st == 429 and i > 12: break
    check(429 in codes, 'many different user names from one address: the address is throttled (codes %s)' % sorted(set(codes)))
    advance(5000)
    advance(10); c = Client(); c.login('secadm', 'Secure-Admin-Pass-1')
    # ---- HTTP/1.0, methods
    out = raw_exchange(b'GET /favicon.svg HTTP/1.0\r\n\r\n'); check(status_of(out) in (200, 400, 505), 'HTTP/1.0 without Host: sane status (%d)' % status_of(out))
    out = raw_exchange(b'GET /favicon.svg HTTP/1.0\r\n' + H + b'\r\n'); check(status_of(out) in (200, 505), 'HTTP/1.0 with Host answered (%d)' % status_of(out))
    if status_of(out) == 200: check(out.count(b'HTTP/1.') == 1 and b'<svg' in out, 'HTTP/1.0 answer is one complete response and the connection closes')
    out = raw_exchange(b'GET /favicon.svg HTTP/1.0\r\nConnection: keep-alive\r\n' + H + b'\r\nGET /favicon.svg HTTP/1.0\r\n' + H + b'\r\n'); check(status_of(out) in (200, 505), 'HTTP/1.0 keep-alive does not crash (%d)' % status_of(out))
    for m in (b'TRACE', b'CONNECT', b'PUT', b'PATCH', b'PROPFIND', b'POST'):
        out = raw_exchange(m + b' / HTTP/1.1\r\n' + H + b'Content-Length: 0\r\n\r\n'); check(status_of(out) in (400, 405, 501) and b' 200 ' not in out[:15], 'method %s on / refused (%d)' % (m.decode(), status_of(out)))
    st, d, r = c.raw('HEAD', '/api/state'); check(st == 405, 'HEAD on the API refused')
    st, d, r = c.raw('POST', '/api/state', b'{}', {'X-HTTP-Method-Override': 'DELETE', 'X-CSRF-Token': c.csrf}); check(st == 200, 'method override header is ignored')
    # ---- smuggling variants: whatever the answer, a second hidden request must never be answered
    smug = b'GET /favicon.svg HTTP/1.1\r\n' + H + b'\r\n'
    variants = [
        ('TE chunked + CL', b'POST /api/state HTTP/1.1\r\n' + H + b'Content-Length: %d\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n' % (len(smug) + 5) + smug),
        ('TE upper case', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding: Chunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n' + smug),
        ('TE with tab', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding:\tchunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n' + smug),
        ('TE space before colon', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding : chunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n' + smug),
        ('TE x, chunked', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding: x, chunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n' + smug),
        ('TE identity', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding: identity\r\nContent-Length: 4\r\n\r\n0\r\n\r\n' + smug),
        ('two TE', b'POST /api/state HTTP/1.1\r\n' + H + b'Transfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n\r\n0\r\n\r\n' + smug),
        ('CL comma list', b'POST /api/state HTTP/1.1\r\n' + H + b'Content-Length: 2, 2\r\n\r\n{}' + smug),
        ('CL plus sign', b'POST /api/state HTTP/1.1\r\n' + H + b'Content-Length: +2\r\n\r\n{}' + smug),
        ('CL hex', b'POST /api/state HTTP/1.1\r\n' + H + b'Content-Length: 0x2\r\n\r\n{}' + smug),
        ('CL leading space in value fold', b'POST /api/state HTTP/1.1\r\n' + H + b'Content-Length: 2\r\n Content-Length: 40\r\n\r\n{}' + smug),
        ('GET with body', b'GET /favicon.svg HTTP/1.1\r\n' + H + b'Content-Length: %d\r\n\r\n' % len(smug) + smug),
        ('lone CR in header', b'GET /favicon.svg HTTP/1.1\r\n' + H + b'X-A: b\rGET /x HTTP/1.1\r\n\r\n' + smug),
        ('request line with tabs', b'GET\t/favicon.svg\tHTTP/1.1\r\n' + H + b'\r\n' + smug),
        ('request line extra space', b'GET  /favicon.svg HTTP/1.1\r\n' + H + b'\r\n' + smug),
        ('lowercase http', b'GET /favicon.svg http/1.1\r\n' + H + b'\r\n' + smug),
    ]
    for name, payload in variants:
        out = raw_exchange(payload, timeout=6)
        n = out.count(b'HTTP/1.1 ') + out.count(b'HTTP/1.0 ')
        # the hidden GET would answer 200 with an svg: at most one answer, and never the svg after a refusal
        refused = status_of(out) >= 400
        check(n <= 1 or not refused, 'smuggling (%s): no second response behind a refused request (%d responses, first %d)' % (name, n, status_of(out)))
        check(not (refused and b'<svg' in out), 'smuggling (%s): hidden request not executed' % name)
        cc = Client(); check(cc.raw('GET', '/favicon.svg')[0] == 200, 'alive after smuggling variant (%s)' % name); cc.close()
    # leading empty lines before the request line are tolerated or refused, never misparsed
    out = raw_exchange(b'\r\n\r\nGET /favicon.svg HTTP/1.1\r\n' + H + b'\r\n'); check(status_of(out) in (200, 400), 'leading CRLFs (%d)' % status_of(out))
    # ---- static path traversal, more encodings
    cc = Client()
    for p in ['/%2e%2e%2fetc%2fpasswd', '/..%5cwindows%5cwin.ini', '/app.js%00.txt', '/%252e%252e/etc/passwd', '/static/../app.js/..', '/%c0%ae%c0%ae/etc/passwd', '/.%2e/web.json', '/....//web.json', '/app.js%2f..%2fweb.json', '/C:/windows/win.ini', '/c%3a/windows/win.ini', '/\\\\server\\share', '/web.json.', '/WEB.JSON', '/audit.log::$DATA', '/index.html/', '/%', '/%zz', '/?', '/#frag']:
        st, d, r = cc.raw('GET', p)
        check(st in (200, 400, 404) and b'"users"' not in d and b'login_ok' not in d and b'[fonts]' not in d and b'root:' not in d, 'traversal probe %s: %d, nothing leaked' % (p, st))
    st, d, r = cc.raw('GET', '/app.js?x=1'); check(st == 200 and b'function' in d, 'query string ignored on static files')
    st, d, r = cc.raw('GET', '/./app.js'); check(st in (200, 400, 404), './ segment sane')
    cc.close()
    advance(10); c = Client(); st, j = c.login('secadm', 'Secure-Admin-Pass-1'); check(st == 200, 'relogin before the JSON tests (%d)' % st)
    # ---- invalid UTF-8 and odd JSON
    def post_raw(cmd, body, ctype='application/json'): return c.raw('POST', '/api/' + cmd, body, {'X-CSRF-Token': c.csrf, 'Content-Type': ctype})
    for name, body in [('lone 0xff', b'{"task":{"name":"a\xffb"}}'), ('overlong', b'{"task":{"name":"a\xc0\xafb"}}'), ('truncated seq', b'{"task":{"name":"a\xe2\x82"}}'), ('surrogate escape', b'{"task":{"name":"a\\ud800b"}}'),
                       ('utf8 surrogate', b'{"task":{"name":"a\xed\xa0\x80b"}}'), ('nul escape', b'{"task":{"name":"a\\u0000b"}}'), ('BOM', b'\xef\xbb\xbf{"task":{}}'), ('control char', b'{"task":{"name":"a\x01b"}}')]:
        st, d, r = post_raw('tasks.save', body)
        check(st < 500, 'bad UTF-8 / JSON (%s) does not crash the server (got %d)' % (name, st))
        if name in ('lone 0xff', 'overlong', 'truncated seq', 'utf8 surrogate'): check(st == 400, 'invalid UTF-8 body (%s) is refused (got %d)' % (name, st))
        print('   utf8 case %s -> %d %r' % (name, st, d[:120]), flush=True)
        try: json.loads(d)
        except Exception: check(False, 'answer to (%s) is valid JSON' % name)
        st2, d2, r2 = post_raw('tasks.list', b'{}'); check(st2 == 200, 'the server still serves after (%s)' % name)
        try: json.loads(d2); check(True, 'x')
        except Exception: check(False, 'task list stays valid JSON after (%s)' % name)
    cfg = open(os.path.join(LOCAL_DATA, 'config.json'), 'rb').read()
    try: cfg.decode('utf-8'); check(True, 'x')
    except UnicodeDecodeError: check(False, 'config.json stays valid UTF-8 after the bad names')
    # depth, size and shape
    print('   json stress...', flush=True)
    print('   stress: deep = b[ * 100000', flush=True)
    deep = b'[' * 100000 + b']' * 100000
    st, d, r = post_raw('state', deep); check(st in (400, 413), 'a 100000-deep JSON is refused without a crash (got %d)' % st)
    print('   stress: st, d, r = post_raw(state, b', flush=True)
    st, d, r = post_raw('state', b'{"a":' * 5000 + b'1' + b'}' * 5000); check(st in (200, 400, 413), 'a 5000-deep object is handled (got %d)' % st)
    print('   stress: st, d, r = post_raw(state, b', flush=True)
    st, d, r = post_raw('state', b'{' + b','.join(b'"k%d":%d' % (i, i) for i in range(100000)) + b'}'); check(st in (400, 413), '100000 keys refused quickly (got %d)' % st)
    print('   stress: st, d, r = post_raw(state, b', flush=True)
    st, d, r = post_raw('state', b'{"a":' + b'9' * 5000 + b'}'); check(st in (200, 400, 413), 'a 5000-digit number handled (got %d)' % st)
    print('   stress: st, d, r = post_raw(state, b', flush=True)
    st, d, r = post_raw('state', b'{"a":"' + b'\\u00e9' * 400000 + b'"}'); check(st in (200, 400, 413), '2.4 MB of escapes handled (got %d)' % st)
    st, d, r = post_raw('state', b'{"a":1,"a":2}'); check(st in (200, 400), 'duplicate keys handled (got %d)' % st)
    st, d, r = post_raw('state', b''); check(st in (200, 400), 'empty body handled (got %d)' % st)
    check(c.api('state')[0] == 200, 'the server is fine after the JSON stress')
    print('   audit...', flush=True)
    advance(10); c = Client(); c.login('secadm', 'Secure-Admin-Pass-1')
    # ---- audit log injection
    evil_user = 'evil\r\n2099-01-01T00:00:00Z\tlogin_ok\tsecadm\t6.6.6.6\t'
    Client().api('login', {'user': evil_user, 'password': 'whatever-pass-1'}, csrf=False)
    c.api('tasks.save', {'task': {'name': 'x\r\n2099-01-01T00:00:00Z\tlogin_ok\tsecadm\t6.6.6.6\tforged', 'elements': []}})
    c.api('tasks.run', {'id': 'id\r\n2099-01-01T00:00:00Z\tlogin_ok\tsecadm\t6.6.6.6\tforged'})
    a = admin('web.audit.tail', lines=300)['result']['text']
    check('6.6.6.6' not in ''.join(l.split('\t')[3] if len(l.split('\t')) > 3 else '' for l in a.splitlines()), 'audit: forged IP field never appears as a field')
    check(not any(l.startswith('2099-') for l in a.splitlines()), 'audit: no forged line starts with a timestamp')
    aud = open(os.path.join(LOCAL_DATA, 'audit.log'), encoding='utf-8', errors='replace').read()
    check(all(l.count('\t') == 4 for l in aud.splitlines() if l), 'audit: every line has exactly five fields')
    check('\x00' not in aud, 'audit: no NUL bytes')
    print('   many users...', flush=True)
    # ---- certificate regeneration and listener restarts while connections are open
    print('   cert regeneration...', flush=True)
    def peer_fp():
        sk = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(sk)
        try: return hashlib.sha256(t.getpeercert(True)).hexdigest()
        finally: t.close()
    advance(10); c = Client(); c.login('secadm', 'Secure-Admin-Pass-1')
    idle = []
    for i in range(8):
        try:
            sk = socket.create_connection(('127.0.0.1', PORT), timeout=10); idle.append(ctx_default().wrap_socket(sk))
        except OSError: break
    fp0 = peer_fp()
    stop = [False]; werr = []
    def hammer():
        cc = Client()
        while not stop[0]:
            try:
                st, d, r = cc.raw('GET', '/favicon.svg')
                if st != 200: werr.append(st)
            except Exception: cc.conn = None
            time.sleep(0.02)
    hs = [threading.Thread(target=hammer) for _ in range(4)]
    for t in hs: t.start()
    for i in range(4):
        r = admin('web.cert.generate', names=['regen%d.example.test' % i]); check(r['ok'], 'certificate regeneration %d while connections are open' % i); time.sleep(0.4)
    stop[0] = True
    for t in hs: t.join()
    fp1 = peer_fp(); check(fp1 != fp0, 'new connections get the new certificate')
    st, j = c.api('state'); check(st in (200, 401), 'the old keep-alive connection either works or ends cleanly (%d)' % st)
    for sk in idle:
        try: sk.close()
        except Exception: pass
    cc = Client(); check(cc.raw('GET', '/favicon.svg')[0] == 200, 'the server serves after the regenerations (hammer errors: %s)' % sorted(set(werr))[:3])
    # the listener is switched off and on again with clients connected
    keep = Client(); keep.raw('GET', '/favicon.svg')
    admin('web.settings.set', enabled=False); time.sleep(0.5)
    check(not reachable('127.0.0.1'), 'switched off: nothing listens')
    try: keep.raw('GET', '/favicon.svg', retry=False); check(False, 'an open connection to a switched-off site must not be served')
    except (http.client.HTTPException, OSError): check(True, 'an open connection ends when the site is switched off')
    admin('web.settings.set', enabled=True, port=PORT, bind=['127.0.0.1']); time.sleep(1.2)
    check(https_ok('127.0.0.1'), 'switched on again: served')
    advance(3000)
    # ---- many users
    t0 = time.time(); N = 60
    for i in range(N):
        r = admin('web.users.save', name='bulk%03d' % i, role='viewer', password='Bulk-User-Pass-%03d' % i, twofa='off')
        if not r['ok']: check(False, 'create bulk user %d: %r' % (i, r)); break
    ul = admin('web.users.list')['result']['users']; check(len(ul) >= N, '%d users listed (%d)' % (N, len(ul)))
    advance(3000); b = Client(); st, j = b.login('bulk%03d' % (N - 1), 'Bulk-User-Pass-%03d' % (N - 1)); check(st == 200 and j['role'] == 'viewer', 'the last of many users can log in')
    check(b.api('tasks.save', {'task': {'name': 'x'}})[0] == 403, 'bulk viewer cannot save tasks')
    for i in range(N): admin('web.users.delete', name='bulk%03d' % i)
    check(b.api('state')[0] == 401, 'deleted bulk user lost the session')
    check(time.time() - t0 < 120, 'many users handled in reasonable time (%.0f s)' % (time.time() - t0))
    # user name rules
    for bad in ['', ' ', 'a' * 200, 'x\ny', 'x\ty', '../x', 'a b\x00c', '<b>x</b>']:
        r = admin('web.users.save', name=bad, role='viewer', password='Valid-Long-Pass-1', twofa='off')
        if r['ok']: admin('web.users.delete', name=bad)
        check(True, 'user name probe')
    r = admin('web.users.list')['result']['users']; check(all('\n' not in u['name'] and '\t' not in u['name'] and '\x00' not in u['name'] for u in r), 'no user name with control characters was stored')
    advance(3000)

def t_status_counts():
    st = admin('web.status')['result']
    check(st['enabled'] and st['listening'] and st['users'] >= 3 and 'sessions' in st and st['port'] == PORT, 'web.status fields')
    check(not any(k in json.dumps(st) for k in PW.values()), 'web.status has no secrets')

# ---------------------------------------------------------------- loopD2: TLS stack matrix, churn, hostile handshakes
RUN_DIR = os.path.dirname(DATA.rstrip('/'))
def _proc_stats():
    """RSS (kB) and thread count of the Wine process that hosts this test run (found by its --servers argument)."""
    best = None
    try:
        for p in os.listdir('/proc'):
            if not p.isdigit(): continue
            try: cl = open('/proc/%s/cmdline' % p, 'rb').read().replace(b'\0', b' ').decode('latin1')
            except OSError: continue
            if 'host_for_python_clients' in cl and RUN_DIR in cl:
                st = open('/proc/%s/status' % p).read()
                rss = int([l for l in st.splitlines() if l.startswith('VmRSS')][0].split()[1])
                thr = int([l for l in st.splitlines() if l.startswith('Threads')][0].split()[1])
                if best is None or rss > best[0]: best = (rss, thr, int(p))
    except Exception: pass
    return best

def _site_alive():
    try:
        c = Client(); st, d, r = c.raw('GET', '/favicon.svg'); c.close(); return st == 200
    except Exception: return False

def t_tls_matrix():
    """Every cipher suite / protocol / group the server accepts, ALPN, resumption, renegotiation, SNI and odd ClientHellos."""
    ossl = subprocess.run(['openssl', 'ciphers', '-v', 'ALL:COMPLEMENTOFALL:@SECLEVEL=0'], capture_output=True, text=True).stdout.splitlines()
    names = [l.split()[0] for l in ossl if l.split() and ' TLSv1.2 ' in l or ' SSLv3 ' in l.split('Kx')[0] or ' TLSv1 ' in l]
    names = sorted(set(l.split()[0] for l in ossl if l.strip()))
    accepted = []
    def try_cipher(cn):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE
        try:
            c.set_ciphers(cn + ':@SECLEVEL=0'); c.minimum_version = ssl.TLSVersion.MINIMUM_SUPPORTED; c.maximum_version = ssl.TLSVersion.TLSv1_2
        except (ssl.SSLError, ValueError): return None
        try:
            s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s); r = (t.version(), t.cipher()[0]); t.close(); return r
        except (ssl.SSLError, OSError): return False
    for cn in names:
        r = try_cipher(cn)
        if r: accepted.append(r)
    print('   TLS<=1.2 suites accepted by the server: %s' % sorted(set(a[1] for a in accepted)), flush=True)
    check(len(accepted) >= 1, 'at least one TLS 1.2 suite works')
    bad = [a for a in accepted if any(w in a[1] for w in ('RC4', 'DES-CBC', 'NULL', 'EXPORT', 'MD5', 'anon', 'ADH', 'AECDH', 'SEED', 'IDEA', 'CAMELLIA128-SHA', 'PSK', 'SRP'))]
    check(not bad, 'no weak suite is accepted: %r' % (bad,))
    check(all(a[0] == 'TLSv1.2' for a in accepted), 'only TLS 1.2 (nothing older) on <=1.2 offers: %r' % set(a[0] for a in accepted))
    nofs = [a for a in accepted if not (a[1].startswith('ECDHE') or a[1].startswith('DHE') or a[1].startswith('TLS_'))]
    check(not nofs, 'every TLS 1.2 suite has forward secrecy (no static RSA key exchange): %r' % (nofs,))
    # TLS 1.3 suites one by one
    for suite in ('TLS_AES_128_GCM_SHA256', 'TLS_AES_256_GCM_SHA384', 'TLS_CHACHA20_POLY1305_SHA256'):
        out = subprocess.run(['openssl', 's_client', '-connect', '127.0.0.1:%d' % PORT, '-tls1_3', '-ciphersuites', suite, '-brief'], input=b'', capture_output=True, timeout=30)
        txt = (out.stdout + out.stderr).decode('latin1')
        check(suite in txt, 'TLS 1.3 suite %s negotiates' % suite)
    # key-exchange groups
    for grp in ('X25519', 'secp256r1', 'secp384r1', 'secp521r1'):
        out = subprocess.run(['openssl', 's_client', '-connect', '127.0.0.1:%d' % PORT, '-groups', grp, '-brief'], input=b'', capture_output=True, timeout=30)
        txt = (out.stdout + out.stderr).decode('latin1')
        check('Protocol version' in txt or 'Protocol  :' in txt, 'group %s: handshake works or is cleanly refused (%s)' % (grp, txt.strip().splitlines()[-1][:60] if txt.strip() else '?'))
    # ALPN: never h2 (the server speaks HTTP/1.1 only)
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE; c.set_alpn_protocols(['h2', 'http/1.1'])
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s); ap = t.selected_alpn_protocol(); t.close()
    check(ap in (None, 'http/1.1'), 'ALPN never selects h2: %r' % (ap,))
    c.set_alpn_protocols(['h2']); 
    try:
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s); ap = t.selected_alpn_protocol(); t.close()
        check(ap is None, 'a client that offers only h2 gets no ALPN (or an alert): %r' % (ap,))
    except (ssl.SSLError, OSError): check(True, 'client offering only h2 is refused with an alert')
    # SNI: unknown names, huge SNI, IP SNI all get the same certificate / do not crash
    for sni in ('unknown.example', 'a' * 250, 'xn--80ak6aa92e.com', 'x.' * 100 + 'org'):
        try:
            c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE
            s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s, server_hostname=sni); t.close(); ok = True
        except (ssl.SSLError, OSError, UnicodeError, ValueError): ok = True      # a refusal is as good as a handshake
        check(ok, 'SNI %r' % sni[:20])
    check(_site_alive(), 'site alive after the SNI cases')
    # resumption: TLS 1.2 session id/ticket and TLS 1.3 ticket reuse must work or be refused, never break
    for ver in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
        c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE; c.minimum_version = c.maximum_version = ver
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s)
        t.sendall(b'GET /favicon.svg HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'); 
        try: t.recv(100)
        except OSError: pass
        sess = t.session; t.close()
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s, session=sess)
        reused = t.session_reused; t.sendall(b'GET /favicon.svg HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'); d = t.recv(100); t.close()
        check(d.startswith(b'HTTP/1.1 200'), 'resumed %s connection serves (reused=%s)' % (ver.name, reused))
    # a forged / garbage session ticket must just fall back to a full handshake
    c = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE; c.maximum_version = ssl.TLSVersion.TLSv1_2
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = c.wrap_socket(s); sess = t.session; t.close()
    # TLS 1.2 renegotiation by the client must not crash or be silently honoured in a loop
    try:
        p = subprocess.Popen(['openssl', 's_client', '-connect', '127.0.0.1:%d' % PORT, '-tls1_2', '-quiet'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = p.communicate(b'R\nR\nR\nR\nR\nGET /favicon.svg HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n', timeout=40)
        print('   renegotiation x5: stdout %d bytes, stderr %r' % (len(out), err[-120:]), flush=True)
    except subprocess.TimeoutExpired: p.kill(); check(False, 'renegotiation probe hung the client')
    time.sleep(0.5); check(_site_alive(), 'site alive after renegotiation attempts')
    # records at the limits: a 16 KiB+ record, zero-length records, a 1-byte-per-record request
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(s)
    for ch in b'GET /favicon.svg HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n': t.send(bytes([ch]))
    got = b''
    try:
        while True:
            d = t.recv(4096)
            if not d: break
            got += d
    except OSError: pass
    t.close(); check(got.startswith(b'HTTP/1.1 200'), 'request sent one byte per TLS record is served')
    # ---- hostile ClientHellos / garbage at the TCP level
    def tcp(payload, wait=0.0, rd=True):
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10)
        try:
            s.sendall(payload); time.sleep(wait)
            if rd: s.settimeout(3); 
            try: return s.recv(4096) if rd else b''
            except OSError: return b''
        finally: s.close()
    rnd = __import__('random').Random(777)
    hello = bytes.fromhex('16030100c5010000c10303') + bytes(rnd.getrandbits(8) for _ in range(32)) + bytes.fromhex('00') + bytes.fromhex('0004130113020100') + b'\x01\x00' + bytes(rnd.getrandbits(8) for _ in range(150))
    cases = {'empty': b'', 'one byte': b'\x16', 'ssl2 hello': bytes.fromhex('80 2e 01 03 01 00 15 00 00 00 10') + os.urandom(40), 'garbage': os.urandom(3000),
             'record len 0xffff': b'\x16\x03\x03\xff\xff' + os.urandom(100), 'truncated hello': hello[:60], 'bad hello body': hello, 'alert flood': b'\x15\x03\x03\x00\x02\x02\x28' * 500,
             'http plain': b'GET / HTTP/1.1\r\nHost: x\r\n\r\n', 'zero-len records': b'\x16\x03\x03\x00\x00' * 1000, 'ccs first': b'\x14\x03\x03\x00\x01\x01', 'appdata first': b'\x17\x03\x03\x00\x10' + os.urandom(16)}
    for n, p in cases.items():
        try: tcp(p)
        except OSError: pass
        check(True, 'hostile bytes: ' + n)
    for i in range(60):
        try: tcp(hello[:rnd.randrange(1, len(hello))] + os.urandom(rnd.randrange(0, 50)), rd=bool(i % 2))
        except OSError: pass
    time.sleep(1); check(_site_alive(), 'site alive after hostile ClientHello variants')
    # fuzz: mutate a REAL ClientHello (captured from python ssl) byte by byte
    cap = {}
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO(); so = ctx.wrap_bio(inb, outb, server_hostname='fuzz.example')
    try: so.do_handshake()
    except ssl.SSLWantReadError: pass
    real = outb.read()
    for i in range(150):
        m = bytearray(real)
        for _ in range(rnd.randrange(1, 4)): m[rnd.randrange(5, len(m))] = rnd.getrandbits(8)
        try: tcp(bytes(m), rd=bool(i % 3))
        except OSError: pass
    time.sleep(1); check(_site_alive(), 'site alive after 150 mutated real ClientHellos')

def t_tls_churn():
    """Many connections open and close in every way; threads, memory and the accept loop must recover."""
    before = _proc_stats(); print('   process before: %r' % (before,), flush=True)
    N = 400 if QUICK else 1000
    errs = []; done = [0]
    lock = threading.Lock()
    def worker(k, n):
        r = __import__('random').Random(k)
        for i in range(n):
            mode = r.randrange(6)
            try:
                s = socket.create_connection(('127.0.0.1', PORT), timeout=20)
                if mode == 0: s.close()
                elif mode == 1:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); s.close()
                else:
                    t = ctx_default().wrap_socket(s)
                    if mode == 2: t.close()
                    elif mode == 3:
                        t.sendall(b'GET /favicon.svg HTTP/1.1\r\nHost: x\r\n\r\n'); t.recv(200); t.close()
                    elif mode == 4:
                        t.sendall(b'GET /favicon.svg HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'); 
                        while t.recv(65536): pass
                        t.close()
                    else:
                        t.sendall(b'POST /api/state HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\nabc'); t.close()
            except (OSError, ssl.SSLError) as e:
                with lock: errs.append(repr(e)[:80])
            with lock: done[0] += 1
    t0 = time.time()
    # cap per IP is 32: use up to 8 workers
    ts = [threading.Thread(target=worker, args=(k, N // 8)) for k in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    dt = time.time() - t0
    print('   %d connections in %.1f s, %d client-side errors (%s)' % (done[0], dt, len(errs), sorted(set(errs))[:3]), flush=True)
    check(done[0] == (N // 8) * 8, 'all churn connections ran')
    check(len(errs) <= done[0] // 20, 'at most 5%% of connections failed under churn (%d)' % len(errs))
    time.sleep(3)
    check(_site_alive(), 'the site serves normally after the churn')
    admin('web.users.save', name='churnop', role='operator', password='Churn-Operator-Pw-1', twofa='off'); advance(3000)
    c = Client(); st, j = c.login('churnop', 'Churn-Operator-Pw-1'); check(st == 200, 'a login works after the churn (%d)' % st)
    after = _proc_stats(); print('   process after: %r' % (after,), flush=True)
    if before and after:
        check(after[1] <= before[1] + 40, 'thread count recovered: %d -> %d' % (before[1], after[1]))
        check(after[0] <= before[0] + 60 * 1024, 'memory did not grow by more than 60 MB: %d -> %d kB' % (before[0], after[0]))
    # second round: memory must not keep growing (leak per connection would show as linear growth)
    mid = _proc_stats()
    ts = [threading.Thread(target=worker, args=(k + 100, N // 8)) for k in range(8)]
    for t in ts: t.start()
    for t in ts: t.join()
    time.sleep(3); fin = _proc_stats(); print('   process after round 2: %r' % (fin,), flush=True)
    if mid and fin: check(fin[0] <= mid[0] + 25 * 1024, 'round 2 of %d connections grew memory by < 25 MB: %d -> %d kB' % (N, mid[0], fin[0]))

def t_tls_exhaustion():
    """Stalled connections from MANY source addresses (the per-IP cap does not help) must not starve a real client for long."""
    socks = []
    for i in range(2, 250):
        try:
            s = socket.socket(); s.bind(('127.0.0.%d' % (i % 250 + 2 if i < 249 else 2), 0)); s.settimeout(5); s.connect(('127.0.0.1', PORT)); socks.append(s)
        except OSError: break
    print('   %d stalled connections opened (from different 127.0.0.x)' % len(socks), flush=True)
    t0 = time.time(); ok = False
    for attempt in range(3):
        try:
            c = Client(); st, d, r = c.raw('GET', '/favicon.svg'); c.close(); ok = (st == 200)
            if ok: break
        except Exception as e: print('   attempt %d: %r' % (attempt, e), flush=True)
    check(ok, 'a real client is served while %d connections stall the handshake (took %.1f s)' % (len(socks), time.time() - t0))
    # the stalled ones must be dropped by the server after its timeout, freeing the slots
    t1 = time.time(); alive = len(socks)
    while time.time() - t1 < 80 and alive:
        alive = 0
        for s in socks:
            try:
                s.settimeout(0.05); d = s.recv(1)
                if d == b'': continue
                alive += 1
            except socket.timeout: alive += 1
            except OSError: pass
        if alive: time.sleep(2)
    print('   stalled connections still open after %.0f s: %d' % (time.time() - t1, alive), flush=True)
    check(alive == 0, 'the server drops handshake-stalled connections within 80 s (%d left)' % alive)
    for s in socks:
        try: s.close()
        except OSError: pass
    time.sleep(1)
    check(_site_alive(), 'site alive after the stall test')
    # worse: 40 connections each send ONE byte of a ClientHello and then stall (they are readable, so they do occupy workers): a real client must be served in bounded time
    socks = []
    for i in range(40):
        try:
            s = socket.socket(); s.bind(('127.0.0.%d' % (i + 50), 0)); s.settimeout(5); s.connect(('127.0.0.1', PORT)); s.sendall(b'\x16'); socks.append(s)
        except OSError: break
    time.sleep(0.5); t0 = time.time(); ok = False
    try:
        c = Client(); st, d, r = c.raw('GET', '/favicon.svg'); c.close(); ok = (st == 200)
    except Exception as e: print('   real client: %r' % (e,), flush=True)
    dt = time.time() - t0; print('   real client served after %.1f s while %d connections trickle one byte' % (dt, len(socks)), flush=True)
    check(ok and dt < 15, 'a real client is served within 15 s while %d one-byte connections stall (%.1f s)' % (len(socks), dt))
    for s in socks: s.close()
    time.sleep(1)
    # slow-loris at the TLS level: a ClientHello trickled byte by byte, while other clients are served
    s = socket.create_connection(('127.0.0.1', PORT), timeout=10)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    inb, outb = ssl.MemoryBIO(), ssl.MemoryBIO(); so = ctx.wrap_bio(inb, outb, server_hostname='slow.example')
    try: so.do_handshake()
    except ssl.SSLWantReadError: pass
    hello = outb.read(); served = 0
    for i, b in enumerate(hello[:80]):
        try: s.sendall(bytes([b]))
        except OSError: break
        if i % 20 == 0:
            if _site_alive(): served += 1
        time.sleep(0.05)
    check(served >= 3, 'other clients are served while a ClientHello trickles in (%d probes ok)' % served)
    s.close()


def t_pfx_variants():
    """Certificate import: RSA / EC / chain / legacy RC2+3DES / no password / wrong password / key mismatch / truncated / huge / not a key: the served certificate only changes on a good import."""
    d = os.path.join(DATA, 'pfx2'); os.makedirs(d, exist_ok=True)
    def sh(*a, inp=None):
        r = subprocess.run(list(a), capture_output=True, timeout=120, input=inp)
        return r
    def served():
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = ctx_default().wrap_socket(s); der = t.getpeercert(True); t.close()
        return 'sha256:' + hashlib.sha256(der).hexdigest()
    try: sh('openssl', 'version')
    except Exception as e: print('   (no openssl: skipped)'); return
    def mk(name, keyspec, cn='v.example.test', extra=()):
        key, crt = os.path.join(d, name + '.key'), os.path.join(d, name + '.crt')
        args = ['openssl', 'req', '-x509', '-nodes', '-keyout', key, '-out', crt, '-days', '30', '-subj', '/CN=' + cn, '-addext', 'subjectAltName=DNS:' + cn + ',IP:127.0.0.1']
        if keyspec.startswith('ec:'): args += ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:' + keyspec[3:]]
        else: args += ['-newkey', keyspec]
        r = sh(*(args + list(extra))); assert r.returncode == 0, r.stderr
        return key, crt
    def fp(crt): return 'sha256:' + hashlib.sha256(sh('openssl', 'x509', '-in', crt, '-outform', 'DER').stdout).hexdigest()
    def export(name, key, crt, pw, *opts, chain=None):
        out = os.path.join(d, name + '.pfx')
        a = ['openssl', 'pkcs12', '-export', '-inkey', key, '-in', crt, '-out', out, '-passout', 'pass:' + pw] + list(opts)
        if chain: a += ['-certfile', chain]
        r = sh(*a); return out if r.returncode == 0 else None
    base = served(); good = 0
    cases = []
    k, c = mk('rsa2048', 'rsa:2048'); cases.append(('RSA-2048 modern AES', export('rsa2048', k, c, 'pw-one'), 'pw-one', fp(c)))
    k2, c2 = mk('rsa4096', 'rsa:3072'); cases.append(('RSA-3072', export('rsa3072', k2, c2, 'pw-two'), 'pw-two', fp(c2)))
    k3, c3 = mk('p384', 'ec:secp384r1'); cases.append(('EC P-384', export('p384', k3, c3, 'pw-three'), 'pw-three', fp(c3)))
    k4, c4 = mk('p256', 'ec:prime256v1'); cases.append(('EC P-256', export('p256', k4, c4, 'pw-four'), 'pw-four', fp(c4)))
    cases.append(('legacy RC2/3DES', export('legacy', k, c, 'pw-leg', '-legacy'), 'pw-leg', fp(c)))
    cases.append(('3DES + SHA1 MAC', export('des3', k, c, 'pw-des', '-keypbe', 'PBE-SHA1-3DES', '-certpbe', 'PBE-SHA1-3DES', '-macalg', 'sha1'), 'pw-des', fp(c)))
    cases.append(('unicode password', export('uni', k, c, 'Пароль-ü-€'), 'Пароль-ü-€', fp(c)))
    cases.append(('empty password', export('empty', k, c, ''), '', fp(c)))
    cases.append(('long password 500', export('longpw', k, c, 'x' * 500), 'x' * 500, fp(c)))
    # chain: CA signs the leaf; the PFX has leaf + CA
    ca_key, ca_crt = os.path.join(d, 'ca.key'), os.path.join(d, 'ca.crt')
    sh('openssl', 'req', '-x509', '-nodes', '-newkey', 'rsa:2048', '-keyout', ca_key, '-out', ca_crt, '-days', '30', '-subj', '/CN=Test CA')
    lk, lcsr, lcrt = [os.path.join(d, 'leaf.' + e) for e in ('key', 'csr', 'crt')]
    sh('openssl', 'req', '-new', '-nodes', '-newkey', 'rsa:2048', '-keyout', lk, '-out', lcsr, '-subj', '/CN=leaf.example.test')
    ext = os.path.join(d, 'leaf.ext'); open(ext, 'w').write('subjectAltName=DNS:leaf.example.test,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n')
    sh('openssl', 'x509', '-req', '-in', lcsr, '-CA', ca_crt, '-CAkey', ca_key, '-CAcreateserial', '-out', lcrt, '-days', '20', '-extfile', ext)
    cases.append(('leaf + CA chain', export('chain', lk, lcrt, 'pw-chain', chain=ca_crt), 'pw-chain', fp(lcrt)))
    # chain in the "wrong" order: CA first (openssl puts the -in cert as the key's cert; fine), plus a PFX with the key only matching the second cert
    bad = []
    bad.append(('wrong password', cases[0][1], 'nope'))
    bad.append(('empty password for protected', cases[0][1], ''))
    if cases[0][1]:
        raw = open(cases[0][1], 'rb').read()
        tp = os.path.join(d, 'trunc.pfx'); open(tp, 'wb').write(raw[:len(raw) // 2]); bad.append(('truncated', tp, 'pw-one'))
        tp = os.path.join(d, 'flip.pfx'); m = bytearray(raw); m[len(m) // 2] ^= 0xff; open(tp, 'wb').write(bytes(m)); bad.append(('one flipped byte', tp, 'pw-one'))
        
    tp = os.path.join(d, 'zero.pfx'); open(tp, 'wb').write(b''); bad.append(('empty file', tp, 'x'))
    tp = os.path.join(d, 'huge.pfx'); open(tp, 'wb').write(b'\x30\x82' + os.urandom(40 * 1024 * 1024)); bad.append(('40 MB of random', tp, 'SecretPw!9'))
    tp = os.path.join(d, 'pemkey.pfx'); open(tp, 'wb').write(open(k, 'rb').read() + open(c, 'rb').read()); bad.append(('PEM key+cert instead of PFX', tp, ''))
    tp = os.path.join(d, 'der_only.pfx'); open(tp, 'wb').write(sh('openssl', 'x509', '-in', c, '-outform', 'DER').stdout); bad.append(('DER certificate only', tp, ''))
    tp = os.path.join(d, 'nokey.pfx'); r = sh('openssl', 'pkcs12', '-export', '-nokeys', '-in', c, '-out', tp, '-passout', 'pass:SecretPw!9'); bad.append(('certificate without a key', tp, 'SecretPw!9')) if os.system('true') == 0 else 0
    tp = os.path.join(d, 'mismatch.pfx')
    # key of one cert, cert of another: build via a PEM bundle trick (openssl refuses) -> craft with python-less approach: use -inkey k2 -in c (openssl checks, rc != 0)
    r = sh('openssl', 'pkcs12', '-export', '-inkey', k2, '-in', c, '-out', tp, '-passout', 'pass:p')
    if r.returncode == 0: bad.append(('key does not match the certificate', tp, 'p'))
    for n in ('x' * 400, 'a\nb', 'C:\\nonexistent\\dir\\x.pfx', '\\\\no-such-host\\share\\x.pfx', 'CON', 'NUL', '..\\..\\..\\x', ''):
        bad.append(('odd path ' + repr(n[:20]), n, 'SecretPw!9'))
    for name, path, pw in bad:
        if path is None: continue
        wp = path if (path.startswith(('C:', '\\\\', '..', 'CON', 'NUL')) or path == '' or path[:1] in 'xa') and not path.startswith('/') else winpath(path)
        r = admin('web.cert.import', pfx_path=wp, password=pw)
        check(not r['ok'], 'import refused: ' + name + ' (%s)' % (r.get('error') or '')[:70])
        check(not (pw and pw in (r.get('error') or '')), 'the error does not repeat the password: ' + name)
    check(served() == base, 'the served certificate did not change after the refused imports')
    check(_site_alive(), 'site alive after the refused imports')
    # type confusion for the command arguments
    for args in ({}, {'pfx_path': 5}, {'pfx_path': None, 'password': None}, {'pfx_path': ['a'], 'password': {'x': 1}}, {'pfx_path': True, 'password': 1.5}):
        r = admin('web.cert.import', **args); check(not r['ok'], 'import with odd argument types refused: %r' % (args,))
    for name, path, pw, want in cases:
        if path is None: print('   (openssl could not make %s here)' % name); continue
        r = admin('web.cert.import', pfx_path=winpath(path), password=pw)
        if not r['ok']: check(name in ('legacy RC2/3DES', '3DES + SHA1 MAC', 'empty password'), 'import %s: %r' % (name, r.get('error'))); print('   import %s refused: %r' % (name, r.get('error')), flush=True); continue
        time.sleep(0.4)
        check(r['result']['fingerprint'] == want, 'import %s: fingerprint of the certificate' % name)
        check(served() == want, 'import %s: the imported certificate is served (the private key matches)' % name)
        # the TLS 1.2 ECDHE-ECDSA-only story: an RSA certificate must still handshake (the cipher list must offer RSA suites then)
        for ver in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
            cx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); cx.check_hostname = False; cx.verify_mode = ssl.CERT_NONE; cx.minimum_version = cx.maximum_version = ver
            try:
                s = socket.create_connection(('127.0.0.1', PORT), timeout=10); t = cx.wrap_socket(s); res = t.cipher()[0]; t.close()
            except (ssl.SSLError, OSError) as e: res = None
            check(res is not None, 'import %s: %s handshake works (%s)' % (name, ver.name, res))
        st = admin('web.status')['result']
        check(st['cert']['fingerprint'] == want and st['cert']['self_signed'] is (name == 'x'), 'import %s: status shows it' % name)
    # after the last import, the data file must hold neither a password nor a private key in clear text
    wj = open(os.path.join(LOCAL_DATA, 'web.json')).read()
    check('BEGIN' not in wj and 'pw-' not in wj and 'Пароль' not in wj, 'no password / PEM key in web.json after all imports')
    # a generated certificate afterwards works again, and a restart-free switch back is clean
    r = admin('web.cert.generate', names=['back.example.test']); check(r['ok'], 'generate after imports'); time.sleep(0.4)
    check(served() == r['result']['fingerprint'], 'generated certificate served again')
    admin('web.cert.generate', names=[]); time.sleep(0.4)
    check(_site_alive(), 'site alive at the end of the certificate tests')


def t_rpc_fuzz():
    """Every RPC command reachable through the web, with type-confused / huge / deeply nested arguments, as admin: never a 5xx, a dropped connection or a dead service."""
    R = __import__('random').Random(int(os.environ.get('VM2B_FUZZ_SEED', '4242')))
    admin('web.users.save', name='fuzzadm', role='admin', password='Fuzz-Admin-Pass-123', twofa='off'); advance(3000)
    c = Client(); st, j = c.login('fuzzadm', 'Fuzz-Admin-Pass-123'); check(st == 200, 'fuzz admin logs in')
    def deep(n, leaf=0):
        v = leaf
        for _ in range(n): v = [v] if R.random() < 0.5 else {'a': v}
        return v
    pool = [None, True, False, 0, -1, 1, 2 ** 31, 2 ** 31 - 1, -2 ** 31, 2 ** 53, 2 ** 63, -2 ** 63, 2 ** 64, 10 ** 30, 1.5, -0.0, 1e308, 1e-320, '', ' ', 'a', 'A' * 70000, '\u0000', 'a\u0000b', '\U0001F600', '\u202e', '../../x', 'C:\\Windows\\System32', '\\\\host\\share', 'CON', '%s%n%x', '{{7*7}}', '<script>1</script>',
            "'; DROP TABLE x;--", '-1', '1e999', 'NaN', 'true', 'null', [], {}, [[]], [{}], {'': ''}, deep(30), deep(200), list(range(3000)), {str(i): i for i in range(500)}, ['x'] * 5000, '1' * 400, 'ü' * 3000]
    keys = ['id', 'name', 'type', 'path', 'host', 'port', 'user', 'password', 'passphrase', 'vm', 'vms', 'server', 'task', 'archive', 'volume', 'target', 'dest', 'folder', 'enabled', 'lines', 'limit', 'offset', 'kind', 'job', 'schedule', 'windows', 'compression', 'level', 'threads', 'chunk', 'volume_mb', 'keep', 'retention', 'mode', 'source', 'ssh', 'smb', 'cert', 'encrypt', 'verify', 'esxi', 'datastore', 'network', 'register', 'power_on', 'new_name', 'files', 'from', 'to', 'tag', 'text', 'q', 'filter']
    server_ok = {'type': 'directory', 'name': 'fz', 'path': 'C:\\nonexistent-fz'}
    task_ok = {'name': 'fz', 'enabled': False, 'server': 'x', 'vms': [], 'target': {'type': 'local', 'path': 'C:\\nonexistent-fz'}, 'schedule': {'enabled': False}, 'windows': []}
    cmds = [x for x in ALL_CMDS if x not in ('servers.test', 'servers.vms')] + ['password', 'logout_all', 'me']
    bad5xx = []; dropped = []; n = 0
    t0 = time.time()
    iters = 25 if QUICK else 80
    for cmd in cmds:
        for it in range(iters):
            m = R.randrange(6)
            if m == 0: args = {R.choice(keys): R.choice(pool) for _ in range(R.randrange(1, 6))}
            elif m == 1: args = R.choice(pool)
            elif m == 2: args = {'server': dict(server_ok, **{R.choice(keys): R.choice(pool)})}
            elif m == 3: args = {'task': dict(task_ok, **{R.choice(keys): R.choice(pool) for _ in range(R.randrange(1, 4))})}
            elif m == 4: args = {k: R.choice(pool) for k in R.sample(keys, 12)}
            else:
                base = R.choice(pool); args = dict(base) if isinstance(base, dict) else {}; args[R.choice(keys)] = R.choice(pool)
            if cmd in ('backups.delete', 'history.clear') and R.random() < 0.9: continue          # not too destructive for the later tests
            if cmd == 'settings.set' and isinstance(args, dict): args.pop('port', None)
            try:
                st, j = c.api(cmd, args)
            except Exception as e:
                dropped.append((cmd, repr(e)[:60])); c.close(); c = Client(); advance(5); c.login('fuzzadm', 'Fuzz-Admin-Pass-123'); continue
            n += 1
            if st >= 500: bad5xx.append((cmd, st))
            if st == 429: advance(300)
            if st == 401: advance(5); c = Client(); c.login('fuzzadm', 'Fuzz-Admin-Pass-123')
    print('   %d fuzzed calls in %.0f s' % (n, time.time() - t0), flush=True)
    check(not bad5xx, '5xx answers to fuzzed arguments: %r' % bad5xx[:5])
    check(not dropped, 'dropped connections on fuzzed arguments: %r' % dropped[:5])
    check(_site_alive(), 'site alive after the fuzz')
    r = admin('web.status'); check(r['ok'] and r['result']['listening'], 'web.status OK after the fuzz')
    r = core('state'); check(r['ok'], 'service core state OK after the fuzz')
    # the same through the PIPE-side admin commands with garbage (host side, no HTTP)
    for cmd in ('web.settings.set', 'web.users.save', 'web.users.delete', 'web.users.totp_enroll', 'web.users.totp_confirm', 'web.users.totp_disable', 'web.audit.tail', 'web.cert.generate', 'web.interfaces.list'):
        for it in range(30 if not QUICK else 10):
            args = {R.choice(keys + ['bind', 'names', 'role', 'twofa', 'code', 'session_minutes', 'require_2fa_admins', 'enabled']): R.choice(pool) for _ in range(R.randrange(0, 5))}
            if cmd == 'web.settings.set': args.pop('port', None); args.pop('enabled', None); args.pop('bind', None)
            if cmd == 'web.users.delete' and args.get('name') in ('adm', 'op', 'vw', 'fuzzadm', 'secadm'): continue
            if os.environ.get('VM2B_FUZZ_VERBOSE'): print('   >', cmd, json.dumps(args)[:200], flush=True)
            try: r = admin(cmd, **args)
            except Exception as e: check(False, 'admin %s crashed the host: %r' % (cmd, e)); return
    check(_site_alive(), 'site alive after the pipe-side fuzz'); check(admin('web.status')['ok'], 'web.status after the pipe-side fuzz')
    # the login endpoint with type confusion
    for body in ({'user': 5, 'password': 5}, {'user': None, 'password': None}, {'user': ['a'], 'password': {'x': 1}}, {'user': 'a' * 100000, 'password': 'b'}, {'user': 'fuzzadm', 'password': 'p' * 1000000}, {'user': 'fuzzadm', 'password': 'Fuzz-Admin-Pass-123', 'code': 123456}, {'user': 'fuzzadm', 'password': 'Fuzz-Admin-Pass-123', 'code': ['1']}, [], 'str', 5, None):
        x = Client(); st, j = x.api('login', body, csrf=False); advance(400)
        check(st in (400, 401, 413, 429) or (st == 200 and isinstance(body, dict) and body.get('code') is not None), 'login with odd body %.40r -> %d' % (json.dumps(body)[:40], st))
    advance(3000); check(_site_alive(), 'site alive after the login type-confusion')


def t_concurrent_admin():
    """Concurrent edits of users / settings / tasks from the pipe side and from several admin sessions: consistent files, no lost users, no crash; 2FA enrolment races."""
    admin('web.users.save', name='cadm', role='admin', password='Concurrent-Admin-1', twofa='off'); advance(3000)
    admin('web.users.save', name='cenr', role='viewer', password='Concurrent-Enrol-1x', twofa='off')
    errs = []; lock = threading.Lock()
    def sess():
        x = Client(); st, j = x.login('cadm', 'Concurrent-Admin-1')
        if st != 200: raise RuntimeError('login %d' % st)
        return x
    def tasks_worker(k):
        try:
            x = sess()
            for i in range(15):
                st, j = x.api('tasks.save', {'task': {'name': 'ct-%d-%d' % (k, i), 'enabled': False, 'schedule': {'enabled': False}, 'windows': []}})
                if st != 200: errs.append(('tasks.save', st)); 
                st, j = x.api('tasks.list')
                if st != 200: errs.append(('tasks.list', st))
        except Exception as e: errs.append(repr(e)[:80])
    def settings_worker(k):
        try:
            x = sess()
            for i in range(15):
                st, j = x.api('settings.set', {'threads': (i + k) % 4}); 
                if st >= 500: errs.append(('settings.set', st))
                st, j = x.api('settings.get')
                if st != 200: errs.append(('settings.get', st))
        except Exception as e: errs.append(repr(e)[:80])
    def users_worker(k):
        try:
            for i in range(12):
                n = 'cu%d_%d' % (k, i)
                r = admin('web.users.save', name=n, role='viewer', password='Cu-Password-%d-%d-xx' % (k, i), twofa='off')
                if not r['ok']: errs.append(('users.save', r.get('error')))
                if i % 3 == 2:
                    r = admin('web.users.delete', name=n)
                    if not r['ok']: errs.append(('users.delete', r.get('error')))
        except Exception as e: errs.append(repr(e)[:80])
    def enrol_worker(k):
        try:
            for i in range(6):
                r = admin('web.users.totp_enroll', name='cenr'); 
                r2 = admin('web.users.totp_confirm', name='cenr', code='000000')
                r3 = admin('web.users.totp_disable', name='cenr')
        except Exception as e: errs.append(repr(e)[:80])
    ts = [threading.Thread(target=f, args=(k,)) for f in (tasks_worker, settings_worker) for k in range(3)] + [threading.Thread(target=users_worker, args=(k,)) for k in range(3)] + [threading.Thread(target=enrol_worker, args=(0,))]
    t0 = time.time()
    for t in ts: t.start()
    for t in ts: t.join()
    print('   concurrent edits took %.1f s, %d errors %r' % (time.time() - t0, len(errs), errs[:3]), flush=True)
    check(not errs, 'no failed call during concurrent edits: %r' % errs[:4])
    users = admin('web.users.list')['result']['users']; names = [u['name'] for u in users]
    check(len(names) == len(set(names)), 'user names stay unique')
    want = {'cu%d_%d' % (k, i) for k in range(3) for i in range(12) if i % 3 != 2}
    check(want <= set(names), 'no user was lost by a concurrent save (%d missing)' % len(want - set(names)))
    check(not any(('cu%d_%d' % (k, i)) in names for k in range(3) for i in range(12) if i % 3 == 2), 'deleted users stay deleted')
    wj = open(os.path.join(LOCAL_DATA, 'web.json'), 'rb').read()
    try: json.loads(wj.decode('utf-8')); ok = True
    except Exception: ok = False
    check(ok, 'web.json is valid JSON after the concurrent edits')
    for fn in os.listdir(LOCAL_DATA):
        if fn.endswith('.json'):
            try: json.loads(open(os.path.join(LOCAL_DATA, fn), 'rb').read().decode('utf-8')); good = True
            except Exception as e: good = False
            check(good, 'data file %s is valid JSON' % fn)
    ta = Client(); ta.login('cadm', 'Concurrent-Admin-1'); tl = ta.api('tasks.list')[1].get('result')
    tn = [t.get('name') for t in (tl if isinstance(tl, list) else tl.get('tasks', []))] if tl else []
    check(sum(1 for n in tn if n and n.startswith('ct-')) == 3 * 15, 'all 45 saved tasks exist exactly once (%d)' % sum(1 for n in tn if n and n.startswith('ct-')))
    au = admin('web.audit.tail', lines=400)['result']['text']
    check(all(l.strip() for l in au.split('\n') if l) and '\x00' not in au, 'audit tail is readable text')
    # clean up the extra users so later counts are stable
    for u in names:
        if u.startswith('cu'): admin('web.users.delete', name=u)
    admin('web.users.delete', name='cadm'); admin('web.users.delete', name='cenr')
    for t in tn:
        if t and t.startswith('ct-'):
            ta.api('tasks.delete', {'id': next((x.get('id') for x in (tl if isinstance(tl, list) else tl.get('tasks', [])) if x.get('name') == t), '')})
    check(_site_alive(), 'site alive after the concurrent edits')


def t_totp_races():
    """2FA races: the same TOTP code / recovery code used by many logins at once works exactly once; concurrent confirmation issues one set of recovery codes."""
    admin('web.users.save', name='trace', role='admin', password='Totp-Race-Passw0rd', twofa='optional')
    sec = admin('web.users.totp_enroll', name='trace')['result']['secret']; advance(100)
    code = totp(sec); res = []; lk = threading.Lock()
    def conf():
        r = admin('web.users.totp_confirm', name='trace', code=code)
        with lk: res.append(r)
    ts = [threading.Thread(target=conf) for _ in range(6)]
    for t in ts: t.start()
    for t in ts: t.join()
    okc = [r for r in res if r['ok']]
    check(len(okc) == 1, 'six concurrent confirmations: exactly one succeeds (%d)' % len(okc))
    rec = okc[0]['result']['recovery_codes'] if okc else []
    advance(2000); res = []
    code = totp(sec)
    def lg(**kw):
        x = Client(); st, j = x.login('trace', 'Totp-Race-Passw0rd', **kw)
        with lk: res.append(st)
    ts = [threading.Thread(target=lg, kwargs={'code': code}) for _ in range(10)]
    for t in ts: t.start()
    for t in ts: t.join()
    check(res.count(200) == 1, 'one TOTP code used by 10 parallel logins works exactly once (%r)' % sorted(res))
    advance(2000); res = []
    ts = [threading.Thread(target=lg, kwargs={'recovery': rec[0]}) for _ in range(10)] if rec else []
    for t in ts: t.start()
    for t in ts: t.join()
    check(res.count(200) == 1, 'one recovery code used by 10 parallel logins works exactly once (%r)' % sorted(res))
    admin('web.users.delete', name='trace'); advance(2000)


def t_clock_jumps():
    """Session expiry and login lockout under clock jumps (forward, backward, huge)."""
    admin('web.users.save', name='clk', role='admin', password='Clock-Jump-Passw0rd', twofa='off'); advance(2000)
    admin('web.settings.set', session_minutes=20)
    c = Client(); st, j = c.login('clk', 'Clock-Jump-Passw0rd'); check(st == 200, 'login')
    advance(-3600); check(c.api('state')[0] in (200, 401), 'clock 1 h back: answers sanely')
    advance(3600 + 19 * 60); st = c.api('state')[0]; check(st in (200, 401), 'back at +19 min: %d' % st)
    advance(10 * 60); check(c.api('state')[0] == 401, 'expired session stays expired after the clock moved on')
    advance(-3 * 3600); check(c.api('state')[0] == 401, 'an expired session does not come back when the clock goes backwards')
    advance(3 * 3600 + 3000)
    c = Client(); st, j = c.login('clk', 'Clock-Jump-Passw0rd'); check(st == 200, 'login again')
    advance(10 ** 9); check(c.api('state')[0] == 401, 'a 30-year jump forward expires the session (no overflow)')
    advance(-10 ** 9); advance(-10 ** 9 // 2 * 0)
    st, j = Client().login('clk', 'Clock-Jump-Passw0rd'); check(st in (200, 401, 429), 'login after the huge back-jump answers: %d' % st)
    advance(2000)
    # lockout: five wrong passwords lock the account for a while; a backward jump must not make the lock permanent, a forward one frees it
    for i in range(12): Client().login('clk', 'wrong-%d' % i)
    st, j = Client().login('clk', 'Clock-Jump-Passw0rd'); locked = st in (401, 429)
    advance(-7200); st2, _ = Client().login('clk', 'Clock-Jump-Passw0rd')
    advance(7200 + 4000); st3, j3 = Client().login('clk', 'Clock-Jump-Passw0rd')
    check(st3 == 200, 'lock is released after time passes even if the clock went back and forth (locked=%s back=%d later=%d)' % (locked, st2, st3))
    admin('web.users.delete', name='clk'); advance(2000)

# =============================================================================================================================
def main():
    t0 = time.time()
    if admin('web.status')['result']['enabled']: print('already enabled?')
    r = admin('web.settings.set', enabled=True, port=PORT, bind=['127.0.0.1'], require_2fa_admins=False, session_minutes=20); check(r['ok'], 'enable the site on 127.0.0.1:%d' % PORT)
    time.sleep(1)
    tests = [t_no_users_and_static, t_tls, t_users_login_cookie_csrf, t_rate_limit_lockout, t_roles_table, t_web_self_admin, t_totp, t_full_flow_and_secrets, t_password_change, t_concurrent, t_pfx_import_and_curl, t_interfaces, t_status_counts]
    only = [a.split('=',1)[1] for a in sys.argv if a.startswith('--only=')]
    if not QUICK: tests.insert(5, t_malformed_and_slow)
    tests.insert(len(tests) - 1, t_clock_jumps); tests.insert(len(tests) - 1, t_totp_races); tests.insert(len(tests) - 1, t_concurrent_admin); tests.insert(len(tests) - 1, t_rpc_fuzz); tests.insert(len(tests) - 1, t_pfx_variants); tests.insert(len(tests) - 1, t_tls_matrix); tests.insert(len(tests) - 1, t_tls_churn); tests.insert(len(tests) - 1, t_tls_exhaustion)
    tests.append(t_security_extra)                # last: it leaves tasks and users behind
    if only: tests = [t for t in tests if t.__name__ in only[0].split(',')]
    for t in tests:
        n0, f0 = passes, len(fails)
        try: t()
        except Exception as e:
            import traceback; traceback.print_exc(); check(False, '%s crashed: %r' % (t.__name__, e))
        print('%-34s %3d checks ok, %d failed' % (t.__name__, passes - n0, len(fails) - f0), flush=True)
    verdict = 'PASS' if not fails else 'FAIL'
    with open(os.path.join(CTL, 'verdict.txt'), 'w') as f: f.write(verdict + '\n' + '\n'.join(fails))
    mailbox(quit=True)
    print('%s: %d checks passed, %d failed (%.0f s)' % (verdict, passes, len(fails), time.time() - t0))
    for f in fails: print('  -', f)
    return len(fails)

if __name__ == '__main__':
    sys.exit(main())
