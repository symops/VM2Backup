import os

from .base import Target, TargetError


class _LocalWriter:
    def __init__(self, tmp, final):
        self._tmp, self._final = tmp, final
        self._f = open(tmp, "wb")

    def write(self, data):
        return self._f.write(data)

    def flush(self):
        pass

    def close(self):
        self._f.flush()
        os.fsync(self._f.fileno())
        self._f.close()
        os.replace(self._tmp, self._final)

    def abort(self):
        try:
            self._f.close()
        finally:
            if os.path.exists(self._tmp):
                os.remove(self._tmp)


class LocalTarget(Target):
    kind = "local"

    def __init__(self, path):
        self.path = os.path.abspath(path)

    def connect(self):
        os.makedirs(self.path, exist_ok=True)
        return self

    def _p(self, name):
        if os.path.basename(name) != name:
            raise TargetError(f"недопустимое имя файла: {name!r}")
        return os.path.join(self.path, name)

    def open_write(self, name):
        return _LocalWriter(self._p(name) + ".part", self._p(name))

    def open_read(self, name):
        try:
            return open(self._p(name), "rb")
        except OSError as e:
            raise TargetError(str(e))

    def list(self):
        return sorted(n for n in os.listdir(self.path) if not n.endswith(".part"))

    def delete(self, name):
        try:
            os.remove(self._p(name))
        except FileNotFoundError:
            pass
