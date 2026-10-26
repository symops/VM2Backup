import os
from urllib.parse import parse_qs, unquote, urlparse

from .base import Target, TargetError
from .ftp import FtpTarget
from .local import LocalTarget
from .ssh import ScpTarget

__all__ = ["Target", "TargetError", "LocalTarget", "FtpTarget", "ScpTarget",
           "target_from_url", "target_from_config"]


def _bool(v):
    return str(v).lower() not in ("0", "false", "no", "off")


def target_from_config(c):
    """c: словарь вида {'type': 'ftp'|'ftpes'|'ftps'|'scp'|'local', ...}."""
    t = c["type"]
    if t == "local":
        return LocalTarget(c["path"])
    if t in ("ftp", "ftpes", "ftps"):
        tls = {"ftp": "none", "ftpes": "explicit", "ftps": "implicit"}[t]
        return FtpTarget(c["host"], c.get("port"), c.get("user", "anonymous"), c.get("password", ""),
                         c.get("directory", "/"), tls=tls, verify=c.get("verify", True),
                         ca_file=c.get("ca_file"), passive=c.get("passive", True))
    if t == "sftp":
        raise ValueError("SFTP больше не поддерживается, создайте SCP-сервер")
    if t == "scp":
        return ScpTarget(c["host"], c.get("port", 22), c.get("user"), c.get("password"), c.get("key_file"),
                   c.get("key_passphrase"), c.get("directory", "."), c.get("host_key_sha256"),
                   c.get("known_hosts"), c.get("insecure_auto_add", False))
    raise ValueError(f"неизвестный тип хранилища: {t}")


def target_from_url(url):
    """Путь, file://, ftp://, ftpes:// (явный TLS), ftps:// (неявный TLS), scp://.

    Параметры: ?verify=0 &ca=файл &key=файл &hostkey=SHA256:... &insecure=1
    Пароль берётся из URL или переменной окружения VM2B_TARGET_PASSWORD.
    """
    u = urlparse(url)
    if u.scheme in ("", "file"):
        return LocalTarget(unquote(u.path) if u.scheme else url)
    q = {k: v[-1] for k, v in parse_qs(u.query).items()}
    cfg = {"type": u.scheme, "host": u.hostname, "port": u.port, "user": unquote(u.username or "") or None,
           "password": unquote(u.password) if u.password else os.environ.get("VM2B_TARGET_PASSWORD"),
           "directory": unquote(u.path) or ("/" if u.scheme.startswith("ftp") else ".")}
    if u.scheme.startswith("ftp"):
        cfg["user"] = cfg["user"] or "anonymous"
        cfg["password"] = cfg["password"] or ""
        cfg["verify"] = _bool(q.get("verify", "1"))
        cfg["ca_file"] = q.get("ca")
    else:
        cfg["key_file"] = q.get("key")
        cfg["host_key_sha256"] = q.get("hostkey")
        cfg["insecure_auto_add"] = _bool(q.get("insecure", "0"))
    if cfg["port"] is None:
        del cfg["port"]
    return target_from_config(cfg)
