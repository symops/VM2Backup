# A from-scratch FTP server with switchable QUIRKS (and hostile behaviours) for the VM2Backup FTP tests.  pyftpdlib (ftp_server.py) is
# the "well behaved" server; this one imitates what vsftpd / ProFTPD / Pure-FTPd / IIS / FileZilla / old embedded servers do differently.
#   ftp_quirk.py --root DIR --user U --password P --certdir D --spec NAME:PORT:MODE:quirk,quirk=val,...  [--spec ...]
#   MODE = plain | ftpes | ftps.   One listener per --spec, all in one process.  Quirks (comma separated):
#     nofeat nomlsd nosize nomdtm nonlst          the command answers 500/502 (nofeat: FEAT itself is unknown)
#     list=unix|dos|eplf|none                      LIST format  (none: 502)
#     nlst=names|paths|dotpaths|empty450           NLST flavour: bare names (default) / "dir/name" / "./name" / 450 "No files found"
#     epsv=refuse|garbage|hang  pasv=zero|nat|nat172|garbage    EPSV refused / 229 without a port / never answered; PASV announces 0.0.0.0 / 192.168.. / 172.16.. / junk
#     banner=multi|120|421|554|spaces              welcome banner variants (multi-line 220-, a 120 "wait" first, 421 busy + close, 554 refused)
#     login=nopass|acct|busy421|denied             USER answered 230 / PASS answered 332 / 421 / 530
#     utf8off                                      FEAT without UTF8 (names are bytes either way)
#     rnto_noover                                  RNTO onto an existing file is refused 550 (the client must DELE first)
#     stor_noover                                  STOR onto an existing file is refused 553
#     slow=BYTES_PER_SEC                           data connection throttled
#     idle=SEC                                     "421 Timeout" and close after SEC seconds without a command
#     prot=refuse|c_only                           PROT P answered 534 / PROT P not accepted but PROT C is
#     pbsz=order                                   PROT before PBSZ gives 503 (strict servers); PBSZ with a non-zero argument answered 200 PBSZ=0
#     reuse                                        TLS data connections must resume the control session (vsftpd require_ssl_reuse), else 425
#     nonotify                                     no TLS close_notify on the data channel: the socket is just closed
#     bigdir=N                                     folder "big" with N files
#     host=NAME  (info only)
#   hostile modes (the control connection only; MODE is ignored):  hostile=garbage|hugeline|endless|lines100k|nocrlf|dribble|silent|lines5k|replyloop|bannerthenclose
import select, argparse, os, random, socket, ssl, struct, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_common as common


def parse_spec(s):
    name, port, mode, q = (s.split(':', 3) + [''])[:4]
    qd = {}
    for item in [x for x in q.split(',') if x]:
        if '=' in item:
            k, v = item.split('=', 1)
        else:
            k, v = item, True
        qd[k] = v
    return name, int(port), mode, qd


class Quit(Exception):
    pass


def u8(s):
    return s.encode('latin-1').decode('utf-8', 'replace')


def esc(b):
    return b.decode('latin-1')


class Session(threading.Thread):
    def __init__(self, sock, mode, q, args, ctx):
        super().__init__(daemon=True)
        self.c = sock; self.mode = mode; self.q = q; self.args = args; self.ctx = ctx
        self.cwd = []; self.user = None; self.authed = False; self.rnfr = None; self.pasv = None; self.port = None
        self.prot = False; self.tls = False; self.rest = 0; self.buf = b''; self.pbsz = False
        self.session = None

    # ---- io
    def send(self, text):
        self.c.sendall(text.encode('latin-1') if isinstance(text, str) else text)

    def reply(self, code, text='ok'):
        self.send('%d %s\r\n' % (code, text))

    def line(self):
        idle = float(self.q.get('idle', 600))
        self.c.settimeout(idle)
        while b'\n' not in self.buf:
            try:
                d = self.c.recv(4096)
            except socket.timeout:
                if 'idle' in self.q:
                    try: self.send('421 Timeout.\r\n')
                    except Exception: pass
                raise Quit()
            if not d:
                raise Quit()
            self.buf += d
        l, self.buf = self.buf.split(b'\n', 1)
        return l.rstrip(b'\r')

    # ---- paths
    def real(self, name):
        parts = list(self.cwd)
        n = name.replace('\\', '/') if False else name
        if n.startswith('/'):
            parts = []
        for p in n.split('/'):
            if p in ('', '.'):
                continue
            if p == '..':
                if parts: parts.pop()
                continue
            parts.append(p)
        base = os.path.realpath(self.args.root)
        full = os.path.realpath(os.path.join(base, *[x.encode('latin-1').decode('utf-8', 'surrogateescape') for x in parts]))
        if not (full == base or full.startswith(base + os.sep)):
            return None
        return full

    # ---- main
    def run(self):
        try:
            self.main()
        except Quit:
            pass
        except Exception:
            pass
        finally:
            try: self.c.close()
            except Exception: pass
            self.close_pasv()

    def hostile(self, h):
        c = self.c
        if h == 'garbage':
            c.sendall(os.urandom(5000)); time.sleep(0.5)
        elif h == 'hugeline':
            c.sendall(b'220 ' + b'A' * (8 << 20)); time.sleep(1)
        elif h == 'endless':
            end = time.time() + 60
            while time.time() < end:
                c.sendall(b'220-' + b'x' * 70 + b'\r\n')
        elif h == 'lines100k':
            c.sendall(b''.join(b'220-line %d\r\n' % i for i in range(100000)) + b'220 done\r\n')
            self.after_banner()
        elif h == 'lines5k':
            c.sendall(b''.join(b'220-line %d\r\n' % i for i in range(4000)) + b'220 done\r\n')
            self.after_banner()
        elif h == 'nocrlf':
            c.sendall(b'220 hello'); time.sleep(40)
        elif h == 'dribble':
            for ch in b'220 ' + b'z' * 100000:
                c.sendall(bytes([ch])); time.sleep(0.3)
        elif h == 'silent':
            time.sleep(60)
        elif h == 'replyloop':        # a valid banner, then every command is answered with an endless stream of 1xx replies
            c.sendall(b'220 ok\r\n')
            try:
                while True:
                    self.line(); c.sendall(b'150 more\r\n' * 1000)
            except Exception: pass
        elif h == 'bannerthenclose':
            c.sendall(b'220 hi\r\n'); time.sleep(0.2)

    def after_banner(self):
        try:
            self.main_loop()
        except Exception:
            pass

    def main(self):
        h = self.q.get('hostile')
        if h:
            return self.hostile(h)
        b = self.q.get('banner')
        if self.mode == 'ftps':
            self.c = self.ctx.wrap_socket(self.c, server_side=True); self.tls = True
        if b == '421':
            self.send('421 Too many users, closing.\r\n'); return
        if b == '554':
            self.send('554 Connections from your network are not allowed.\r\n'); return
        if b == '120':
            self.send('120 Service ready in 1 minutes.\r\n')
        if b == 'multi':
            self.send('220-Welcome to the quirk server\r\n   (second line)\r\n220-\r\n 220 looks like a reply but is not\r\n220 Ready.\r\n')
        elif b == 'spaces':
            self.send('220   Ready   \r\n')
        else:
            self.send('220 Quirk FTP ready.\r\n')
        self.main_loop()

    def main_loop(self):
        while True:
            raw = self.line()
            if not raw:
                continue
            cmd, _, arg = esc(raw).partition(' ')
            cmd = cmd.upper()
            fn = getattr(self, 'c_' + cmd, None)
            if fn is None:
                self.reply(500, "'%s' not understood" % cmd); continue
            if cmd not in ('USER', 'PASS', 'QUIT', 'AUTH', 'PBSZ', 'PROT', 'FEAT', 'OPTS', 'NOOP', 'SYST') and not self.authed:
                self.reply(530, 'Please login with USER and PASS.'); continue
            fn(arg)

    # ---- commands
    def c_USER(self, a):
        self.user = a
        l = self.q.get('login')
        if l == 'busy421': self.send('421 Server busy.\r\n'); raise Quit()
        if l == 'denied' : self.reply(530, 'Login incorrect.'); return
        if l == 'nopass' and u8(a) == self.args.user: self.authed = True; self.reply(230, 'Logged in without password.'); return
        self.reply(331, 'Please specify the password.')

    def c_PASS(self, a):
        l = self.q.get('login')
        if l == 'acct': self.reply(332, 'Need account for login.'); return
        if self.user == self.args.user and u8(a) == self.args.password:
            self.authed = True; self.reply(230, 'Login successful.')
        else:
            self.reply(530, 'Login incorrect.')

    def c_AUTH(self, a):
        if self.mode != 'ftpes' or a.upper() not in ('TLS', 'TLS-C', 'SSL'):
            self.reply(504, 'Unknown AUTH type.'); return
        self.reply(234, 'Proceed with negotiation.')
        self.c = self.ctx.wrap_socket(self.c, server_side=True); self.tls = True
        self.c.settimeout(30)
        try: self.session = self.c.session
        except Exception: self.session = None
        self.buf = b''

    def c_PBSZ(self, a):
        if not self.tls: self.reply(503, 'PBSZ needs a protected control connection.'); return
        self.pbsz = True
        self.reply(200, 'PBSZ=0')

    def c_PROT(self, a):
        if not self.tls: self.reply(503, 'PROT needs a protected control connection.'); return
        if self.q.get('pbsz') == 'order' and not self.pbsz: self.reply(503, 'PBSZ missing.'); return
        p = self.q.get('prot')
        if a.upper() == 'P':
            if p in ('refuse', 'c_only'): self.reply(534, 'Request denied for policy reasons.'); return
            self.prot = True
        elif a.upper() == 'C':
            self.prot = False
        else:
            self.reply(504, 'PROT not supported.'); return
        self.reply(200, 'PROT now %s' % a.upper())

    def c_FEAT(self, a):
        if 'nofeat' in self.q: self.reply(500, "'FEAT' not understood"); return
        f = ['PASV', 'EPSV', 'REST STREAM', 'TVFS']
        if 'nomlsd' not in self.q: f.append('MLST type*;size*;modify*;')
        if 'nosize' not in self.q: f.append('SIZE')
        if 'nomdtm' not in self.q: f.append('MDTM')
        if 'utf8off' not in self.q: f.append('UTF8')
        if self.mode != 'plain': f += ['AUTH TLS', 'PBSZ', 'PROT']
        self.send('211-Features:\r\n' + ''.join(' %s\r\n' % x for x in f) + '211 End\r\n')

    def c_OPTS(self, a):
        if a.upper().startswith('UTF8') and 'utf8off' not in self.q: self.reply(200, 'Always in UTF8 mode.')
        else: self.reply(501, 'Option not understood.')

    def c_SYST(self, a): self.reply(215, 'UNIX Type: L8')
    def c_NOOP(self, a): self.reply(200, 'NOOP ok.')
    def c_QUIT(self, a): self.reply(221, 'Goodbye.'); raise Quit()
    def c_TYPE(self, a):
        if a.upper() in ('I', 'A', 'A N', 'L 8'): self.reply(200, 'Switching to Binary mode.')
        else: self.reply(504, 'Unknown TYPE.')
    def c_PWD(self, a): self.reply(257, '"/%s" is the current directory' % '/'.join(self.cwd))
    def c_REST(self, a): self.reply(502, 'REST not supported.') if 'norest' in self.q else (setattr(self, 'rest', int(a)), self.reply(350, 'Restarting at %s.' % a))

    def c_CWD(self, a):
        if a == '/': self.cwd = []; self.reply(250, 'Directory successfully changed.'); return
        p = self.real(a)
        if p is None or not os.path.isdir(p): self.reply(550, 'Failed to change directory.'); return
        parts = list(self.cwd) if not a.startswith('/') else []
        for x in a.split('/'):
            if x in ('', '.'): continue
            if x == '..':
                if parts: parts.pop()
            else: parts.append(x)
        self.cwd = parts
        self.reply(250, 'Directory successfully changed.')

    def c_CDUP(self, a): self.c_CWD('..')

    def c_MKD(self, a):
        p = self.real(a)
        if p is None: self.reply(550, 'Create directory operation failed.'); return
        try: os.mkdir(p); self.reply(257, '"%s" created' % a)
        except OSError: self.reply(550, 'Create directory operation failed.')

    def c_DELE(self, a):
        p = self.real(a)
        try:
            if p is None or not os.path.isfile(p): raise OSError()
            os.unlink(p); self.reply(250, 'Delete operation successful.')
        except OSError: self.reply(550, 'Delete operation failed.')

    def c_SIZE(self, a):
        if 'nosize' in self.q: self.reply(500, "'SIZE' not understood"); return
        p = self.real(a)
        if p is None or not os.path.isfile(p): self.reply(550, 'Could not get file size.'); return
        sz = os.path.getsize(p); lie = self.q.get('sizelie')
        if lie == 'long': sz = sz // 2          # announces less than it sends
        elif lie == 'short': sz = sz * 2 + 10   # announces more than it sends
        self.reply(213, str(sz))

    def c_MDTM(self, a):
        if 'nomdtm' in self.q: self.reply(500, "'MDTM' not understood"); return
        p = self.real(a)
        if p is None or not os.path.isfile(p): self.reply(550, 'Could not get file modification time.'); return
        self.reply(213, time.strftime('%Y%m%d%H%M%S', time.gmtime(os.path.getmtime(p))))

    def c_RNFR(self, a):
        p = self.real(a)
        if p is None or not os.path.exists(p): self.reply(550, 'RNFR command failed.'); return
        self.rnfr = p; self.reply(350, 'Ready for RNTO.')

    def c_RNTO(self, a):
        if not self.rnfr: self.reply(503, 'RNFR required first.'); return
        p = self.real(a); src = self.rnfr; self.rnfr = None
        if p is None: self.reply(550, 'Rename failed.'); return
        if 'rnto_noover' in self.q and os.path.exists(p): self.reply(550, 'Rename failed: file exists.'); return
        try: os.replace(src, p); self.reply(250, 'Rename successful.')
        except OSError: self.reply(550, 'Rename failed.')

    # ---- data connections
    def close_pasv(self):
        if self.pasv:
            try: self.pasv.close()
            except Exception: pass
            self.pasv = None

    def c_PASV(self, a):
        self.close_pasv(); self.port = None
        s = socket.socket(); s.bind((common.BIND, 0)); s.listen(1); self.pasv = s
        pt = s.getsockname()[1]
        k = self.q.get('pasv')
        if k == 'garbage': self.reply(227, 'Entering Passive Mode (what,is,this)'); return
        ip = {'zero': '0,0,0,0', 'nat': '192,168,1,5', 'nat172': '172,16,9,9', 'public': '203,0,113,7'}.get(k, common.BIND.replace('.', ','))
        if k == 'dead': pt = 1
        self.reply(227, 'Entering Passive Mode (%s,%d,%d).' % (ip, pt >> 8, pt & 255))

    def c_EPSV(self, a):
        e = self.q.get('epsv')
        if e == 'refuse': self.reply(502, 'EPSV not supported.'); return
        if e == 'hang': time.sleep(300); return
        self.close_pasv(); self.port = None
        s = socket.socket(); s.bind((common.BIND, 0)); s.listen(1); self.pasv = s
        if e == 'garbage': self.reply(229, 'Entering Extended Passive Mode'); return
        self.reply(229, 'Entering Extended Passive Mode (|||%d|)' % s.getsockname()[1])

    def c_PORT(self, a):
        n = [int(x) for x in a.split(',')]
        self.close_pasv(); self.port = ('.'.join(str(x) for x in n[:4]), n[4] * 256 + n[5]); self.reply(200, 'PORT command successful.')

    def c_EPRT(self, a):
        d = a[0]; p = a.split(d)
        self.close_pasv(); self.port = (p[2], int(p[3])); self.reply(200, 'EPRT command successful.')

    def open_data(self, msg):
        """accepts the data connection, sends the 150 reply, then (PROT P) runs the TLS handshake; None = failed (reply sent)"""
        try:
            if self.pasv:
                self.pasv.settimeout(15); d, _ = self.pasv.accept(); self.close_pasv()
            elif self.port:
                d = socket.create_connection(self.port, 15); self.port = None
            else:
                self.reply(425, 'Use PORT or PASV first.'); return None
        except Exception:
            self.reply(425, 'Failed to establish connection.'); return None
        self.reply(150, msg)
        if self.prot:
            try:
                d.settimeout(30)
                d = self.ctx.wrap_socket(d, server_side=True)
            except Exception:
                self.reply(425, 'TLS handshake failed.'); return None
            if 'reuse' in self.q:
                try: ok = bool(d.session_reused)
                except Exception: ok = False
                if not ok:
                    try: d.close()
                    except Exception: pass
                    self.reply(522, 'SSL connection failed; session reuse required'); return None
        d.settimeout(60)
        return d

    def end_data(self, d):
        if self.prot and 'nonotify' not in self.q:
            try:
                d.settimeout(2); d = d.unwrap()
            except Exception:
                pass
        try: d.close()
        except Exception: pass

    def push(self, d, data):
        rate = int(self.q.get('slow', 0)) if self.q.get('slow') else 0
        if not rate:
            d.sendall(data); return
        step = max(1, rate // 20)
        for i in range(0, len(data), step):
            d.sendall(data[i:i + step]); time.sleep(0.05)

    def entries(self, p):
        names = []
        try: names = sorted(os.listdir(p))
        except OSError: pass
        out = []
        for n in names:
            fp = os.path.join(p, n)
            try: st = os.lstat(fp)
            except OSError: continue
            out.append((os.fsencode(n).decode('latin-1'), st))
        return out

    def c_LIST(self, a):
        k = self.q.get('list', 'unix')
        if k == 'none': self.reply(502, 'LIST not implemented.'); return
        if a.startswith('-'): a = ''
        p = self.real(a)
        if p is None or not os.path.isdir(p): self.reply(550, 'No such directory.'); return
        d = self.open_data('Here comes the directory listing.')
        if d is None: return
        lines = []
        for n, st in self.entries(p):
            isdir = (st.st_mode & 0o170000) == 0o040000
            t = time.gmtime(st.st_mtime)
            if k == 'unix':
                lines.append('%s 1 ftp ftp %12d %s %2d %s %s' % ('drwxr-xr-x' if isdir else '-rw-r--r--', st.st_size, time.strftime('%b', t), t.tm_mday, time.strftime('%H:%M', t) if (time.time() - st.st_mtime) < 15000000 else time.strftime(' %Y', t), n))
            elif k == 'dos':
                lines.append('%s  %s %s %s' % (time.strftime('%m-%d-%y  %I:%M%p', t), '      <DIR>   ' if isdir else '%14d' % st.st_size, '' if isdir else '', n))
            elif k == 'eplf':
                lines.append('+i8388621.29609,m%d,%s,\t%s' % (int(st.st_mtime), '/' if isdir else 'r,s%d' % st.st_size, n))
        try:
            pre = 'total %d\r\n' % len(lines) if k == 'unix' else ''
            self.push(d, (pre + ''.join(l + '\r\n' for l in lines)).encode('latin-1'))
            self.end_data(d); self.reply(226, 'Directory send OK.')
        except Exception:
            try: d.close()
            except Exception: pass

    def c_NLST(self, a):
        if 'nonlst' in self.q: self.reply(502, 'NLST not implemented.'); return
        k = self.q.get('nlst', 'names')
        if a.startswith('-'): a = ''
        p = self.real(a)
        if p is None or not os.path.isdir(p): self.reply(550, 'No such directory.'); return
        if 'listbomb' in self.q:
            d = self.open_data('Opening data connection for file list.')
            if d is None: return
            try:
                blk = (b'x' * 99 + b'\r\n') * 10000
                for _ in range(2000): d.sendall(blk)       # ~2 GB of "names" unless the client gives up
            except Exception: pass
            try: d.close()
            except Exception: pass
            return
        ents = [n for n, st in self.entries(p) if (st.st_mode & 0o170000) != 0o040000 or True]
        if k == 'empty450' and not ents: self.reply(450, 'No files found'); return
        d = self.open_data('Opening data connection for file list.')
        if d is None: return
        pre = {'paths': '/'.join(self.cwd) + '/', 'dotpaths': './'}.get(k, '')
        try:
            self.push(d, ''.join(pre + n + '\r\n' for n in ents).encode('latin-1'))
            self.end_data(d); self.reply(226, 'Transfer complete.')
        except Exception:
            try: d.close()
            except Exception: pass

    def c_MLSD(self, a):
        if 'nomlsd' in self.q: self.reply(500, "'MLSD' not understood"); return
        p = self.real(a)
        if p is None or not os.path.isdir(p): self.reply(550, 'No such directory.'); return
        d = self.open_data('Opening data connection.')
        if d is None: return
        out = ''
        for n, st in self.entries(p):
            isdir = (st.st_mode & 0o170000) == 0o040000
            out += 'Type=%s;Size=%d;Modify=%s; %s\r\n' % ('dir' if isdir else 'file', st.st_size, time.strftime('%Y%m%d%H%M%S', time.gmtime(st.st_mtime)), n)
        try:
            self.push(d, out.encode('latin-1')); self.end_data(d); self.reply(226, 'MLSD complete.')
        except Exception:
            try: d.close()
            except Exception: pass

    def c_RETR(self, a):
        p = self.real(a)
        if p is None or not os.path.isfile(p): self.reply(550, 'Failed to open file.'); return
        d = self.open_data('Opening BINARY mode data connection for %s (%d bytes).' % (a, os.path.getsize(p)))
        if d is None: return
        try:
            with open(p, 'rb') as f:
                f.seek(self.rest); self.rest = 0
                while True:
                    b = f.read(65536)
                    if not b: break
                    self.push(d, b)
            self.end_data(d); self.reply(226, 'Transfer complete.')
        except Exception:
            try: d.close()
            except Exception: pass

    def c_STOR(self, a):
        p = self.real(a)
        if p is None: self.reply(553, 'Could not create file.'); return
        if 'stor_noover' in self.q and os.path.exists(p): self.reply(553, 'Could not create file: exists.'); return
        d = self.open_data('Ok to send data.')
        if d is None: return
        try:
            with open(p, 'wb') as f:
                while True:
                    if self.q.get('slowread'): time.sleep(int(self.q['slowread']) / 1000.0)      # a server slower than the client: RST at the client's close is noticed (pyftpdlib takes it for the end of the file)
                    if 'hupend' in self.q:                                                  # pyftpdlib: a HUP / error flag on the data socket ends the upload at once, unread bytes are dropped, the reply is 226
                        pl = select.poll(); pl.register(d.fileno(), select.POLLHUP | select.POLLERR)
                        if pl.poll(0): break
                    try: b = d.recv(65536)
                    except ssl.SSLError as e:
                        if 'EOF' in str(e): b = b''
                        else: raise
                    if not b: break
                    f.write(b)
            d.close(); self.reply(226, 'Transfer complete.')
        except Exception:
            try: d.close()
            except Exception: pass
            self.reply(426, 'Failure writing network stream.')


def listener(name, port, mode, q, args, certdir):
    ctx = None
    if mode in ('ftpes', 'ftps'):
        ctx = common.tls_context(os.path.join(certdir, q.get('cert', 'server') + '.pem') if isinstance(q.get('cert'), str) else os.path.join(certdir, 'server.pem'),
                                 os.path.join(certdir, (q['cert'] if isinstance(q.get('cert'), str) else 'server') + '.key'))
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((common.BIND, port)); s.listen(64)
    while True:
        c, _ = s.accept()
        Session(c, mode, q, args, ctx).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True); ap.add_argument('--user', default='u'); ap.add_argument('--password', default='p')
    ap.add_argument('--certdir', required=True); ap.add_argument('--spec', action='append', default=[])
    a = ap.parse_args()
    os.makedirs(a.root, exist_ok=True)
    for sp in a.spec:
        name, port, mode, q = parse_spec(sp)
        r = os.path.join(a.root, name)
        os.makedirs(r, exist_ok=True)
        if 'bigdir' in q:
            bd = os.path.join(r, 'big'); os.makedirs(bd, exist_ok=True)
            for i in range(int(q['bigdir'])):
                open(os.path.join(bd, 'volume-%06d.bin' % i), 'wb').close()
        args = argparse.Namespace(root=r, user=a.user, password=a.password)
        threading.Thread(target=listener, args=(name, port, mode, q, args, a.certdir), daemon=True).start()
    print('ready', flush=True)
    threading.Event().wait()


if __name__ == '__main__':
    main()
