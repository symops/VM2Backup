"""Общий интерфейс хранилищ бэкапов."""


class TargetError(Exception):
    pass


class Target:
    """Хранилище: плоский каталог с файлами. Запись атомарна: имя.part -> имя при close()."""

    kind = "base"

    def connect(self):
        return self

    def close(self):
        pass

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    def open_write(self, name):
        """-> объект с write(bytes), close() (фиксирует файл) и abort() (удаляет недописанное)."""
        raise NotImplementedError

    def open_read(self, name):
        """-> объект с read(n), close()."""
        raise NotImplementedError

    def list(self):
        raise NotImplementedError

    def delete(self, name):
        raise NotImplementedError

    def exists(self, name):
        return name in self.list()

    def put_bytes(self, name, data):
        w = self.open_write(name)
        try:
            w.write(data)
        except BaseException:
            w.abort()
            raise
        w.close()

    def get_bytes(self, name):
        r = self.open_read(name)
        try:
            out = bytearray()
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    return bytes(out)
                out += chunk
        finally:
            r.close()
