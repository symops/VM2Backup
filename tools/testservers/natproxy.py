# Прозрачный TCP-ретранслятор, который ИМИТИРУЕТ NAT перед FTP-сервером (раунд 11).
#   natproxy.py <адрес:порт для клиента> <адрес:порт сервера> [исходящий адрес]
# Клиент подключается к ретранслятору, тот открывает соединение к серверу с ДРУГОГО исходящего адреса (по умолчанию 127.0.0.9), поэтому
# сервер видит адрес управляющего соединения, не равный адресу клиента.  Так на loopback воспроизводится ровно то, что происходит с гостем Windows
# за NAT dockur: клиент в команде PORT называет свой адрес, а сервер pyftpdlib видит адрес NAT и отвечает
# «501 Rejected data connection to foreign address».  Данные (байты) ретранслятор не меняет, TLS не трогает.
import socket, sys, threading

listen, target = sys.argv[1], sys.argv[2]
src = sys.argv[3] if len(sys.argv) > 3 else '127.0.0.9'
lh, lp = listen.rsplit(':', 1); th, tp = target.rsplit(':', 1)


def pump(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def serve(c):
    try:
        u = socket.socket()
        if src not in ('', '-'):
            u.bind((src, 0))
        u.connect((th, int(tp)))
    except OSError:
        c.close(); return
    threading.Thread(target=pump, args=(c, u), daemon=True).start()
    threading.Thread(target=pump, args=(u, c), daemon=True).start()


ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind((lh, int(lp))); ls.listen(64)
print('ready', flush=True)
while True:
    c, _ = ls.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
