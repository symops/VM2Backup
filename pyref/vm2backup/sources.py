"""Источники виртуальных машин."""
import io
import json
import os
import shutil
import subprocess
import tarfile
from dataclasses import dataclass


class SourceError(Exception):
    pass


@dataclass
class VmInfo:
    name: str
    state: str = "unknown"
    size: int = 0


def _add_bytes(tar, arcname, data):
    ti = tarfile.TarInfo(arcname)
    ti.size = len(data)
    ti.mtime = int(__import__("time").time())
    tar.addfile(ti, io.BytesIO(data))


class DirectorySource:
    """Каталог с ВМ: каждая вложенная папка - одна машина (VMware, VirtualBox, Hyper-V-экспорт и т.п.).

    Подходит и для датасторов, смонтированных по NFS/SMB. Копировать работающую ВМ нельзя,
    если в папке есть файл блокировки (*.lck) - защита от неконсистентной копии.
    """

    kind = "directory"

    def __init__(self, root, allow_live=False):
        self.root, self.allow_live = os.path.abspath(root), allow_live

    def _dir(self, name):
        p = os.path.join(self.root, name)
        if os.path.basename(name) != name or not os.path.isdir(p):
            raise SourceError(f"ВМ не найдена: {name}")
        return p

    def _locked(self, p):
        return any(n.endswith(".lck") for _, ds, fs in os.walk(p) for n in fs + ds)

    def list_vms(self):
        out = []
        for n in sorted(os.listdir(self.root)):
            p = os.path.join(self.root, n)
            if os.path.isdir(p):
                size = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(p) for f in fs)
                out.append(VmInfo(n, "running" if self._locked(p) else "stopped", size))
        return out

    def pack(self, vm, tar, progress=None):
        p = self._dir(vm)
        if self._locked(p) and not self.allow_live:
            raise SourceError(f"ВМ {vm} запущена (найден файл блокировки). Остановите её или разрешите живую копию.")
        meta = {"source": "directory", "vm": vm}
        _add_bytes(tar, "vm2backup-meta.json", json.dumps(meta).encode())
        for d, ds, fs in os.walk(p):
            ds.sort()
            for f in sorted(fs):
                full = os.path.join(d, f)
                if f.endswith(".lck"):
                    continue
                tar.add(full, arcname=os.path.join(vm, os.path.relpath(full, p)), recursive=False)
                if progress:
                    progress(os.path.getsize(full))


class LibvirtSource:
    """KVM/QEMU через virsh: XML-описание + файловые диски. Бэкап запущенной ВМ - только с allow_live."""

    kind = "libvirt"

    def __init__(self, uri="qemu:///system", allow_live=False, virsh="virsh"):
        self.uri, self.allow_live, self.virsh = uri, allow_live, virsh

    def _v(self, *args):
        try:
            r = subprocess.run([self.virsh, "-c", self.uri, *args], capture_output=True, text=True, timeout=120)
        except FileNotFoundError:
            raise SourceError("virsh не найден (установите libvirt-clients)")
        if r.returncode:
            raise SourceError(f"virsh {' '.join(args)}: {r.stderr.strip()}")
        return r.stdout

    def _disks(self, vm):
        disks = []
        for line in self._v("domblklist", vm, "--details").splitlines()[2:]:
            parts = line.split(None, 3)
            if len(parts) == 4 and parts[0] == "file" and parts[1] == "disk" and os.path.isabs(parts[3]):
                disks.append(parts[3].strip())
        return disks

    def list_vms(self):
        out = []
        for n in self._v("list", "--all", "--name").split():
            state = self._v("domstate", n).strip()
            size = sum(os.path.getsize(d) for d in self._disks(n) if os.path.exists(d))
            out.append(VmInfo(n, state, size))
        return out

    def pack(self, vm, tar, progress=None):
        state = self._v("domstate", vm).strip()
        if state != "shut off" and not self.allow_live:
            raise SourceError(f"ВМ {vm} в состоянии «{state}». Выключите её или разрешите живую копию.")
        disks = self._disks(vm)
        meta = {"source": "libvirt", "vm": vm, "state": state,
                "disks": {os.path.basename(d): d for d in disks}}
        _add_bytes(tar, "vm2backup-meta.json", json.dumps(meta).encode())
        _add_bytes(tar, f"{vm}/domain.xml", self._v("dumpxml", vm).encode())
        for d in disks:
            tar.add(d, arcname=f"{vm}/disks/{os.path.basename(d)}", recursive=False)
            if progress:
                progress(os.path.getsize(d))

    def finish_restore(self, dest, vm, define=True, original_paths=False, force=False):
        """Опционально возвращает диски на прежние пути и регистрирует ВМ (virsh define)."""
        base = os.path.join(dest, vm)
        with open(os.path.join(dest, "vm2backup-meta.json")) as f:
            meta = json.load(f)
        if original_paths:
            for name, orig in meta["disks"].items():
                if os.path.exists(orig) and not force:
                    raise SourceError(f"{orig} уже существует (используйте force)")
                os.makedirs(os.path.dirname(orig), exist_ok=True)
                shutil.copyfile(os.path.join(base, "disks", name), orig)
        if define:
            self._v("define", os.path.join(base, "domain.xml"))
