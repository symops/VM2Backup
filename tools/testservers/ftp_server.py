# FTP / FTPES / FTPS test server for VM2Backup (pyftpdlib) with FAULT INJECTION.
#   ftp_server.py --port P --mode plain|ftpes|ftps --root DIR --user U --password PW --certdir D [--cert server]
#                 [--pasv A-B] [--masquerade IP] [--no-epsv] [--list nlst|mlsd|list] [--idle SEC] [--tls-max 1.2]
# Faults are armed in-band by any logged-in session:  SITE FAULT <kind> [n]  (one shot, applies to the NEXT matching transfer):
#   up n        the control and data connections are reset (RST) once n bytes of an upload (STOR) have arrived
#   down n      ... after n bytes of a download (RETR) were sent
#   list n      ... after n bytes of a directory listing were sent
#   stall ms    the server stops serving for ms milliseconds in the middle of the next STOR (client timeout tests)
#   lost226     after the next STOR completed, the control connection is reset before the client sees the 226
#   norename    the next RNTO is refused with 550
#   stor421     the next STOR is answered 421 and the control connection closed
#   stor552     the next STOR is answered 552 (storage exceeded)
#   clear       disarms everything.      SITE REUSE   answers whether the last TLS data connection resumed the control session.
import os, socket, struct, sys, threading, time, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_common as common
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler, DTPHandler, TLS_DTPHandler
from pyftpdlib.servers import FTPServer
from OpenSSL import SSL

FAULT = {'kind': None, 'n': 0}
LAST_REUSE = {'v': None}
OPTS = {}


def rst(sock):
    try:
        s = sock
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        except Exception:
            pass
    except Exception:
        pass


def cut_everything(dtp):
    ch = dtp.cmd_channel
    for ob in (dtp, ch):
        try:
            sk = ob.socket
            raw = sk._socket if hasattr(sk, '_socket') else sk       # pyOpenSSL Connection wraps the real socket
            rst(raw)
        except Exception:
            pass
    try:
        ch.close()
    except Exception:
        pass
    try:
        dtp.close()
    except Exception:
        pass
    raise ConnectionResetError('fault injected')


class FaultMixin:
    _fc = 0

    def recv(self, n):
        d = super().recv(n)
        k = getattr(self, 'cmd', None)
        if d and FAULT['kind'] in ('up', 'stall') and k == 'STOR':
            self._fc += len(d)
            if FAULT['kind'] == 'up' and self._fc >= FAULT['n']:
                FAULT['kind'] = None; cut_everything(self)
            if FAULT['kind'] == 'stall':
                ms = FAULT['n']; FAULT['kind'] = None; time.sleep(ms / 1000.0)
        return d

    def send(self, data):
        # (self.cmd is set only after the first push, so the kind of transfer is read from the channel itself)
        kind = FAULT['kind']
        retr = getattr(self, 'file_obj', None) is not None
        k = 'RETR' if retr else 'LIST'
        if data and ((kind == 'down' and retr) or (kind == 'list' and not retr)):
            left = FAULT['n'] - self._fc
            if left <= 0:
                FAULT['kind'] = None; cut_everything(self)
            if len(data) > left:
                data = data[:left]
        r = super().send(data)
        if r:
            self._fc += r
            if FAULT['kind'] == kind and kind in ('down', 'list') and ((kind == 'down' and retr) or (kind == 'list' and not retr)) and self._fc >= FAULT['n']:
                FAULT['kind'] = None; cut_everything(self)
        return r


class PlainDTP(FaultMixin, DTPHandler):
    pass


class TlsDTP(FaultMixin, TLS_DTPHandler):
    def handle_ssl_established(self):
        try:
            LAST_REUSE['v'] = bool(self.socket.session_reused())
        except Exception:
            LAST_REUSE['v'] = None
        super().handle_ssl_established()


def faults(cls):
    cls.use_sendfile = False
    cls.auth_failed_timeout = 0
    cls.timeout = OPTS.get('idle', 300)

    def ftp_SITE_FAULT(self, line):
        p = line.split()
        if p[0] == 'clear':
            FAULT['kind'] = None
        else:
            FAULT['kind'] = p[0]; FAULT['n'] = int(p[1]) if len(p) > 1 else 0
        self.respond('200 fault armed: %s' % line)

    def ftp_SITE_REUSE(self, line):
        self.respond('200 reused=%s' % ('unknown' if LAST_REUSE['v'] is None else int(LAST_REUSE['v'])))

    def ftp_STOR(self, file, mode='w'):
        k = FAULT['kind']
        if k == 'stor421':
            FAULT['kind'] = None; self.respond('421 Service not available, closing control connection.'); self.close_when_done(); return
        if k == 'stor552':
            FAULT['kind'] = None; self.respond('552 Exceeded storage allocation.'); return
        return cls.__mro__[1].ftp_STOR(self, file, mode)

    def ftp_RNTO(self, path):
        if FAULT['kind'] == 'norename':
            FAULT['kind'] = None; self.respond('550 Cannot rename (fault injected).'); return
        return cls.__mro__[1].ftp_RNTO(self, path)

    def on_file_received(self, file):
        if FAULT['kind'] == 'lost226':
            FAULT['kind'] = None
            try:
                sk = self.socket; rst(sk._socket if hasattr(sk, '_socket') else sk)
            except Exception:
                pass
            try:
                self.close()
            except Exception:
                pass

    def ftp_EPSV(self, line):
        if OPTS.get('no_epsv'):
            self.respond('500 EPSV not understood.'); return
        return cls.__mro__[1].ftp_EPSV(self, line)

    def ftp_NLST(self, path):
        if OPTS.get('list') in ('mlsd', 'list'):
            self.respond('550 No files found.' if OPTS['list'] == 'mlsd' else '502 NLST not implemented.'); return
        return cls.__mro__[1].ftp_NLST(self, path)

    def ftp_MLSD(self, path):
        if OPTS.get('list') in ('nlst', 'list'):
            self.respond('502 MLSD not implemented.'); return
        return cls.__mro__[1].ftp_MLSD(self, path)

    def ftp_FEAT(self, line):
        r = cls.__mro__[1].ftp_FEAT
        if OPTS.get('list') in ('nlst', 'list'):
            # hide MLSD from the feature list
            old = self._extra_feats
        return r(self, line)

    cls.ftp_SITE_FAULT = ftp_SITE_FAULT
    cls.ftp_SITE_REUSE = ftp_SITE_REUSE
    cls.ftp_STOR = ftp_STOR
    cls.ftp_RNTO = ftp_RNTO
    cls.on_file_received = on_file_received
    cls.ftp_EPSV = ftp_EPSV
    cls.ftp_NLST = ftp_NLST
    cls.ftp_MLSD = ftp_MLSD
    cls.proto_cmds = dict(cls.proto_cmds)
    cls.proto_cmds['SITE FAULT'] = dict(perm=None, auth=True, arg=True, help='SITE FAULT kind [n]')
    cls.proto_cmds['SITE REUSE'] = dict(perm=None, auth=True, arg=False, help='SITE REUSE')
    return cls


class PlainHandler(FTPHandler):
    dtp_handler = PlainDTP


class ExplicitHandler(TLS_FTPHandler):
    dtp_handler = TlsDTP
    tls_control_required = False
    tls_data_required = False


class ImplicitHandler(TLS_FTPHandler):
    dtp_handler = TlsDTP

    def handle(self):
        self.secure_connection(self.ssl_context)
        super().handle()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--mode', default='plain')
    ap.add_argument('--root', required=True)
    ap.add_argument('--user', default='u')
    ap.add_argument('--password', default='p')
    ap.add_argument('--certdir', required=True)
    ap.add_argument('--cert', default='server')
    ap.add_argument('--pasv', default='')
    ap.add_argument('--masquerade', default='')
    ap.add_argument('--no-epsv', action='store_true')
    ap.add_argument('--list', default='')
    ap.add_argument('--idle', type=int, default=300)
    ap.add_argument('--tls-max', default='')
    a = ap.parse_args()
    OPTS.update(no_epsv=a.no_epsv, list=a.list, idle=a.idle)
    os.makedirs(a.root, exist_ok=True)
    auth = DummyAuthorizer()
    auth.add_user(a.user, a.password, a.root, perm='elradfmwMT')
    base = {'plain': PlainHandler, 'ftpes': ExplicitHandler, 'ftps': ImplicitHandler}[a.mode]
    H = faults(type('H', (base,), {}))
    H.authorizer = auth
    if a.pasv:
        lo, hi = a.pasv.split('-'); H.passive_ports = range(int(lo), int(hi) + 1)
    if a.masquerade:
        H.masquerade_address = a.masquerade
    if a.mode != 'plain':
        H.certfile = os.path.join(a.certdir, a.cert + '.pem'); H.keyfile = os.path.join(a.certdir, a.cert + '.key')
        H.tls_control_required = False
    H.banner = 'VM2B test FTP ready.'
    import logging
    logging.basicConfig(level=logging.WARNING)
    srv = FTPServer((common.BIND, a.port), H)
    srv.max_cons = 0; srv.max_cons_per_ip = 0
    print('ready', flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
