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
from pyftpdlib.log import logger as _plog

FAULT = {'kind': None, 'n': 0}
# Раунд 11: активный режим за NAT.  SITE FAULT foreign - сервер принимает в PORT / EPRT адрес, не совпадающий с адресом управляющего соединения
# (pyftpdlib permit_foreign_addresses; за NAT гость называет свой внутренний адрес, а сервер видит адрес NAT).  Включается ТОЛЬКО по команде теста и
# снимается SITE FAULT clear: без неё сервер, как и раньше, отвечает 501 «Rejected data connection to foreign address».
# SITE FAULT portshift N - однократно: сервер подключается не к порту из PORT, а к порту + N (обратное соединение не доходит: 425).
FOREIGN = {'on': False, 'shift': 0}
LAST_REUSE = {'v': None}
STRICT = {'on': os.environ.get('VM2B_FTP_STRICT') == '1', 'slow_ms': 0}      # SITE FAULT strict: a TLS upload that ends without the client's close_notify is answered 451 (no rename); SITE FAULT clear switches it off
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


def _safe(fn):
    try:
        fn()
    except Exception:
        pass


def cut_everything(dtp):
    """Обрыв по команде SITE FAULT: оба соединения (управляющее и данных) получают RST.
    Закрывать каналы ПРЯМО ЗДЕСЬ нельзя: мы внутри обработчика данных, который вызван из кода pyftpdlib (push_dtp_data / handle_read), а FTPHandler.close()
    удаляет атрибут data_channel; код pyftpdlib, продолжающий работу после нашего исключения, обращается к нему и падает с
    AttributeError: 'H' object has no attribute 'data_channel' (в журнале сервера; оборванная передача оставляла канал в неопределённом состоянии,
    и следующий тест на том же сервере иногда зависал: ftp_abort_and_idle_connection на Windows 10 / 11). Поэтому здесь только включается RST при закрытии
    (SO_LINGER 0), а сами каналы закрываются на следующем обороте цикла ввода-вывода, когда pyftpdlib уже вернулся из своих вызовов."""
    ch = dtp.cmd_channel
    for ob in (dtp, ch):
        try:
            sk = ob.socket
            raw = sk._socket if hasattr(sk, '_socket') else sk       # pyOpenSSL Connection wraps the real socket
            rst(raw)
        except Exception:
            pass
    loop = ch.ioloop
    loop.call_later(0, _safe, ch.close)
    loop.call_later(0, _safe, dtp.close)
    raise ConnectionResetError('fault injected')


class FaultMixin:
    _fc = 0

    def recv(self, n):
        d = super().recv(n)
        k = getattr(self, 'cmd', None)
        if d and STRICT['slow_ms'] and k == 'STOR':
            time.sleep(STRICT['slow_ms'] / 1000.0)      # SITE FAULT slowread N: a slow reader (the client's send buffer stays full until its last byte, so its close_notify cannot be written at once)
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
    _shut_n = 0

    def _do_ssl_shutdown(self):
        # pyftpdlib spins forever here after an unexpected EOF ('shutdown while in init' is swallowed and the channel is never closed), the single-threaded server then
        # answers nobody and the client waits out its 60 s for the 226 (round 9).  After a few fruitless rounds the channel is simply closed.
        self._shut_n += 1
        if self._unclean_upload() and STRICT['on'] and not self._closed:
            self._resp = ('451 TLS data connection closed without close_notify.', _plog.info)
        if self._shut_n > 8:
            self._ssl_established = False; self._ssl_closing = False
            return self.close()
        return super()._do_ssl_shutdown()

    def close(self):
        # Раунд 11: канал данных умер, а ответа на команду нет (клиент закрыл соединение посреди TLS-рукопожатия: pyftpdlib закрывает канал, ничего не отвечая на STOR / RETR / LIST;
        # проверено синтетическим клиентом, ClientHello и закрытие).  Клиент тогда ждал бы 226 положенные 60 с и сообщал «receive timed out» - тот же признак, что в сбое раунда 9.
        # Настоящий сервер отвечает 426; сервер отвечает так же и пишет случай в журнал INFO, чтобы такой канал был виден.
        if not self._closed and not self._resp and getattr(self, 'cmd', None) is not None:
            _plog.info('data channel closed with no reply to %s (TLS handshake not finished?): answering 426', getattr(self, 'cmd', '?'))
            self._resp = ('426 Data connection closed before the transfer was done.', _plog.info)
        super().close()

    _noted = False

    def _unclean_upload(self):
        """Раунд 11: True, если это канал ЗАГРУЗКИ, который клиент закрыл БЕЗ TLS close_notify (отмечается один раз, в журнал INFO).
        Раньше проверка стояла только в handle_close, но pyftpdlib при конце загрузки (ZeroReturnError / «Unexpected EOF» в recv) зовёт DTPHandler.handle_close
        МИМО нашего переопределения, а потом сразу _do_ssl_shutdown: проверка молчала, и строгий режим не мог сработать.  Теперь её вызывают оба входа."""
        if self._noted or not getattr(self, 'receive', False):
            return False
        self._noted = True
        try:
            clean = bool(self.socket.get_shutdown() & SSL.RECEIVED_SHUTDOWN)
        except Exception:
            clean = True
        if not clean:
            _plog.info('data connection of an upload closed WITHOUT the client close_notify (%s, strict=%s)', getattr(self, 'cmd', '?'), STRICT['on'])
        return not clean

    def handle_close(self):
        if not self._closed and self._unclean_upload() and STRICT['on']:
            self._resp = ('451 TLS data connection closed without close_notify.', _plog.info)
            self.close()
            return
        super().handle_close()

    def handle_ssl_established(self):
        try:
            LAST_REUSE['v'] = bool(self.socket.session_reused())
        except Exception:
            LAST_REUSE['v'] = None
        super().handle_ssl_established()


def faults(cls):
    cls.data_channel = None            # FTPHandler.close() удаляет атрибут экземпляра: пусть после закрытия чтение даёт None, а не AttributeError
    cls.use_sendfile = False
    cls.auth_failed_timeout = 0
    cls.timeout = OPTS.get('idle', 300)

    def ftp_SITE_FAULT(self, line):
        p = line.split()
        if p[0] == 'clear':
            FAULT['kind'] = None; STRICT['on'] = os.environ.get('VM2B_FTP_STRICT') == '1'; STRICT['slow_ms'] = 0; FOREIGN['on'] = False; FOREIGN['shift'] = 0
        elif p[0] == 'foreign':
            FOREIGN['on'] = True
        elif p[0] == 'portshift':
            FOREIGN['shift'] = int(p[1]) if len(p) > 1 else 1
        elif p[0] == 'strict':
            STRICT['on'] = True
        elif p[0] == 'slowread':
            STRICT['slow_ms'] = int(p[1]) if len(p) > 1 else 15
        else:
            FAULT['kind'] = p[0]; FAULT['n'] = int(p[1]) if len(p) > 1 else 0
        self.respond('200 fault armed: %s' % line)

    def _make_eport(self, ip, port):
        if FOREIGN['on']:
            self.permit_foreign_addresses = True
        if FOREIGN['shift']:
            port = (port + FOREIGN['shift']) % 65536; FOREIGN['shift'] = 0
        return cls.__mro__[1]._make_eport(self, ip, port)

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
    cls._make_eport = _make_eport
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
    logging.basicConfig(level=getattr(logging, os.environ.get('VM2B_FTP_LOGLEVEL', 'WARNING').upper(), logging.WARNING))
    srv = FTPServer((common.BIND, a.port), H)
    srv.max_cons = 0; srv.max_cons_per_ip = 0
    print('ready', flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    main()
