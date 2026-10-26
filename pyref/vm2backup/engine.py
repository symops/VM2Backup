"""Движок: пакует ВМ (tar) -> сжатие -> шифрование -> хранилище, проверка, восстановление, ротация."""
import hashlib
import json
import os
import re
import tarfile
import time
from dataclasses import dataclass

from . import compression
from .crypto import DecryptReader, EncryptWriter
from .targets.base import TargetError


class IntegrityError(Exception):
    pass


class _HashWriter:
    def __init__(self, sink):
        self.sink, self.sha, self.size = sink, hashlib.sha256(), 0

    def write(self, data):
        self.sha.update(data)
        self.size += len(data)
        self.sink.write(data)
        return len(data)

    def flush(self):
        pass


class _HashReader:
    def __init__(self, src):
        self.src, self.sha, self.size = src, hashlib.sha256(), 0

    def read(self, n=-1):
        d = self.src.read(n)
        self.sha.update(d)
        self.size += len(d)
        return d

    def drain(self):
        while self.read(1 << 20):
            pass


@dataclass
class BackupResult:
    name: str
    manifest: dict


def _safe(name):
    return re.sub(r"[^\w.-]+", "_", name)


def archive_name(vm, compression_algo, encrypted, when=None):
    ts = time.strftime("%Y%m%d-%H%M%S", time.localtime(when))
    return f"{_safe(vm)}_{ts}.tar{compression.EXTENSIONS[compression_algo]}{'.enc' if encrypted else ''}"


def backup(source, vm, target, compression_algo="zstd", level=None, password=None, keep=0,
           verify=True, progress=None):
    if compression_algo not in compression.ALGORITHMS:
        raise ValueError(f"неизвестный алгоритм сжатия: {compression_algo}")
    name = archive_name(vm, compression_algo, bool(password))
    if target.exists(name):
        name = name.replace(".tar", f".{os.getpid()}.tar", 1)
    writer = target.open_write(name)
    stats = {"raw": 0}
    try:
        hasher = _HashWriter(writer)
        top = hasher
        enc = comp = None
        if password:
            top = enc = EncryptWriter(top, password)
        if compression_algo != "none":
            top = comp = compression.CompressWriter(top, compression_algo, level)
        tar = tarfile.open(fileobj=top, mode="w|", format=tarfile.PAX_FORMAT)
        source.pack(vm, tar, progress)
        tar.close()
        if comp:
            comp.close()
        if enc:
            enc.close()
    except BaseException:
        writer.abort()
        raise
    writer.close()
    manifest = {"version": 1, "vm": vm, "archive": name, "created": time.time(),
                "compression": compression_algo, "encrypted": bool(password),
                "size": hasher.size, "sha256": hasher.sha.hexdigest(), "source": source.kind}
    target.put_bytes(name + ".json", json.dumps(manifest, indent=1).encode())
    if verify:
        try:
            verify_backup(target, name, password)
        except Exception:
            target.delete(name)
            target.delete(name + ".json")
            raise
    if keep:
        prune(target, vm, keep)
    return BackupResult(name, manifest)


def list_backups(target, vm=None):
    out = []
    for n in target.list():
        if n.endswith(".json"):
            try:
                m = json.loads(target.get_bytes(n))
            except (ValueError, TargetError):
                continue
            if m.get("archive") and (vm is None or m.get("vm") == vm):
                out.append(m)
    return sorted(out, key=lambda m: m["created"])


def _manifest(target, name):
    try:
        return json.loads(target.get_bytes(name + ".json"))
    except (ValueError, TargetError) as e:
        raise IntegrityError(f"нет манифеста для {name}: {e}")


def _open_chain(target, name, manifest, password):
    raw = target.open_read(name)
    hr = _HashReader(raw)
    top = hr
    if manifest["encrypted"]:
        if not password:
            raise IntegrityError("архив зашифрован, нужен пароль")
        top = DecryptReader(top, password)
    if manifest["compression"] != "none":
        top = compression.DecompressReader(top, manifest["compression"])
    return raw, hr, top


def verify_backup(target, name, password=None):
    """Проверяет SHA-256 и размер; при наличии пароля (или без шифрования) читает весь tar целиком."""
    m = _manifest(target, name)
    deep = (not m["encrypted"]) or bool(password)
    raw, hr, top = _open_chain(target, name, m, password if deep else None) if deep else (None, None, None)
    if not deep:
        raw = target.open_read(name)
        hr = top = _HashReader(raw)
    try:
        if deep:
            with tarfile.open(fileobj=top, mode="r|") as tar:
                for _ in tar:
                    pass
        hr.drain()
    finally:
        raw.close()
    if hr.size != m["size"] or hr.sha.hexdigest() != m["sha256"]:
        raise IntegrityError(f"{name}: контрольная сумма не совпала")
    return m


def restore(target, name, dest, password=None):
    m = _manifest(target, name)
    os.makedirs(dest, exist_ok=True)
    raw, hr, top = _open_chain(target, name, m, password)
    try:
        with tarfile.open(fileobj=top, mode="r|") as tar:
            tar.extractall(dest, filter="data")
        hr.drain()
    finally:
        raw.close()
    if hr.size != m["size"] or hr.sha.hexdigest() != m["sha256"]:
        raise IntegrityError(f"{name}: контрольная сумма не совпала, восстановленным данным доверять нельзя")
    return m


def delete_backup(target, name):
    target.delete(name)
    target.delete(name + ".json")


def prune(target, vm, keep):
    old = list_backups(target, vm)[:-keep] if keep > 0 else []
    for m in old:
        delete_backup(target, m["archive"])
    return [m["archive"] for m in old]
