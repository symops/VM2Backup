"""FTP, FTPES (явный TLS, AUTH TLS) и FTPS (неявный TLS)."""
import ftplib
import posixpath
import ssl

from .base import Target, TargetError


class _ImplicitFTPTLS(ftplib.FTP_TLS):
    """Управляющее соединение сразу оборачивается в TLS (порт 990)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self._sock = None

    @property
    def sock(self):
        return self._sock

    @sock.setter
    def sock(self, value):
        if value is not None and not isinstance(value, ssl.SSLSocket):
            value = self.context.wrap_socket(value, server_hostname=self.host)
        self._sock = value


def _finish(ftp, conn, strict):
    if isinstance(conn, ssl.SSLSocket):
        try:
            conn.unwrap()
        except (OSError, ssl.SSLError):
            pass
    conn.close()
    try:
        ftp.voidresp()
    except ftplib.all_errors:
        if strict:
            raise


class _FtpWriter:
    def __init__(self, ftp, tmp, final):
        self._ftp, self._tmp, self._final = ftp, tmp, final
        self._conn = ftp.transfercmd("STOR " + tmp)

    def write(self, data):
        self._conn.sendall(data)
        return len(data)

    def flush(self):
        pass

    def close(self):
        try:
            _finish(self._ftp, self._conn, strict=True)
            self._ftp.rename(self._tmp, self._final)
        except ftplib.all_errors as e:
            raise TargetError(f"FTP: не удалось записать файл: {e}")

    def abort(self):
        try:
            self._conn.close()
            try:
                self._ftp.voidresp()
            except ftplib.all_errors:
                pass
            self._ftp.delete(self._tmp)
        except ftplib.all_errors:
            pass


class _FtpReader:
    def __init__(self, ftp, name):
        self._ftp = ftp
        self._conn = ftp.transfercmd("RETR " + name)

    def read(self, n=-1):
        if n is None or n < 0:
            out = bytearray()
            while True:
                d = self.read(1 << 20)
                if not d:
                    return bytes(out)
                out += d
        return self._conn.recv(n)

    def close(self):
        _finish(self._ftp, self._conn, strict=False)


class FtpTarget(Target):
    """tls: 'none' | 'explicit' | 'implicit'."""

    kind = "ftp"

    def __init__(self, host, port=None, user="anonymous", password="", directory="/",
                 tls="none", verify=True, ca_file=None, passive=True, timeout=30):
        if tls not in ("none", "explicit", "implicit"):
            raise ValueError("tls должен быть none, explicit или implicit")
        self.host, self.user, self.password = host, user, password
        self.port = port or (990 if tls == "implicit" else 21)
        self.directory, self.tls, self.passive, self.timeout = directory, tls, passive, timeout
        self.verify, self.ca_file = verify, ca_file
        self._ftp = None

    def _context(self):
        if self.verify:
            return ssl.create_default_context(cafile=self.ca_file)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    def connect(self):
        try:
            if self.tls == "none":
                ftp = ftplib.FTP(timeout=self.timeout)
            else:
                cls = ftplib.FTP_TLS if self.tls == "explicit" else _ImplicitFTPTLS
                ftp = cls(context=self._context(), timeout=self.timeout)
            ftp.connect(self.host, self.port)
            ftp.login(self.user, self.password)
            if self.tls != "none":
                ftp.prot_p()  # данные тоже шифруются
            ftp.set_pasv(self.passive)
            self._mkdirs(ftp, self.directory)
            self._ftp = ftp
        except ssl.SSLError as e:
            raise TargetError(f"FTP: ошибка TLS: {e}")
        except ftplib.all_errors as e:
            raise TargetError(f"FTP: {e}")
        return self

    @staticmethod
    def _mkdirs(ftp, directory):
        if directory.startswith("/"):
            ftp.cwd("/")
        for part in [p for p in directory.split("/") if p]:
            try:
                ftp.cwd(part)
            except ftplib.error_perm:
                ftp.mkd(part)
                ftp.cwd(part)

    def close(self):
        if self._ftp:
            try:
                self._ftp.quit()
            except Exception:
                self._ftp.close()
            self._ftp = None

    def open_write(self, name):
        return _FtpWriter(self._ftp, name + ".part", name)

    def open_read(self, name):
        try:
            return _FtpReader(self._ftp, name)
        except ftplib.all_errors as e:
            raise TargetError(f"FTP: {e}")

    def list(self):
        try:
            names = self._ftp.nlst()
        except ftplib.error_perm:
            return []
        return sorted(n for n in (posixpath.basename(x) for x in names)
                      if n and not n.endswith(".part") and n not in (".", ".."))

    def delete(self, name):
        try:
            self._ftp.delete(name)
        except ftplib.error_perm:
            pass
