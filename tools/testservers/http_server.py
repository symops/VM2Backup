# HTTP/1.1 test server for the VM2Backup HTTP client.  usage: http_server.py <certdir> <port>[:tls[:cert]] ...
# Endpoints (all deterministic, see common.pattern):
#   GET /len/<n>               Content-Length body of n pattern bytes
#   GET /chunked/<n>?c=<k>     chunked body, chunks of k bytes
#   GET /close/<n>             n bytes, no length, connection closed afterwards (HTTP/1.1 Connection: close)
#   GET /http10/<n>            HTTP/1.0 style answer: no length, closes
#   GET /head-me               HEAD answers carry a Content-Length but no body
#   PUT|POST /put/<name>       reads the body (Content-Length or chunked), answers "<size> <crc32>" (201); Expect: 100-continue works
#   POST /echo                 answers with the body it got
#   PUT /reject                answers 401 at once WITHOUT reading the body (then closes)
#   GET /cookie/set            sets vmware_soap_session="abc123" and other=1
#   GET /cookie/del            deletes other
#   GET /cookie/show           answers with the Cookie header it got
#   GET /auth                  200 only with Basic user:pass, else 401
#   GET /redir                 302 Location: /len/5
#   GET /status/<code>         that status with a short body
#   GET /sneaky                answers keep-alive but closes the socket right after (the next request on it must be re-sent)
#   GET /cut/<n>               announces Content-Length 5000000 but sends only n bytes, then drops the connection
#   GET /cutchunk/<n>          chunked answer that stops after n bytes without the last chunk
#   GET /slow                  never answers (timeouts)
#   GET /count                 how many connections / requests this server has seen so far: "<conns> <reqs>"
#   GET /tlsinfo               answers with the negotiated TLS version
import os, socket, sys, threading, time, zlib, base64, struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_common as common

STATS = {'conns': 0, 'reqs': 0}
LOCK = threading.Lock()


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'VM2BTest/1'

    def log_message(self, *a):
        pass

    def setup(self):
        super().setup()
        with LOCK:
            STATS['conns'] += 1

    def _send(self, code, body=b'', headers=(), close=False):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        if close:
            self.send_header('Connection', 'close'); self.close_connection = True
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def _stream(self, n):
        off = 0
        while off < n:
            k = min(1 << 20, n - off)
            self.wfile.write(common.pattern(off, k)); off += k

    def _read_body(self):
        te = self.headers.get('Transfer-Encoding', '')
        crc = 0; size = 0
        if 'chunked' in te.lower():
            while True:
                line = self.rfile.readline().strip()
                n = int(line.split(b';')[0], 16)
                if n == 0:
                    while self.rfile.readline().strip():
                        pass
                    break
                left = n
                while left:
                    d = self.rfile.read(min(left, 1 << 20))
                    if not d: raise EOFError
                    crc = zlib.crc32(d, crc); size += len(d); left -= len(d)
                self.rfile.readline()
        else:
            left = int(self.headers.get('Content-Length', '0'))
            body = []
            while left:
                d = self.rfile.read(min(left, 1 << 20))
                if not d: raise EOFError
                crc = zlib.crc32(d, crc); size += len(d); left -= len(d)
                if size < (1 << 22): body.append(d)
            self._last_body = b''.join(body)
            return size, crc
        self._last_body = b''
        return size, crc

    def handle_one_request(self):
        super().handle_one_request()
        with LOCK:
            STATS['reqs'] += 1

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        p = self.path.split('?')[0]; q = self.path.split('?')[1] if '?' in self.path else ''
        parts = p.strip('/').split('/')
        if parts[0] == 'len':
            n = int(parts[1])
            self.send_response(200); self.send_header('Content-Length', str(n)); self.end_headers()
            if self.command != 'HEAD': self._stream(n)
        elif parts[0] == 'chunked':
            n = int(parts[1]); ck = 65536
            for kv in q.split('&'):
                if kv.startswith('c='): ck = int(kv[2:])
            self.send_response(200); self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            off = 0
            while off < n:
                k = min(ck, n - off)
                self.wfile.write(b'%x\r\n' % k + common.pattern(off, k) + b'\r\n'); off += k
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        elif parts[0] == 'close':
            n = int(parts[1])
            self.send_response(200); self.send_header('Connection', 'close'); self.end_headers(); self.close_connection = True
            self._stream(n)
        elif parts[0] == 'http10':
            n = int(parts[1])
            self.wfile.write(b'HTTP/1.0 200 OK\r\n\r\n'); self._stream(n); self.close_connection = True
        elif parts[0] == 'head-me':
            self.send_response(200); self.send_header('Content-Length', '1234'); self.end_headers()
        elif parts[0] == 'cookie':
            if parts[1] == 'set':
                self.send_response(200)
                self.send_header('Set-Cookie', 'vmware_soap_session="abc123"; Path=/; HttpOnly; Secure')
                self.send_header('Set-Cookie', 'other=1; Path=/')
                self.send_header('Content-Length', '0'); self.end_headers()
            elif parts[1] == 'del':
                self.send_response(200)
                self.send_header('Set-Cookie', 'other=; Max-Age=0; Path=/')
                self.send_header('Content-Length', '0'); self.end_headers()
            else:
                self._send(200, (self.headers.get('Cookie') or '').encode())
        elif parts[0] == 'auth':
            want = 'Basic ' + base64.b64encode(b'user:p\xc3\xa4ss word').decode()
            if self.headers.get('Authorization') == want: self._send(200, b'welcome')
            else: self._send(401, b'no', [('WWW-Authenticate', 'Basic realm="t"')])
        elif parts[0] == 'redir':
            self._send(302, b'moved', [('Location', '/len/5')])
        elif parts[0] == 'status':
            self._send(int(parts[1]), b'status ' + parts[1].encode())
        elif parts[0] == 'sneaky':
            self._send(200, b'ok')
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except Exception: pass
            self.close_connection = True
        elif parts[0] == 'cut':
            n = int(parts[1])
            self.send_response(200); self.send_header('Content-Length', '5000000'); self.end_headers()
            self._stream(n); self.wfile.flush()
            time.sleep(0.1)   # the abortive close below discards what the client has not read yet: let it read the head first
            try: self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            except Exception: pass
            self.close_connection = True
            try: self.connection.close()
            except Exception: pass
        elif parts[0] == 'cutchunk':
            n = int(parts[1])
            self.send_response(200); self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            off = 0
            while off < n:
                k = min(4096, n - off); self.wfile.write(b'%x\r\n' % k + common.pattern(off, k) + b'\r\n'); off += k
            self.wfile.flush(); self.close_connection = True
            try: self.connection.shutdown(socket.SHUT_RDWR)
            except Exception: pass
        elif parts[0] == 'slow':
            time.sleep(20); self.close_connection = True
        elif parts[0] == 'count':
            with LOCK: self._send(200, ('%d %d' % (STATS['conns'], STATS['reqs'])).encode())
        elif parts[0] == 'tlsinfo':
            v = self.connection.version() if hasattr(self.connection, 'version') else 'none'
            self._send(200, (v or 'none').encode())
        else:
            self._send(404, b'not found')

    def handle_expect_100(self):
        # a refused "Expect: 100-continue" is answered with the final status at once (http.server would send 100 first, and the
        # client's body would then run into the closed socket: the RST discards the 401 before the client has read it)
        if self.path.strip('/').split('/')[0] == 'reject':
            self._send(401, b'denied', close=True); return False
        return super().handle_expect_100()

    def do_PUT(self):
        parts = self.path.strip('/').split('/')
        if parts[0] == 'reject':
            self._send(401, b'denied', close=True); return
        try:
            size, crc = self._read_body()
        except EOFError:
            self.close_connection = True; return
        self._send(201, ('%d %d' % (size, crc)).encode())

    def do_POST(self):
        parts = self.path.strip('/').split('/')
        try:
            size, crc = self._read_body()
        except EOFError:
            self.close_connection = True; return
        if parts[0] == 'echo':
            self._send(200, self._last_body)
        else:
            self._send(201, ('%d %d' % (size, crc)).encode())


class Srv(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64


def serve(port, ctx):
    class TlsSrv(Srv):
        def process_request_thread(self, request, client_address):
            try:
                request = ctx.wrap_socket(request, server_side=True)
            except Exception:
                try: request.close()
                except Exception: pass
                return
            super().process_request_thread(request, client_address)
    srv = (TlsSrv if ctx is not None else Srv)((common.BIND, port), H)
    srv.serve_forever()


if __name__ == '__main__':
    certdir = sys.argv[1]
    if not os.path.exists(os.path.join(certdir, 'server.pem')): common.make_certs(certdir)
    for spec in sys.argv[2:]:
        parts = spec.split(':')
        port = int(parts[0]); ctx = None
        if len(parts) > 1 and parts[1] == 'tls':
            cert = parts[2] if len(parts) > 2 else 'server'
            ctx = common.tls_context(os.path.join(certdir, cert + '.pem'), os.path.join(certdir, cert + '.key'))
        threading.Thread(target=serve, args=(port, ctx), daemon=True).start()
    print('ready', flush=True)
    threading.Event().wait()
