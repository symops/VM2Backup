"""Потоковое сжатие и распаковка: none, gzip, xz, zstd."""
import lzma
import zlib

import zstandard

ALGORITHMS = ("none", "gzip", "xz", "zstd")
EXTENSIONS = {"none": "", "gzip": ".gz", "xz": ".xz", "zstd": ".zst"}


def _compressor(algo, level):
    if algo == "gzip":
        return zlib.compressobj(level if level is not None else 6, zlib.DEFLATED, 31)
    if algo == "xz":
        return lzma.LZMACompressor(format=lzma.FORMAT_XZ, preset=level if level is not None else 6)
    if algo == "zstd":
        return zstandard.ZstdCompressor(level=level if level is not None else 3).compressobj()
    raise ValueError(f"неизвестный алгоритм сжатия: {algo}")


def _decompressor(algo):
    if algo == "gzip":
        return zlib.decompressobj(31)
    if algo == "xz":
        return lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    if algo == "zstd":
        return zstandard.ZstdDecompressor().decompressobj()
    raise ValueError(f"неизвестный алгоритм сжатия: {algo}")


class CompressWriter:
    """write()/close() -> сжатые данные уходят в sink; sink закрывается вызывающим."""

    def __init__(self, sink, algo, level=None):
        self.sink = sink
        self._c = _compressor(algo, level)
        self.raw_bytes = 0

    def write(self, data):
        self.raw_bytes += len(data)
        out = self._c.compress(data)
        if out:
            self.sink.write(out)
        return len(data)

    def flush(self):
        pass

    def close(self):
        out = self._c.flush()
        if out:
            self.sink.write(out)


class DecompressReader:
    """read(n) -> распакованные данные из источника."""

    def __init__(self, src, algo, chunk=1 << 20):
        self.src = src
        self._d = _decompressor(algo)
        self._chunk = chunk
        self._buf = b""
        self._eof = False

    def read(self, n=-1):
        while (n < 0 or len(self._buf) < n) and not self._eof:
            data = self.src.read(self._chunk)
            if not data:
                self._eof = True
                break
            self._buf += self._d.decompress(data)
        if n < 0:
            out, self._buf = self._buf, b""
        else:
            out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def close(self):
        pass
