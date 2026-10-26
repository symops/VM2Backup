#!/usr/bin/env python3
"""Test servers for the SSH / SCP autotests (paramiko based), all in one process:

  base+1  scp       password + keyboard-interactive + public key; exec emulates mkdir/ls/rm/mv/scp -t/scp -f (the classic scp protocol)
  base+2  kbdint    keyboard-interactive only, one "Password:" prompt (ESXi style)
  base+3  kbdint2   keyboard-interactive only, two rounds (2 prompts, then 1)
  base+21 fault_scp like scp, with fault injection controlled over a TCP control port (port+10)
  base+40.. garbage servers speaking nonsense (see GARBAGE)
  base+60 mitm      a relay to the real sshd that damages the stream on command (control port base+70)

Fault control (one line per command, "k=v k=v ..."; applies to the NEXT connection to that server unless sticky=1):
  drop_c2s=N     close the socket after N bytes were received from the client
  drop_s2c=N     close the socket after N bytes were sent (the last write is cut in the middle)
  rst=1          close with RST instead of FIN
  hang_c2s=N     after N bytes received stop reading and writing (black hole, no close)
  idle_drop=S    close the connection after S seconds without traffic
  rekey=N        paramiko starts a key exchange every N bytes / N/500 packets (rekey storm)
  slow_ms=M      sleep M ms before every write to the client
  evil_scp=m     `scp -f` answers with garbage: bigsize badline negative errormsg short noack
  clear=1        forget everything pending
  stats=1        replies with a JSON line {"conns":..,"fired":..}
"""
import argparse, base64, hashlib, json, os, random, shlex, socket, struct, sys, threading, time
import paramiko
from paramiko.common import AUTH_SUCCESSFUL, AUTH_FAILED, OPEN_SUCCEEDED, OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

import logging
logging.getLogger("paramiko").setLevel(logging.CRITICAL)

ARGS = None
BIND = os.environ.get("VM2B_BIND_ADDR", "127.0.0.1")           # where the servers listen / what a client must use (the Windows VM harness: the Docker bridge address)
HOST = os.environ.get("VM2B_SERVER_HOST", BIND)


def GUEST_PATH(p):
    m = os.environ.get("VM2B_GUEST_MAP", "")                  # <host prefix>=<guest prefix>, as in net_common.guest_path
    if m:
        hp, gp = m.split("=", 1)
        if p == hp or p.startswith(hp.rstrip("/") + "/"):
            return gp.rstrip("\\") + p[len(hp.rstrip("/")):].replace("/", "\\")
    return "Z:" + p
AUTHKEYS = set()
HOSTKEYS = []
FIRED = [0]


def fp_of(key):
    return "SHA256:" + base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode().rstrip("=")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class ScpExec:
    def __init__(self, root, channel, cmd, evil=None):
        self.root, self.ch, self.evil = root, channel, evil
        self.cmd = cmd

    def real(self, p):
        import posixpath
        if not p.startswith("/"): p = "/" + p
        return self.root + posixpath.normpath(p)

    def out(self, s):
        self.ch.sendall(s if isinstance(s, bytes) else s.encode())

    def err(self, s):
        self.ch.sendall_stderr(s if isinstance(s, bytes) else s.encode())

    def byte(self):
        d = self.ch.recv(1)
        if not d: raise EOFError()
        return d

    def line(self):
        b = b""
        while True:
            c = self.byte()
            if c == b"\n": return b.decode("utf-8", "replace")
            b += c

    def run(self):
        code = 0
        try:
            argv = shlex.split(self.cmd)
            code = self.dispatch(argv)
        except EOFError:
            code = 1
        except Exception as e:
            try: self.err("error: %s\n" % e)
            except Exception: pass
            code = 1
        try:
            self.ch.send_exit_status(code)
            self.ch.shutdown_write()
            self.ch.close()
        except Exception:
            pass

    def dispatch(self, a):
        if a[:2] == ["mkdir", "-p"]:
            os.makedirs(self.real(a[-1]), exist_ok=True); return 0
        if a[0] == "ls":
            p = self.real(a[-1])
            try: names = sorted(os.listdir(p))
            except OSError as e:
                self.err("ls: cannot access '%s': %s\n" % (a[-1], e.strerror)); return 2
            for n in names:
                self.out(n + ("/" if os.path.isdir(os.path.join(p, n)) else "") + "\n")
            return 0
        if a[:2] == ["rm", "-f"]:
            try: os.remove(self.real(a[-1]))
            except FileNotFoundError: pass
            except OSError as e:
                self.err("rm: cannot remove '%s': %s\n" % (a[-1], e.strerror)); return 1
            return 0
        if a[:2] == ["mv", "-f"]:
            try: os.replace(self.real(a[-2]), self.real(a[-1]))
            except OSError as e:
                self.err("mv: cannot move: %s\n" % e.strerror); return 1
            return 0
        if a[:2] == ["scp", "-t"]: return self.sink(self.real(a[-1]))
        if a[:2] == ["scp", "-f"]: return self.source(self.real(a[-1]), a[-1])
        self.err("sh: %s: command not found\n" % a[0]); return 127

    def sink(self, d):
        self.out(b"\0")
        while True:
            try: first = self.byte()
            except EOFError: return 0
            l = self.line()
            if first != b"C":
                self.out(b"\x01bad scp control line\n"); return 1
            mode, size, name = l.split(" ", 2)
            size = int(size)
            if "/" in name or name in ("", ".", ".."):
                self.out(b"\x01bad file name\n"); return 1
            try:
                f = open(os.path.join(d, name), "wb")
            except OSError as e:
                self.out(b"\x01scp: " + name.encode() + b": " + e.strerror.encode() + b"\n"); return 1
            self.out(b"\0")
            left = size
            with f:
                while left:
                    chunk = self.ch.recv(min(65536, left))
                    if not chunk: raise EOFError()
                    f.write(chunk); left -= len(chunk)
            self.byte()                  # the 0 after the data
            self.out(b"\0")

    def source(self, p, shown):
        if self.evil:
            return self.evil_source()
        self.byte()                      # the client's 0
        try: f = open(p, "rb")
        except OSError as e:
            self.out(b"\x01scp: " + shown.encode() + b": " + e.strerror.encode() + b"\n"); return 1
        with f:
            size = os.fstat(f.fileno()).st_size
            self.out(("C0644 %d %s\n" % (size, os.path.basename(p))))
            if self.byte() != b"\0": return 1
            while True:
                d = f.read(65536)
                if not d: break
                self.ch.sendall(d)
            self.out(b"\0")
            self.byte()
        return 0

    def evil_source(self):
        self.byte(); FIRED[0] += 1
        m = self.evil
        if m == "bigsize":
            self.out("C0644 100000000000000000 f\n"); self.byte(); self.out(os.urandom(300000)); time.sleep(0.3); return 1
        if m == "badline": self.out("Cxyz bogus\n"); return 1
        if m == "negative": self.out("C0644 -5 f\n"); return 1
        if m == "errormsg": self.out(b"\x01scp: boom: Permission denied\n"); return 1
        if m == "short":
            self.out("C0644 100 f\n"); self.byte(); self.out(os.urandom(50)); return 1
        if m == "noack":
            self.out("C0644 10 f\n"); self.byte(); self.out(os.urandom(10)); return 0
        return 1


# ----------------------------------------------------------------------------------------------------------- the server
class FaultSocket:
    def __init__(self, sock, faults, owner):
        self.s = sock; self.f = faults; self.recvd = 0; self.sent = 0; self.last = time.time(); self.dead = False; self.owner = owner
        self.hung = False
        if faults.get("idle_drop"):
            threading.Thread(target=self.watch, daemon=True).start()

    def watch(self):
        lim = float(self.f["idle_drop"])
        while not self.dead:
            time.sleep(0.2)
            if time.time() - self.last > lim:
                FIRED[0] += 1; self.kill(); return

    def kill(self):
        if self.dead: return
        self.dead = True
        try:
            if self.f.get("rst"): self.s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.s.close()
        except Exception: pass

    def recv(self, n):
        if self.dead: raise OSError("closed")
        if self.hung:
            time.sleep(30); raise OSError("hung")
        d = self.s.recv(n)
        self.last = time.time()
        self.recvd += len(d)
        h = self.f.get("hang_c2s")
        if h and self.recvd >= int(h):
            FIRED[0] += 1; self.hung = True
        c = self.f.get("drop_c2s")
        if c and self.recvd >= int(c):
            FIRED[0] += 1; self.kill(); raise OSError("dropped")
        return d

    def send(self, data):
        if self.dead: raise OSError("closed")
        if self.hung:
            time.sleep(30); raise OSError("hung")
        ms = self.f.get("slow_ms")
        if ms: time.sleep(int(ms) / 1000.0)
        c = self.f.get("drop_s2c")
        if c and self.sent + len(data) >= int(c):
            cut = max(0, int(c) - self.sent)
            try: self.s.sendall(data[:cut])
            except Exception: pass
            FIRED[0] += 1; self.kill(); raise OSError("dropped")
        n = self.s.send(data)
        self.sent += n; self.last = time.time()
        return n

    def sendall(self, data):
        while data:
            n = self.send(data); data = data[n:]

    def close(self):
        self.dead = True
        try: self.s.close()
        except Exception: pass

    def __getattr__(self, name):
        return getattr(self.s, name)


class Srv(paramiko.ServerInterface):
    def __init__(self, cfg, faults):
        self.cfg, self.faults = cfg, faults
        self.event = threading.Event()
        self.kbd_round = 0

    def get_allowed_auths(self, username):
        return ",".join(self.cfg["auths"])

    def check_auth_password(self, username, password):
        if "password" in self.cfg["auths"] and username == ARGS.user and password == ARGS.password: return AUTH_SUCCESSFUL
        return AUTH_FAILED

    def check_auth_publickey(self, username, key):
        if "publickey" in self.cfg["auths"] and username == ARGS.user and key.get_base64() in AUTHKEYS: return AUTH_SUCCESSFUL
        return AUTH_FAILED

    def check_auth_interactive(self, username, submethods):
        if "keyboard-interactive" not in self.cfg["auths"] or username != ARGS.user: return AUTH_FAILED
        self.kbd_round = 0
        q = paramiko.InteractiveQuery("", "")
        q.add_prompt("Password: ", False)
        if self.cfg.get("kbd_rounds", 1) == 2: q.add_prompt("Verification: ", False)
        return q

    def check_auth_interactive_response(self, responses):
        ok = all(r == ARGS.password for r in responses) and len(responses) > 0
        if not ok: return AUTH_FAILED
        if self.cfg.get("kbd_rounds", 1) == 2 and self.kbd_round == 0:
            self.kbd_round = 1
            q = paramiko.InteractiveQuery("", "")
            q.add_prompt("Token: ", False)
            return q
        return AUTH_SUCCESSFUL

    def check_channel_request(self, kind, chanid):
        return OPEN_SUCCEEDED if kind == "session" else OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_exec_request(self, channel, command):
        cmd = command.decode("utf-8", "replace")
        evil = self.faults.get("evil_scp")
        job = ScpExec(ARGS.root, channel, cmd, evil)

        def later():
            time.sleep(0.05)          # paramiko sends the channel success only after this callback returns: let it go first
            job.run()
        threading.Thread(target=later, daemon=True).start()
        return True


class Listener(threading.Thread):
    def __init__(self, name, port, cfg):
        super().__init__(daemon=True)
        self.name_, self.port, self.cfg = name, port, cfg
        self.pending = {}; self.sticky = False; self.conns = 0
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind((BIND, port)); self.ls.listen(50)

    def run(self):
        while True:
            try: s, _ = self.ls.accept()
            except Exception: return
            self.conns += 1
            f = dict(self.pending)
            if not self.sticky: self.pending = {}
            threading.Thread(target=self.serve, args=(s, f), daemon=True).start()

    def serve(self, s, faults):
        try:
            sock = FaultSocket(s, faults, self) if faults else s
            t = paramiko.Transport(sock, default_window_size=int(self.cfg.get('window', 2097152)), default_max_packet_size=int(self.cfg.get('maxpkt', 32768)))
            for k in HOSTKEYS: t.add_server_key(k)
            if faults.get("rekey"):
                n = int(faults["rekey"])
                t.packetizer.REKEY_BYTES = n; t.packetizer.REKEY_PACKETS = max(8, n // 500)
            srv = Srv(self.cfg, faults)
            t.start_server(server=srv)
            while t.is_active():
                time.sleep(0.2)
            t.close()
        except Exception:
            pass
        finally:
            try: s.close()
            except Exception: pass


class Control(threading.Thread):
    def __init__(self, port, lst):
        super().__init__(daemon=True)
        self.lst = lst
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind((BIND, port)); self.ls.listen(5)

    def run(self):
        while True:
            c, _ = self.ls.accept()
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        f = c.makefile("rw")
        for line in f:
            kv = {}
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1); kv[k] = v
            if kv.get("clear"):
                self.lst.pending = {}; self.lst.sticky = False; f.write("ok\n"); f.flush(); continue
            if kv.get("stats"):
                f.write(json.dumps({"conns": self.lst.conns, "fired": FIRED[0]}) + "\n"); f.flush(); continue
            self.lst.sticky = bool(kv.pop("sticky", None))
            self.lst.pending = kv
            f.write("ok\n"); f.flush()


class Mitm(threading.Thread):
    """A relay to the real sshd that damages the stream on command: flip_c2s=N / flip_s2c=N (one bit at byte offset N),
    cut_c2s=N / cut_s2c=N (close after N bytes), dup_s2c=N (repeat a 64 byte piece at offset N). Applies to the next connection."""
    def __init__(self, port, target):
        super().__init__(daemon=True)
        self.target = target; self.pending = {}
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind((BIND, port)); self.ls.listen(20)

    def run(self):
        while True:
            c, _ = self.ls.accept()
            f = self.pending; self.pending = {}
            threading.Thread(target=self.relay, args=(c, f), daemon=True).start()

    def relay(self, c, f):
        try: u = socket.create_connection(("127.0.0.1", self.target))
        except Exception:
            c.close(); return

        def pipe(src, dst, tag):
            n = 0
            try:
                while True:
                    d = src.recv(65536)
                    if not d: break
                    flip = f.get("flip_" + tag); cut = f.get("cut_" + tag); dup = f.get("dup_" + tag)
                    if flip is not None and n <= int(flip) < n + len(d):
                        b = bytearray(d); b[int(flip) - n] ^= 1 << random.randint(0, 7); d = bytes(b); FIRED[0] += 1
                    if dup is not None and n <= int(dup) < n + len(d):
                        i = int(dup) - n; d = d[:i + 64] + d[i:i + 64] + d[i + 64:]; FIRED[0] += 1
                    if cut is not None and n + len(d) >= int(cut):
                        dst.sendall(d[:max(0, int(cut) - n)]); FIRED[0] += 1; break
                    dst.sendall(d); n += len(d)
            except Exception: pass
            finally:
                for x in (src, dst):
                    try: x.shutdown(socket.SHUT_RDWR)
                    except Exception: pass
                    try: x.close()
                    except Exception: pass
        threading.Thread(target=pipe, args=(c, u, "c2s"), daemon=True).start()
        threading.Thread(target=pipe, args=(u, c, "s2c"), daemon=True).start()


class MitmControl(Control):
    def handle(self, c):
        f = c.makefile("rw")
        for line in f:
            kv = {}
            for tok in line.split():
                if "=" in tok:
                    k, v = tok.split("=", 1); kv[k] = v
            self.lst.pending = {} if kv.get("clear") else kv
            f.write("ok\n"); f.flush()


# ------------------------------------------------------------------------------------------------------------ garbage
def pkt(payload):
    pad = 8 - ((5 + len(payload)) % 8)
    if pad < 4: pad += 8
    body = bytes([pad]) + payload + os.urandom(pad)
    return struct.pack(">I", len(body)) + body


def nl(s):
    b = s.encode(); return struct.pack(">I", len(b)) + b


def nl_b(b):
    return struct.pack(">I", len(b)) + b


def kexinit(kex, hk="ssh-ed25519", ciph="aes128-ctr", mac="hmac-sha2-256"):
    return (bytes([20]) + os.urandom(16) + nl(kex) + nl(hk) + nl(ciph) + nl(ciph) + nl(mac) + nl(mac) + nl("none") + nl("none") + nl("") + nl("") + b"\0" + struct.pack(">I", 0))


def read_line(s):
    b = b""
    while not b.endswith(b"\n"):
        d = s.recv(1)
        if not d: raise EOFError()
        b += d
    return b


def read_pkt(s):
    h = b""
    while len(h) < 4:
        d = s.recv(4 - len(h))
        if not d: raise EOFError()
        h += d
    n = struct.unpack(">I", h)[0]
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: raise EOFError()
        b += d
    return b[1:n - b[0]]


GARBAGE = ["huge_banner", "banner_lines", "bad_version", "oversize_len", "random_after_banner", "kexinit_bad_lists", "kex_garbage_reply",
           "truncate", "silent", "dribble", "close_immediately", "kex_zero_point", "kexinit_empty_lists", "kex_bad_sig"]


def garbage(mode, s):
    try:
        s.settimeout(30)
        FIRED[0] += 1
        if mode == "huge_banner": s.sendall(b"A" * 200000); time.sleep(0.5); return
        if mode == "banner_lines":
            t0 = time.time()
            while time.time() - t0 < 20: s.sendall(b"hello there\r\n" * 500)
            return
        if mode == "bad_version": s.sendall(b"SSH-1.5-ancient\r\n"); time.sleep(0.5); return
        if mode == "close_immediately": return
        if mode == "silent": time.sleep(40); return
        if mode == "dribble":
            for ch in b"SSH-2.0-slowpoke\r\n":
                s.sendall(bytes([ch])); time.sleep(0.4)
            time.sleep(30); return
        s.sendall(b"SSH-2.0-garbage_1.0\r\n")
        if mode == "oversize_len": s.sendall(struct.pack(">I", 0xFFFFFFFF) + os.urandom(64)); time.sleep(0.5); return
        if mode == "random_after_banner": s.sendall(os.urandom(1 << 20)); time.sleep(0.5); return
        if mode == "truncate": s.sendall(struct.pack(">I", 1000) + os.urandom(100)); return
        read_line(s)
        if mode == "kexinit_bad_lists":
            p = bytes([20]) + os.urandom(16) + struct.pack(">I", 0xFFFFFFF0) + b"curve25519-sha256"
            s.sendall(pkt(p)); time.sleep(0.5); return
        if mode == "kexinit_empty_lists":
            s.sendall(pkt(kexinit("", "", "", ""))); time.sleep(0.5); return
        s.sendall(pkt(kexinit("curve25519-sha256")))
        read_pkt(s)                                   # the client's KEXINIT
        init = read_pkt(s)                            # KEX_ECDH_INIT
        if mode == "kex_garbage_reply":
            s.sendall(pkt(bytes([31]) + os.urandom(200))); time.sleep(0.5); return
        if mode == "kex_zero_point":
            ks = nl("ssh-ed25519") + nl("x" * 32)
            s.sendall(pkt(bytes([31]) + struct.pack(">I", len(ks)) + ks + nl("\0" * 32) + nl("sig"))); time.sleep(0.5); return
        if mode == "kex_bad_sig":
            k = HOSTKEYS[0]; ks = k.asbytes()
            q = os.urandom(32)
            sig = nl("ssh-ed25519") + nl_b(os.urandom(64))
            s.sendall(pkt(bytes([31]) + nl_b(ks) + nl_b(q) + nl_b(sig))); time.sleep(0.5); return
    except Exception:
        pass
    finally:
        try: s.close()
        except Exception: pass


def garbage_listener(mode, port):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((BIND, port)); ls.listen(20)
    while True:
        s, _ = ls.accept()
        threading.Thread(target=garbage, args=(mode, s), daemon=True).start()


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True); ap.add_argument("--user", required=True); ap.add_argument("--password", required=True)
    ap.add_argument("--base", type=int, default=22900); ap.add_argument("--sshd-port", type=int, default=22001)
    ap.add_argument("--keys", required=True); ap.add_argument("--info", required=True)
    ARGS = ap.parse_args()
    os.makedirs(ARGS.root, exist_ok=True)
    for fn in sorted(os.listdir(ARGS.keys)):
        if fn.startswith("u_") and fn.endswith(".pub"):
            parts = open(os.path.join(ARGS.keys, fn)).read().split()
            AUTHKEYS.add(parts[1])
    HOSTKEYS.append(paramiko.Ed25519Key.from_private_key_file(os.path.join(ARGS.keys, "host_ed25519")))
    HOSTKEYS.append(paramiko.RSAKey.from_private_key_file(os.path.join(ARGS.keys, "host_rsa")))
    allauth = ["password", "keyboard-interactive", "publickey"]
    specs = [
        ("scp", ARGS.base + 1, {"auths": allauth}),
        ("kbdint", ARGS.base + 2, {"auths": ["keyboard-interactive"], "kbd_rounds": 1}),
        ("kbdint2", ARGS.base + 3, {"auths": ["keyboard-interactive"], "kbd_rounds": 2}),
        ("fault_scp", ARGS.base + 21, {"auths": allauth}),
        # a server with a tiny channel window (32 KiB) and small packets (2 KiB): flow control must keep working in both directions
        ("tiny_scp", ARGS.base + 5, {"auths": allauth, "window": 32768, "maxpkt": 2048}),
    ]
    info = {"root_unix": ARGS.root, "root": GUEST_PATH(ARGS.root)}
    fp = fp_of(HOSTKEYS[0])
    for name, port, cfg in specs:
        l = Listener(name, port, cfg); l.start()
        e = {"type": "scp", "host": HOST, "port": port, "user": ARGS.user, "password": ARGS.password, "host_key": fp}
        if name.startswith("fault"):
            Control(port + 10, l).start(); e["control_port"] = port + 10
        info[name] = e
    m = Mitm(ARGS.base + 60, ARGS.sshd_port); m.start(); MitmControl(ARGS.base + 70, m).start()
    info["mitm"] = {"host": HOST, "port": ARGS.base + 60, "control_port": ARGS.base + 70, "user": ARGS.user, "password": ARGS.password}
    info["garbage"] = {"host": HOST, "base": ARGS.base + 40, "modes": GARBAGE}
    for i, mname in enumerate(GARBAGE):
        threading.Thread(target=garbage_listener, args=(mname, ARGS.base + 40 + i), daemon=True).start()
    tmp = ARGS.info + ".tmp"
    json.dump(info, open(tmp, "w")); os.replace(tmp, ARGS.info)
    log("ready")
    while True: time.sleep(3600)


if __name__ == "__main__":
    main()
