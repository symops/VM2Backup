# A small TLS server for the "tls" tests of VM2Backup.  One listener per --listen spec:  port:cert:min:max   (cert = name from make_certs;
# min / max = 1.0 .. 1.3 or '-').  Commands (one line, then raw bytes):
#   PING               -> "PONG reused=<0|1> ver=<TLSv1.x> sid=<hex>\n"
#   SEND n             -> n bytes of the pattern, then (per the previous RAGGED / NOTIFY choice) closes
#   RECV n             -> reads n bytes, answers "GOT n crc\n"
#   HANG               -> never answers (timeout tests)
#   BYE                -> "bye" then close_notify (unwrap)
#   RAGGED             -> "bye" then the socket is closed WITHOUT close_notify
import socket, sys, threading, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_common as common


def readline(c):
    b = b''
    while not b.endswith(b'\n'):
        d = c.recv(1)
        if not d:
            return None
        b += d
    return b.strip().decode('latin1')


def handle(c):
    try:
        while True:
            line = readline(c)
            if line is None:
                break
            p = line.split()
            if p[0] == 'PING':
                s = c.session_reused if hasattr(c, 'session_reused') else False
                c.sendall(('PONG reused=%d ver=%s\n' % (1 if s else 0, c.version())).encode())
            elif p[0] == 'SEND':
                n = int(p[1]); off = 0
                while off < n:
                    k = min(n - off, 1 << 20)
                    c.sendall(common.pattern(off, k)); off += k
            elif p[0] == 'RECV':
                n = int(p[1]); crc = 0; got = 0
                while got < n:
                    d = c.recv(min(n - got, 1 << 16))
                    if not d:
                        break
                    crc = zlib.crc32(d, crc); got += len(d)
                c.sendall(('GOT %d %d\n' % (got, crc)).encode())
            elif p[0] == 'HANG':
                threading.Event().wait(30)
            elif p[0] == 'BYE':
                c.sendall(b'bye')
                c.settimeout(1.0)
                try:
                    c = c.unwrap()
                except Exception:
                    pass
                break
            elif p[0] == 'RAGGED':
                c.sendall(b'bye')
                try:
                    c.shutdown(socket.SHUT_RDWR)   # TCP only: SSLSocket.shutdown would not add close_notify
                except Exception:
                    pass
                break
            else:
                c.sendall(b'ERR\n')
    except Exception as e:
        pass
    finally:
        try:
            c.close()
        except Exception:
            pass


def serve(port, ctx, starttls=False):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((common.BIND, port)); s.listen(50)
    while True:
        c, _ = s.accept()
        def run(c=c):
            try:
                if starttls:
                    c.sendall(b'READY\n')
                    l = readline(c)
                    if l != 'STARTTLS':
                        c.close(); return
                    c.sendall(b'GO\n')
                c = ctx.wrap_socket(c, server_side=True)
            except Exception as e:
                try: c.close()
                except Exception: pass
                return
            handle(c)
        threading.Thread(target=run, daemon=True).start()


if __name__ == '__main__':
    certdir = sys.argv[1]
    certs = common.make_certs(certdir) if not os.path.exists(os.path.join(certdir, 'server.pem')) else None
    for spec in sys.argv[2:]:
        parts = spec.split(':')
        port, cert, vmin, vmax = parts[:4]
        st = len(parts) > 4 and parts[4] == 'starttls'
        cp, kp = os.path.join(certdir, cert + '.pem'), os.path.join(certdir, cert + '.key')
        ctx = common.tls_context(cp, kp, None if vmin == '-' else vmin, None if vmax == '-' else vmax, seclevel0=(vmin in ('1.0', '1.1')))
        threading.Thread(target=serve, args=(int(port), ctx, st), daemon=True).start()
    print('ready', flush=True)
    threading.Event().wait()
