#!/usr/bin/env python3
"""Отдаёт ЦЕЛИКОМ содержимое одного файла журнала каждому, кто подключился по TCP, и закрывает соединение.
   logread.py <адрес> <порт> <файл>
Нужен тестам `ssh/auth_guard_scp_*`: на ВМ Windows гость раньше читал журнал sshd через SMB, и кеш клиента SMB отдавал старый файл.
Прямое чтение по TCP кеша не имеет: каждое подключение видит файл, каким он есть на хосте в этот момент."""
import socket, sys, threading

def main():
    host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind((host, port)); ls.listen(8)
    def serve(c):
        try:
            try: data = open(path, "rb").read()
            except OSError: data = b""
            c.sendall(data)
        except OSError: pass
        finally:
            try: c.shutdown(socket.SHUT_WR)
            except OSError: pass
            c.close()
    while True:
        c, _ = ls.accept(); threading.Thread(target=serve, args=(c,), daemon=True).start()

main()
