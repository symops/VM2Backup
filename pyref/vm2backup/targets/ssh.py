"""SCP поверх SSH (paramiko). SFTP больше не поддерживается."""
import base64
import hashlib
import posixpath
import shlex
import tempfile

import paramiko

from .base import Target, TargetError


class _PinnedPolicy(paramiko.MissingHostKeyPolicy):
    def __init__(self, fingerprint):
        self.fp = fingerprint.removeprefix("SHA256:").rstrip("=")

    def missing_host_key(self, client, hostname, key):
        got = base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode().rstrip("=")
        if got != self.fp:
            raise paramiko.SSHException(f"отпечаток ключа сервера {hostname} не совпал: SHA256:{got}")


class _SshTarget(Target):
    def __init__(self, host, port=22, user=None, password=None, key_file=None, key_passphrase=None,
                 directory=".", host_key_sha256=None, known_hosts=None, insecure_auto_add=False,
                 timeout=30):
        self.host, self.port, self.user = host, port or 22, user
        self.password, self.key_file, self.key_passphrase = password, key_file, key_passphrase
        self.directory, self.timeout = directory, timeout
        self.host_key_sha256, self.known_hosts, self.insecure = host_key_sha256, known_hosts, insecure_auto_add
        self._ssh = None

    def connect(self):
        ssh = paramiko.SSHClient()
        ssh.load_system_host_keys()
        if self.known_hosts:
            ssh.load_host_keys(self.known_hosts)
        if self.host_key_sha256:
            ssh.set_missing_host_key_policy(_PinnedPolicy(self.host_key_sha256))
        elif self.insecure:
            ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        else:
            ssh.set_missing_host_key_policy(paramiko.RejectPolicy())
        try:
            ssh.connect(self.host, self.port, self.user, self.password, key_filename=self.key_file,
                        passphrase=self.key_passphrase, timeout=self.timeout,
                        look_for_keys=not self.password and not self.key_file,
                        allow_agent=not self.password and not self.key_file)
        except (paramiko.SSHException, OSError) as e:
            raise TargetError(f"SSH: {e}")
        self._ssh = ssh
        self._after_connect()
        return self

    def _after_connect(self):
        pass

    def close(self):
        if self._ssh:
            self._ssh.close()
            self._ssh = None

    def _exec(self, cmd):
        _, out, err = self._ssh.exec_command(cmd, timeout=self.timeout)
        data = out.read().decode("utf-8", "replace")
        code = out.channel.recv_exit_status()
        return code, data, err.read().decode("utf-8", "replace")


# ---- SCP (классический протокол scp -t / scp -f, нужен scp на сервере) ----

def _ack(chan):
    b = chan.recv(1)
    if not b:
        raise TargetError("SCP: соединение закрыто сервером")
    if b != b"\0":
        msg = b""
        while not msg.endswith(b"\n"):
            c = chan.recv(1)
            if not c:
                break
            msg += c
        raise TargetError("SCP: " + msg.decode("utf-8", "replace").strip())


class _ScpWriter:
    """SCP требует размер файла до передачи, поэтому данные копятся во временном файле."""

    def __init__(self, target, name):
        self._t, self._name = target, name
        self._spool = tempfile.TemporaryFile()

    def write(self, data):
        return self._spool.write(data)

    def flush(self):
        pass

    def close(self):
        t, name = self._t, self._name
        size = self._spool.seek(0, 2)
        self._spool.seek(0)
        chan = t._ssh.get_transport().open_session(timeout=t.timeout)
        chan.exec_command(f"scp -t {shlex.quote(t._dir())}")
        try:
            _ack(chan)
            chan.sendall(f"C0600 {size} {name}.part\n".encode())
            _ack(chan)
            while True:
                chunk = self._spool.read(1 << 20)
                if not chunk:
                    break
                chan.sendall(chunk)
            chan.sendall(b"\0")
            _ack(chan)
            chan.shutdown_write()
        finally:
            chan.close()
            self._spool.close()
        code, _, err = t._exec(f"mv -f {shlex.quote(t._p(name + '.part'))} {shlex.quote(t._p(name))}")
        if code:
            raise TargetError("SCP: не удалось переименовать файл: " + err.strip())

    def abort(self):
        self._spool.close()


class _ScpReader:
    def __init__(self, target, name):
        self._chan = target._ssh.get_transport().open_session(timeout=target.timeout)
        self._chan.exec_command(f"scp -f {shlex.quote(target._p(name))}")
        self._chan.sendall(b"\0")
        hdr = b""
        while not hdr.endswith(b"\n"):
            c = self._chan.recv(1)
            if not c:
                raise TargetError("SCP: файл не найден или доступ запрещён")
            hdr += c
        if hdr[:1] in (b"\1", b"\2"):
            raise TargetError("SCP: " + hdr[1:].decode("utf-8", "replace").strip())
        if hdr[:1] != b"C":
            raise TargetError("SCP: неожиданный ответ сервера")
        self._left = int(hdr.split()[1])
        self._chan.sendall(b"\0")

    def read(self, n=-1):
        if n is None or n < 0:
            out = bytearray()
            while True:
                d = self.read(1 << 20)
                if not d:
                    return bytes(out)
                out += d
        if self._left <= 0:
            return b""
        data = self._chan.recv(min(n, self._left))
        if not data:
            raise TargetError("SCP: передача оборвана")
        self._left -= len(data)
        return data

    def close(self):
        try:
            if self._left <= 0:
                self._chan.recv(1)
                self._chan.sendall(b"\0")
        except Exception:
            pass
        self._chan.close()


class ScpTarget(_SshTarget):
    kind = "scp"

    def _dir(self):
        return self.directory

    def _p(self, name):
        return posixpath.join(self.directory, name)

    def _after_connect(self):
        code, _, err = self._exec(f"mkdir -p {shlex.quote(self.directory)}")
        if code:
            raise TargetError("SCP: не удалось создать каталог: " + err.strip())

    def open_write(self, name):
        return _ScpWriter(self, name)

    def open_read(self, name):
        return _ScpReader(self, name)

    def list(self):
        code, out, err = self._exec(f"ls -1 {shlex.quote(self.directory)}")
        if code:
            raise TargetError("SCP: " + err.strip())
        return sorted(n for n in out.splitlines() if n and not n.endswith(".part"))

    def delete(self, name):
        self._exec(f"rm -f {shlex.quote(self._p(name))}")
