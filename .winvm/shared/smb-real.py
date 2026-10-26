#!/usr/bin/env python3
"""Runs INSIDE the vm2b-win container: a password-protected Samba share for the real-SMB autotests (group smbreal) and a control port.
 docker exec -d vm2b-win python3 /shared/smb-real.py
 smbd listens on 127.0.0.1:4455; qemu forwards the guest's 10.0.2.50:445 and 10.0.2.51:445 to it (guestfwd, see x86/qemu-run.sh).
 Share [vm2b] = /srv/vm2b, user vm2buser (password below).  Control port 127.0.0.1:4456 (the guest reaches it as 10.0.2.2:4456), one line per
 connection:  stop (kill smbd and every session), start, restart, status, wipe (empty the share), ls (list the share)  -> answer 'ok ...'."""
import os, signal, socket, subprocess, sys, shutil, threading
USER, PW = "vm2buser", "Sm8 Real&Pw!42"
CYR_USER, CYR_PW = "юзер-смб", "Пар0ль-Смб!42"             # кириллическая учётная запись SMB (smbreal/real_cyrillic_share_and_account_names; unix-пользователь создаётся с --badname)
EXTRA = {"vm2svc": "Svc-Pass-123!x", CYR_USER: CYR_PW}                     # учётная запись службы из svcacct.ps1: пустой пользователь в записи сервера = права самой службы
SIGNING = "auto"                                           # команда control "signing on|off" -> mandatory | auto (подпись SMB обязательна на стороне сервера)
SHARE = "/srv/vm2b"; CONF = "/tmp/smbreal/smb.conf"; DIR = "/tmp/smbreal"
proc = None; lock = threading.Lock()

def write_conf():
    open(CONF, "w", encoding="utf-8").write(CONF_TEXT.format(DIR=DIR, SHARE=SHARE, USER=USER, SIGN=SIGNING, ALL=USER + " " + " ".join(EXTRA)))

def setup():
    os.makedirs(DIR, exist_ok=True); os.makedirs(SHARE, exist_ok=True); os.chmod(SHARE, 0o777)
    if subprocess.call(["id", USER], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0:
        subprocess.check_call(["useradd", "-M", "-s", "/usr/sbin/nologin", USER])
    for u in list(EXTRA):                                  # кириллическая учётная запись: если useradd её не принимает, обходимся без неё (тест пропустит кириллическое имя пользователя)
        if subprocess.call(["id", u], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) != 0 and subprocess.call(["useradd", "-M", "--badname", "-s", "/usr/sbin/nologin", u]) != 0:
            print("cannot create the unix user", u); del EXTRA[u]
    write_conf()
    for u, pw in [(USER, PW)] + list(EXTRA.items()):
        p = subprocess.Popen(["smbpasswd", "-a", "-s", "-c", CONF, u], stdin=subprocess.PIPE); p.communicate((pw + "\n" + pw + "\n").encode("utf-8"))

CONF_TEXT = """[global]
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = 4455
private dir = {DIR}
pid directory = {DIR}
lock directory = {DIR}
state directory = {DIR}
cache directory = {DIR}
log file = {DIR}/log.smbd
server signing = {SIGN}
max log size = 5000
security = user
map to guest = Never
passdb backend = tdbsam:{DIR}/passdb.tdb
server min protocol = SMB2
load printers = no
disable spoolss = yes
usershare max shares = 0
[vm2b]
path = {SHARE}
read only = no
valid users = {ALL}
force user = root
browseable = yes
comment = VM2Backup real SMB test share
[резерв]
path = {SHARE}
read only = no
valid users = {ALL}
force user = root
browseable = yes
comment = Cyrillic share name
[vm2benc]
path = {SHARE}
read only = no
valid users = {ALL}
force user = root
browseable = yes
smb encrypt = required
comment = SMB3 encryption required
"""

def start():
    global proc
    with lock:
        if proc and proc.poll() is None: return "already running"
        proc = subprocess.Popen(["smbd", "-F", "--no-process-group", "-s", CONF], start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try: socket.create_connection(("127.0.0.1", 4455), 0.2).close(); return "started"
            except OSError: import time; time.sleep(0.1)
        return "started (not answering yet)"

def stop():
    global proc
    with lock:
        if not proc or proc.poll() is not None: proc = None; return "not running"
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        proc.wait(); proc = None
        # smbd убит SIGKILL: в базах блокировок остаются записи (оплоки / аренды) мёртвого процесса; следующий smbd ждёт ответа на их "перехват" по таймауту (30-60 с, "defer_open_done"
        # NT_STATUS_IO_TIMEOUT), и обращения гостя к шаре зависают. Для тестов "сервер пропал и вернулся" состояние блокировок чистим при каждой остановке.
        for n in os.listdir(DIR):
            if n in ("locking.tdb", "brlock.tdb", "leases.tdb", "smbd_cleanupd.tdb", "mutex.tdb") or n.startswith("smbXsrv_") or n.startswith("notify"):
                try: os.remove(os.path.join(DIR, n))
                except OSError: pass
        return "stopped"

def handle(c):
    try:
        cmd = c.makefile().readline().strip()
        if cmd == "stop": r = stop()
        elif cmd == "start": r = start()
        elif cmd == "restart": stop(); r = start()
        elif cmd in ("signing on", "signing off", "signing disabled"):
            global SIGNING
            stop(); SIGNING = {"signing on": "mandatory", "signing off": "auto", "signing disabled": "disabled"}[cmd]; write_conf(); r = "signing " + SIGNING + ": " + start()
        elif cmd == "status": r = "running" if proc and proc.poll() is None else "stopped"
        elif cmd == "wipe":
            for n in os.listdir(SHARE): shutil.rmtree(os.path.join(SHARE, n), ignore_errors=True) if os.path.isdir(os.path.join(SHARE, n)) else os.remove(os.path.join(SHARE, n))
            r = "wiped"
        elif cmd == "ls": r = " ".join(sorted(os.listdir(SHARE)))
        else: r = "unknown command"
        c.sendall(("ok " + r + "\n").encode())
    except Exception as e:
        try: c.sendall(("err %s\n" % e).encode())
        except OSError: pass
    finally: c.close()

def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d: break
            b.sendall(d)
    except OSError: pass
    finally:
        for x in (a, b):
            try: x.shutdown(socket.SHUT_RDWR)
            except OSError: pass

def proxy(ip):
    """dockur variant: the guest (172.30.0.2) reaches the test Samba as <ip>:445; dockur's own smbd owns 172.30.0.1:445, so the extra addresses
    (secondary addresses of the bridge 'docker') are served by a small TCP forwarder to smbd on 127.0.0.1:4455."""
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind((ip, 445)); ls.listen(32)
    def run():
        while True:
            c, _ = ls.accept()
            try: u = socket.create_connection(("127.0.0.1", 4455), 3)
            except OSError: c.close(); continue
            threading.Thread(target=pipe, args=(c, u), daemon=True).start(); threading.Thread(target=pipe, args=(u, c), daemon=True).start()
    threading.Thread(target=run, daemon=True).start()

if __name__ == "__main__":
    setup(); print("smbd:", start(), flush=True)
    ctl = "127.0.0.1"
    if subprocess.call("ip link show docker >/dev/null 2>&1", shell=True) == 0:      # dockur network: guest 172.30.0.2, gateway = this container
        for ip in ("172.30.0.50", "172.30.0.51"):
            subprocess.call(["ip", "addr", "add", ip + "/24", "dev", "docker"], stderr=subprocess.DEVNULL); proxy(ip)
        ctl = "172.30.0.1"
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind((ctl, 4456)); s.listen(8)
    while True:
        c, _ = s.accept(); threading.Thread(target=handle, args=(c,), daemon=True).start()
