"""Шифрование архива паролем: AES-256-GCM по блокам, ключ из scrypt.

Формат: MAGIC | salt(16) | nonce_prefix(4) | блоки [len(4) | ciphertext+tag].
Номер блока и признак последнего входят в nonce/AAD: перестановка, удаление
и обрезка потока обнаруживаются при расшифровке.
"""
import os
import struct

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt

MAGIC = b"V2BENC1\n"
CHUNK = 1 << 20


class DecryptError(Exception):
    pass


def _key(password, salt):
    return Scrypt(salt=salt, length=32, n=2 ** 15, r=8, p=1).derive(password.encode("utf-8"))


def _nonce(prefix, index):
    return prefix + struct.pack(">Q", index)


class EncryptWriter:
    def __init__(self, sink, password):
        self.sink = sink
        salt, self._prefix = os.urandom(16), os.urandom(4)
        self._aes = AESGCM(_key(password, salt))
        self._index = 0
        self._buf = bytearray()
        sink.write(MAGIC + salt + self._prefix)

    def _emit(self, data, last):
        ct = self._aes.encrypt(_nonce(self._prefix, self._index), bytes(data), b"\x01" if last else b"\x00")
        self.sink.write(struct.pack(">I", len(ct)) + ct)
        self._index += 1

    def write(self, data):
        self._buf += data
        # последний блок всегда остаётся в буфере до close()
        while len(self._buf) > CHUNK:
            self._emit(self._buf[:CHUNK], False)
            del self._buf[:CHUNK]
        return len(data)

    def flush(self):
        pass

    def close(self):
        self._emit(self._buf, True)
        self._buf.clear()


class DecryptReader:
    def __init__(self, src, password):
        self.src = src
        head = _read_exact(src, len(MAGIC) + 20)
        if head[: len(MAGIC)] != MAGIC:
            raise DecryptError("это не зашифрованный архив VM2Backup")
        salt = head[len(MAGIC): len(MAGIC) + 16]
        self._prefix = head[len(MAGIC) + 16:]
        self._aes = AESGCM(_key(password, salt))
        self._index = 0
        self._buf = b""
        self._done = False

    def _next_block(self):
        hdr = self.src.read(4)
        if len(hdr) < 4:
            raise DecryptError("архив обрезан")
        (n,) = struct.unpack(">I", hdr)
        ct = _read_exact(self.src, n)
        # сначала пробуем как промежуточный блок, затем как последний
        for last in (False, True):
            try:
                pt = self._aes.decrypt(_nonce(self._prefix, self._index), ct, b"\x01" if last else b"\x00")
                break
            except InvalidTag:
                continue
        else:
            raise DecryptError("неверный пароль или архив повреждён")
        self._index += 1
        self._done = last
        return pt

    def read(self, n=-1):
        while (n < 0 or len(self._buf) < n) and not self._done:
            self._buf += self._next_block()
        if n < 0:
            out, self._buf = self._buf, b""
        else:
            out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def close(self):
        pass


def _read_exact(src, n):
    out = b""
    while len(out) < n:
        part = src.read(n - len(out))
        if not part:
            raise DecryptError("архив обрезан")
        out += part
    return out
