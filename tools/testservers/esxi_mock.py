#!/usr/bin/env python3
"""A VMware ESXi / vCenter emulator for the VM2Backup autotests (python3 stdlib + `cryptography` for the test certificate).

  python3 -I esxi_mock.py --profile 7.0 --kind esxi|vcenter --port N --root DIR --user U --password P --info FILE [--tls-only 1.0] [--dcs DC1,DC2]

Emulates (for the version PROFILE 6.0 / 6.5 / 6.7 / 7.0 / 8.0 / 9.0, as a standalone host or as a vCenter): the vSphere Web Services SOAP API at /sdk
(RetrieveServiceContent, Login / Logout, CreateContainerView / DestroyView, RetrievePropertiesEx / ContinueRetrievePropertiesEx / RetrieveProperties,
CreateSnapshot_Task, RemoveSnapshot_Task, CancelTask, RefreshStorageInfo, task polling), session cookies, and the datastore file access
GET / HEAD / PUT /folder/<path>?dcPath=..&dsName=.. (Range support) over a directory tree  ROOT/<datastore>/<vm folder>/<files>.
Snapshot semantics: after CreateSnapshot the base flat file is frozen and a -delta / -sesparse file receives "guest writes" (control call);
RemoveSnapshot consolidates them into the lower level.  Faults on demand (see /__mock/faults), a record of every call and of the live
snapshots (/__mock/state), so a test can assert "no snapshot leaked".  Control API (JSON, no authentication) under /__mock/.
What the real products do that is NOT verified here is listed in docs/ESXI.md: the XML below is written from the vSphere API reference.
"""
import posixpath, argparse, re, base64, datetime, hashlib, http.server, json, os, random, socket, socketserver, ssl, sys, threading, time, uuid
import urllib.parse
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape as xesc

PROFILES = {
    # esxi version, build, apiVersion, vcenter version, vcenter build, tls (min, max), delta file kind, capacityInBytes present, min SOAP version accepted
    '6.0': dict(ev='6.0.0', eb='3620759', api='6.0', cv='6.0.0', cb='3634793', tls=('1.0', '1.2'), delta='delta', cib=False, minsoap='6.0'),
    '6.5': dict(ev='6.5.0', eb='7967591', api='6.5', cv='6.5.0', cb='7312210', tls=('1.0', '1.2'), delta='delta', cib=False, minsoap='6.0'),
    '6.7': dict(ev='6.7.0', eb='17700523', api='6.7.3', cv='6.7.0', cb='20051473', tls=('1.2', '1.2'), delta='sesparse', cib=True, minsoap='6.0'),
    '7.0': dict(ev='7.0.3', eb='21686933', api='7.0.3.0', cv='7.0.3', cb='22837322', tls=('1.2', '1.2'), delta='sesparse', cib=True, minsoap='6.0'),
    '8.0': dict(ev='8.0.2', eb='22380479', api='8.0.2.0', cv='8.0.2', cb='22617221', tls=('1.2', '1.3'), delta='sesparse', cib=True, minsoap='6.0'),
    '9.0': dict(ev='9.0.0', eb='24000000', api='9.0.0.0', cv='9.0.0', cb='24100000', tls=('1.2', '1.3'), delta='sesparse', cib=True, minsoap='6.5'),
}
NS_ENV = ('xmlns:soapenc="http://schemas.xmlsoap.org/soap/encoding/" xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" '
          'xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"')
LOCK = threading.RLock()


def verkey(s):
    return tuple(int(x) for x in s.split('.') if x.isdigit())


class Fault(Exception):
    def __init__(self, ftype, msg, http=500):
        self.ftype, self.msg, self.http = ftype, msg, http


class S:
    """the whole state of the emulated server"""
    prof = None; kind = 'esxi'; root = ''; user = 'root'; password = 'pw'; dcs = []
    vms = {}; sessions = {}; views = {}; tasks = {}; tokens = {}
    ids = {'vm': 100, 'task': 0, 'snap': 0, 'sess': 0, 'view': 0, 'key': 0, 'tok': 0}
    log = []; counters = {}; events = []; faults = {}; soap_calls = 0; ranges = []
    cert_fp = ''
    dead = False; evn = 0
    inv = {}


def nid(kind):
    S.ids[kind] += 1
    return S.ids[kind]


def record(kind, what, extra=''):
    with LOCK:
        S.log.append(dict(n=len(S.log), t=round(time.time(), 3), kind=kind, what=what, extra=extra))
        if len(S.log) > 50000:
            del S.log[:10000]
        key = kind + ':' + what
        S.counters[key] = S.counters.get(key, 0) + 1


def tick_event():
    """the power-loss switch: after `die_after` events (SOAP calls, file requests, 256 KiB blocks) the server stops answering until /__mock/revive"""
    with LOCK:
        S.evn += 1
        d = S.faults.get('die_after')
        if d and not S.dead and S.evn >= d:
            S.dead = True
            S.faults.pop('die_after', None)
            S.log.append(dict(n=len(S.log), t=round(time.time(), 3), kind='fault', what='died', extra=str(S.evn)))
        return S.dead


def fault_get(name, default=None):
    return S.faults.get(name, default)


def fault_take(name):
    """a counted fault: value is an int count (or -1 = always); returns True and decrements when it fires"""
    with LOCK:
        v = S.faults.get(name)
        if not v:
            return False
        if v > 0:
            S.faults[name] = v - 1
        return True


# ---------------------------------------------------------------------------------------------------------------- the datastore / VMs
class Level:
    def __init__(self, desc, ext, base):
        self.desc, self.ext, self.base, self.writes = desc, ext, base, []


class Disk:
    def __init__(self, key, label, cap, thin, name):
        self.key, self.label, self.cap, self.thin, self.name = key, label, cap, thin, name
        self.ds = None
        self.levels = []          # base first
        self.snapn = 0


class Snap:
    def __init__(self, sid, ref, name, desc, quiesced, state, ctime):
        self.id, self.ref, self.name, self.desc, self.quiesced, self.state, self.ctime = sid, ref, name, desc, quiesced, state, ctime
        self.vmsn = None; self.vmem = None
        self.parent = None; self.chains = []   # the snapshot this one is a child of; per disk the frozen chain (base first) at creation


class VM:
    pass


def ds_dir(ds):
    return os.path.join(S.root, ds)


def file_ds(vm, name):
    return vm.files.get(name, {}).get('ds') or vm.ds


def vm_path(vm, name=''):
    return os.path.join(ds_dir(file_ds(vm, name) if name else vm.ds), vm.dir, name) if name else os.path.join(ds_dir(vm.ds), vm.dir)


def register(vm, name, ftype, ds=None):
    if name not in vm.files:
        vm.files[name] = dict(key=nid('key'), type=ftype, ds=ds)
        os.makedirs(os.path.dirname(os.path.join(ds_dir(ds or vm.ds), vm.dir, name)), exist_ok=True)     # a name '../other/x.vmdk' lives in another folder of the datastore
    return vm.files[name]['key']


def unregister(vm, name):
    p = vm_path(vm, name)
    vm.files.pop(name, None)
    try:
        os.remove(p)
    except OSError:
        pass


def vmx_text(vm):
    t = '.encoding = "UTF-8"\nconfig.version = "8"\ndisplayName = "%s"\nuuid.bios = "%s"\nnvram = "%s.nvram"\nctkEnabled = "TRUE"\n' % (vm.name, vm.uuid, vm.vmx[:-4])
    t += 'scsi0.present = "TRUE"\nscsi0.virtualDev = "pvscsi"\n'
    for i, d in enumerate(vm.disks):
        top = d.levels[-1]
        fn = top.desc if (not d.ds or d.ds == vm.ds) and '/' not in top.desc else '[%s] %s' % (d.ds or vm.ds, posixpath.normpath(vm.dir + '/' + top.desc))
        t += 'scsi0:%d.present = "TRUE"\nscsi0:%d.fileName = "%s"\nscsi0:%d.ctkEnabled = "TRUE"\n' % (i, i, fn, i)
    if vm.snaps:
        t += 'snapshot.redoNotWithParent = "TRUE"\n'
    for x in vm.vmx_extra:
        t += x + '\n'
    return t


def write_vmx(vm):
    with open(vm_path(vm, vm.vmx), 'wb') as f:
        f.write(vmx_text(vm).encode('utf-8'))


def write_text(vm, name, text, ftype=None, ds=None):
    if ftype:
        register(vm, name, ftype, ds)
    with open(vm_path(vm, name), 'wb') as f:
        f.write(text.encode('utf-8'))


def descriptor_text(disk, level, parent):
    return ('# Disk DescriptorFile\nversion=1\nencoding="UTF-8"\nCID=%08x\nparentCID=%s\ncreateType="%s"\n%s\n# Extent description\nRW %d %s "%s"\n'
            % (random.getrandbits(32), 'ffffffff' if parent is None else '%08x' % random.getrandbits(32),
               ('vmfsPassthroughRawDeviceMap' if getattr(disk, 'rdm', None) == 'physical' else 'vmfsRawDeviceMap' if getattr(disk, 'rdm', None) else 'vmfs') if level.base else 'vmfsSparse',
               ('' if parent is None else 'parentFileNameHint="%s"' % posixpath.basename(parent)), disk.cap // 512, 'VMFS' if level.base else 'VMFSSPARSE', posixpath.basename(level.ext)))


def set_sparse(f):
    """Windows (эмулятор внутри гостя): без атрибута "разреженный" truncate до 1 ГиБ реально пишет гигабайт нулей (около минуты, клиент управления не дожидается)."""
    if os.name != 'nt':
        return
    try:
        import ctypes, msvcrt
        br = ctypes.c_ulong(0)
        ctypes.windll.kernel32.DeviceIoControl(ctypes.c_void_p(msvcrt.get_osfhandle(f.fileno())), 0x900C4, None, 0, None, 0, ctypes.byref(br), None)      # FSCTL_SET_SPARSE
    except Exception:
        pass


def extend_sparse(f, size):
    """Увеличить файл до size без записи данных. На Windows truncate() CRT ДОПИСЫВАЕТ нули (минута на ГиБ), а запись последнего байта в разреженный файл оставляет дыру."""
    if os.name != 'nt':
        f.truncate(size)
        return
    set_sparse(f)
    f.seek(size - 1); f.write(b'\0'); f.flush()


def fill_flat(path, size, thin, seed, written=None):
    rnd = random.Random(seed)
    with open(path, 'wb') as f:
        if thin and size > (64 << 20):
            extend_sparse(f, size)
            for _ in range(rnd.randint(3, 8)):
                off = rnd.randrange(0, size - (1 << 20)) & ~511
                f.seek(off)
                f.write(rnd.randbytes(rnd.randint(1000, 600000)))
            f.seek(0); f.write(rnd.randbytes(65536))
            f.seek(size - 4096); f.write(rnd.randbytes(4096))
        else:
            left = size
            while left > 0:
                n = min(left, 1 << 20)
                kind = rnd.randrange(3)
                if kind == 0:
                    f.write(b'\0' * n)
                elif kind == 1:
                    f.write(rnd.randbytes(n))
                else:
                    f.write((b'esxi vmdk block %d ' % rnd.randrange(50) * (n // 16 + 1))[:n])
                left -= n


def create_vm(a):
    with LOCK:
        name = a['name']
        dc = a.get('dc') or S.dcs[0][1]
        ds = a.get('ds') or 'datastore1'
        vm = VM()
        vm.id = ('%d' % nid('vm')) if S.kind == 'esxi' else 'vm-%d' % nid('vm')
        vm.name = name; vm.dc = dc; vm.ds = ds; vm.dir = a.get('dir', name)
        vm.uuid = str(uuid.UUID(int=random.Random(a.get('seed', 1) * 7 + len(name)).getrandbits(128)))
        vm.inst = str(uuid.uuid4()); vm.power = a.get('power', 'poweredOff'); vm.template = bool(a.get('template', False))
        vm.conn = a.get('conn', 'connected'); vm.guest = a.get('guest', 'Other Linux (64-bit)')
        vm.cpu = a.get('cpu', 2); vm.mem = a.get('mem', 1024)
        vm.files = {}; vm.disks = []; vm.snaps = []; vm.vmx = (a.get('vmx') or name) + '.vmx'
        vm.layout_lag = 0
        vm.encrypted = bool(a.get('encrypted', False)); vm.cdrom = a.get('cdrom')
        os.makedirs(vm_path(vm), exist_ok=True)
        vm.vmx_extra = []
        register(vm, vm.vmx, 'config')
        write_text(vm, (a.get('vmx') or name) + '.vmxf', '<?xml version="1.0"?><Foundry><VM><VMId type="string">%s</VMId></VM></Foundry>' % vm.uuid, 'extendedConfig')
        base = a.get('vmx') or name
        with open(vm_path(vm, base + '.nvram'), 'wb') as f:
            f.write(random.Random(a.get('seed', 1)).randbytes(8684))
        register(vm, base + '.nvram', 'nvram')
        write_text(vm, base + '.vmsd', '.encoding = "UTF-8"\nsnapshot.lastUID = "0"\nsnapshot.numSnapshots = "0"\n', 'snapshotList')
        write_text(vm, 'vmware.log', 'log of %s\n' % name * 20, 'log')
        with open(vm_path(vm, base + '-%08x.vswp' % random.getrandbits(32)), 'wb') as f:
            f.truncate(vm.mem << 20 if vm.mem < 64 else 4 << 20)
        for fn in os.listdir(vm_path(vm)):
            if fn.endswith('.vswp'):
                register(vm, fn, 'swap')
        for i, d in enumerate(a.get('disks', [{'size': 1 << 20}])):
            dn = ('../%s/' % d['dir'] if d.get('dir') else '') + (d.get('fname') or (base if i == 0 else '%s_%d' % (base, i)))     # dir: the disk lives in another folder of its datastore
            disk = Disk(2000 + i, 'Hard disk %d' % (i + 1), int(d['size']), bool(d.get('thin', False)), dn)
            disk.ds = d.get('ds')                       # a disk on another datastore than the VM's folder
            disk.indep = bool(d.get('indep'))           # independent-persistent: not covered by snapshots
            disk.rdm = d.get('rdm')                     # 'physical' | 'virtual': a pointer file, the LUN is not readable over HTTP
            lv = Level(dn + '.vmdk', dn + ('-rdmp.vmdk' if disk.rdm == 'physical' else '-rdm.vmdk' if disk.rdm else '-flat.vmdk'), True)
            register(vm, lv.ext, 'diskExtent', disk.ds)
            if disk.rdm:
                with open(vm_path(vm, lv.ext), 'wb') as f:
                    f.truncate(disk.cap)
            else:
                fill_flat(vm_path(vm, lv.ext), disk.cap, disk.thin, d.get('seed', 5 + i))
            if d.get('ctk'):
                with open(vm_path(vm, dn + '-ctk.vmdk'), 'wb') as f:
                    f.write(random.Random(i).randbytes(max(512, disk.cap // 1000)))
                register(vm, dn + '-ctk.vmdk', 'diskExtent', disk.ds)
            write_text(vm, lv.desc, descriptor_text(disk, lv, None), 'diskDescriptor', disk.ds)
            disk.levels.append(lv); vm.disks.append(disk)
        write_vmx(vm)
        for fn in a.get('extra_files', []):
            with open(vm_path(vm, fn['name']), 'wb') as f:
                f.write(random.Random(len(fn['name'])).randbytes(fn.get('size', 100)))
            register(vm, fn['name'], fn.get('type', 'namespaceData'))
        S.vms[vm.id] = vm
        return vm


def make_snapshot(vm, name, desc, memory, quiesce):
    """the effect of CreateSnapshot_Task (called when the task completes)"""
    sid = nid('snap')
    ref = ('%d' % sid) if S.kind == 'esxi' else 'snapshot-%d' % sid
    sn = Snap(sid, ref, name, desc, bool(quiesce), vm.power, time.time())
    sn.parent = getattr(vm, 'cur', None); sn.chains = [list(d.levels) for d in vm.disks]; vm.cur = sn
    base = vm.vmx[:-4]
    sn.vmsn = '%s-Snapshot%d.vmsn' % (base, sid)
    with open(vm_path(vm, sn.vmsn), 'wb') as f:
        f.write(random.Random(sid).randbytes(3000))
    register(vm, sn.vmsn, 'snapshotData')
    if memory and vm.power == 'poweredOn':
        sn.vmem = '%s-Snapshot%d.vmem' % (base, sid)
        with open(vm_path(vm, sn.vmem), 'wb') as f:
            f.write(random.Random(sid + 1).randbytes(200000))
        register(vm, sn.vmem, 'snapshotMemory')
    for disk in vm.disks:
        if getattr(disk, 'indep', False):
            continue                    # an independent disk is not part of snapshots: no delta, writes go to the base file
        disk.snapn += 1
        n = disk.snapn
        ext = '%s-%06d-%s.vmdk' % (disk.name, n, S.prof['delta'])
        lv = Level('%s-%06d.vmdk' % (disk.name, n), ext, False)
        register(vm, ext, 'diskExtent', disk.ds)
        with open(vm_path(vm, ext), 'wb') as f:
            f.write(b'\0' * 512)
        write_text(vm, lv.desc, descriptor_text(disk, lv, disk.levels[-1].desc), 'diskDescriptor', disk.ds)
        disk.levels.append(lv)
    vm.snaps.append(sn)
    rewrite_vmsd(vm)
    write_vmx(vm)
    return sn


def rewrite_vmsd(vm):
    t = '.encoding = "UTF-8"\nsnapshot.lastUID = "%d"\nsnapshot.numSnapshots = "%d"\n' % (S.ids['snap'], len(vm.snaps))
    cur = getattr(vm, 'cur', None)
    if vm.snaps:
        t += 'snapshot.current = "%d"\n' % (cur or vm.snaps[-1]).id
    for i, s in enumerate(vm.snaps):
        t += 'snapshot%d.uid = "%d"\nsnapshot%d.filename = "%s"\nsnapshot%d.displayName = "%s"\nsnapshot%d.description = ""\n' % (i, s.id, i, s.vmsn, i, s.name, i)
        if s.parent:
            t += 'snapshot%d.parent = "%d"\n' % (i, s.parent.id)
        t += 'snapshot%d.createTimeHigh = "0"\nsnapshot%d.numDisks = "%d"\n' % (i, i, len(vm.disks))
    write_text(vm, vm.vmx[:-4] + '.vmsd', t)


def apply_write(vm, disk, off, data):
    top = disk.levels[-1]
    p = vm_path(vm, top.ext)
    if top.base:
        with open(p, 'r+b') as f:
            f.seek(off); f.write(data)
    else:
        with open(p, 'ab') as f:
            f.write(data)
        top.writes.append((off, data))


def remove_snapshot(vm, sn, consolidate=True):
    i = vm.snaps.index(sn)
    tail = sn is getattr(vm, 'cur', None) or (getattr(vm, 'cur', None) is None and sn is vm.snaps[-1])
    if getattr(vm, 'branched', False) and not tail:
        raise Fault('NotSupported', 'the mock removes only the current snapshot of a branched tree')
    for disk in vm.disks:
        if getattr(disk, 'indep', False):
            continue
        lo, up = (disk.levels[-2], disk.levels[-1]) if tail else (disk.levels[i], disk.levels[i + 1])
        if consolidate:
            for off, data in up.writes:
                if lo.base:
                    with open(vm_path(vm, lo.ext), 'r+b') as f:
                        f.seek(off); f.write(data)
                else:
                    with open(vm_path(vm, lo.ext), 'ab') as f:
                        f.write(data)
                    lo.writes.append((off, data))
        unregister(vm, up.desc); unregister(vm, up.ext)
        del disk.levels[-1 if tail else i + 1]
    unregister(vm, sn.vmsn)
    if sn.vmem:
        unregister(vm, sn.vmem)
    vm.snaps.remove(sn)
    for o in vm.snaps:
        if o.parent is sn:
            o.parent = sn.parent
    if getattr(vm, 'cur', None) is sn:
        vm.cur = sn.parent
    rewrite_vmsd(vm)
    write_vmx(vm)


def vm_snapshot_names(vm):
    return [s.name for s in vm.snaps]


# ---------------------------------------------------------------------------------------------------------------- XML building
def x(s):
    return xesc(str(s))


def xbool(b):
    return 'true' if b else 'false'


def vm_datastore_path(vm, name):
    return '[%s] %s' % (file_ds(vm, name), posixpath.normpath(vm.dir + '/' + name))


def file_info_xml(vm, name, info, tag='file'):
    try:
        sz = os.path.getsize(vm_path(vm, name))
    except OSError:
        sz = 0
    try:
        st = os.stat(vm_path(vm, name))
        uniq = st.st_blocks * 512 if hasattr(st, 'st_blocks') else st.st_size       # Windows (эмулятор внутри гостя): нет st_blocks
    except OSError:
        uniq = 0
    return ('<%s><key>%d</key><name>%s</name><type>%s</type><size>%d</size><uniqueSize>%d</uniqueSize><accessible>true</accessible></%s>'
            % (tag, info['key'], x(vm_datastore_path(vm, name)), info['type'], sz, uniq, tag))


def chain_xml(vm, levels_top_first):
    s = ''
    for lv in levels_top_first:
        s += '<chain><fileKey>%d</fileKey><fileKey>%d</fileKey></chain>' % (vm.files[lv.desc]['key'], vm.files[lv.ext]['key'])
    return s


def layout_xml(vm, tag, items_only_files=False):
    lag = vm.layout_lag > 0
    if lag:
        vm.layout_lag -= 1
    hide = layout_hide_now(vm)
    files = ''.join(file_info_xml(vm, n, i, 'file') for n, i in sorted(vm.files.items(), key=lambda kv: kv[1]['key']) if not (lag and n in lag_hidden(vm)) and not (hide is not None and hide in n))
    if items_only_files:
        return ''.join(file_info_xml(vm, n, i, 'VirtualMachineFileLayoutExFileInfo') for n, i in sorted(vm.files.items(), key=lambda kv: kv[1]['key']))
    disks = ''
    for d in vm.disks:
        disks += '<disk><key>%d</key>%s</disk>' % (d.key, chain_xml(vm, list(reversed(d.levels))))
    snaps = ''
    for j, s in enumerate(vm.snaps):
        if lag and j == len(vm.snaps) - 1:
            continue
        dk = ''.join('<disk><key>%d</key>%s</disk>' % (d.key, chain_xml(vm, list(reversed(s.chains[di] if getattr(vm, 'branched', False) else d.levels[:j + 1])))) for di, d in enumerate(vm.disks) if not getattr(d, 'indep', False))
        mem = '<memoryKey>%d</memoryKey>' % vm.files[s.vmem]['key'] if s.vmem else '<memoryKey>-1</memoryKey>'
        snaps += '<snapshot><key type="VirtualMachineSnapshot">%s</key><dataKey>%d</dataKey>%s%s</snapshot>' % (s.ref, vm.files[s.vmsn]['key'], mem, dk)
    return files + disks + snaps + '<timestamp>%s</timestamp>' % iso(time.time())


def lag_hidden(vm):
    return set()


def layout_hide_now(vm):
    """decided once per layoutEx rendering: fault layout_hide {match, reads}: the file entries of matching files are missing from layoutEx.file (the chains still
    name their keys); with reads=N only for the first N renderings after the VM has a snapshot"""
    h = fault_get('layout_hide')
    if not h:
        return None
    if h.get('reads'):
        if not vm.snaps:
            return None
        k = 'layout_hide:%s' % vm.name
        if S.counters.get(k, 0) >= h['reads']:
            return None
        S.counters[k] = S.counters.get(k, 0) + 1
    return h.get('match', '')


def iso(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z')


def device_xml(vm):
    out = ('<VirtualDevice xsi:type="ParaVirtualSCSIController"><key>1000</key><deviceInfo><label>SCSI controller 0</label><summary>VMware paravirtual SCSI</summary></deviceInfo>'
           '<busNumber>0</busNumber><sharedBus>noSharing</sharedBus></VirtualDevice>')
    if getattr(vm, 'cdrom', None):
        out += ('<VirtualDevice xsi:type="VirtualCdrom"><key>3000</key><deviceInfo><label>CD/DVD drive 1</label><summary>ISO [%s]</summary></deviceInfo>'
                '<backing xsi:type="VirtualCdromIsoBackingInfo"><fileName>%s</fileName></backing><controllerKey>200</controllerKey><unitNumber>0</unitNumber></VirtualDevice>' % (x(vm.cdrom), x(vm.cdrom)))
        out += '<VirtualDevice xsi:type="VirtualFloppy"><key>8000</key><deviceInfo><label>Floppy drive 1</label><summary>Remote</summary></deviceInfo><backing xsi:type="VirtualFloppyRemoteDeviceBackingInfo"><deviceName></deviceName></backing></VirtualDevice>'
    for d in vm.disks:
        top = d.levels[-1]
        capb = '<capacityInBytes>%d</capacityInBytes>' % d.cap if S.prof['cib'] else ''
        if getattr(d, 'rdm', None):
            out += ('<VirtualDevice xsi:type="VirtualDisk"><key>%d</key><deviceInfo><label>%s</label><summary>%d KB</summary></deviceInfo>'
                    '<backing xsi:type="VirtualDiskRawDiskMappingVer1BackingInfo"><fileName>%s</fileName><datastore type="Datastore">datastore-%s</datastore><deviceName>/vmfs/devices/disks/naa.600508b1001c</deviceName>'
                    '<compatibilityMode>%s</compatibilityMode><diskMode>persistent</diskMode><uuid>6000C29-%d</uuid></backing><controllerKey>1000</controllerKey><unitNumber>%d</unitNumber>'
                    '<capacityInKB>%d</capacityInKB>%s</VirtualDevice>'
                    % (d.key, x(d.label), d.cap // 1024, x(vm_datastore_path(vm, top.desc)), x(vm.ds), 'physicalMode' if d.rdm == 'physical' else 'virtualMode', d.key, d.key - 2000, d.cap // 1024, capb))
            continue
        out += ('<VirtualDevice xsi:type="VirtualDisk"><key>%d</key><deviceInfo><label>%s</label><summary>%d KB</summary></deviceInfo>'
                '<backing xsi:type="VirtualDiskFlatVer2BackingInfo"><fileName>%s</fileName><datastore type="Datastore">datastore-%s</datastore><diskMode>%s</diskMode>'
                '<thinProvisioned>%s</thinProvisioned><uuid>6000C29-%d</uuid></backing><controllerKey>1000</controllerKey><unitNumber>%d</unitNumber>'
                '<capacityInKB>%d</capacityInKB>%s</VirtualDevice>'
                % (d.key, x(d.label), d.cap // 1024, x(vm_datastore_path(vm, top.desc)), x(vm.ds), 'independent_persistent' if getattr(d, 'indep', False) else 'persistent', xbool(d.thin), d.key, d.key - 2000, d.cap // 1024, capb))
    return '<val xsi:type="ArrayOfVirtualDevice">%s</val>' % out


def snaptree_xml(vm):
    if not vm.snaps:
        return None

    def node(s, root):
        kids = ''.join(node(k, False) for k in vm.snaps if k.parent is s)
        tag = 'rootSnapshotList' if root else 'childSnapshotList'
        return ('<%s><snapshot type="VirtualMachineSnapshot">%s</snapshot><name>%s</name><description>%s</description><id>%d</id><createTime>%s</createTime>'
                '<state>%s</state><quiesced>%s</quiesced><backupManifest></backupManifest>%s<replaySupported>false</replaySupported></%s>'
                % (tag, s.ref, x(s.name), x(s.desc), s.id, iso(s.ctime), s.state, xbool(s.quiesced), kids, tag))
    cur = getattr(vm, 'cur', None) or vm.snaps[-1]
    return '<val xsi:type="VirtualMachineSnapshotInfo"><currentSnapshot type="VirtualMachineSnapshot">%s</currentSnapshot>%s</val>' % (cur.ref, ''.join(node(r, True) for r in vm.snaps if r.parent is None))


def vm_prop(vm, path):
    """the <val> of a VM property; None = not set; Fault InvalidProperty for a path the emulator does not know"""
    inaccessible = vm.conn != 'connected'
    if path == 'name':
        return '<val xsi:type="xsd:string">%s</val>' % x(vm.name)
    if path == 'runtime.powerState':
        return '<val xsi:type="VirtualMachinePowerState">%s</val>' % vm.power
    if path == 'runtime.connectionState':
        return '<val xsi:type="VirtualMachineConnectionState">%s</val>' % vm.conn
    if path.startswith('config.') or path in ('layoutEx', 'layoutEx.file', 'snapshot'):
        if inaccessible and path != 'snapshot':
            return None
    if path == 'config.keyId':
        if verkey(S.prof['api']) < (6, 5):
            raise Fault('InvalidProperty', 'The property config.keyId is not valid for VirtualMachine')
        return '<val xsi:type="CryptoKeyId"><keyId>0e1c3f9a-5b6d-4a44-9c1e-77f6e3a3d001</keyId><providerId><id>mock-kms</id></providerId></val>' if getattr(vm, 'encrypted', False) else None
    if path == 'config.template':
        return '<val xsi:type="xsd:boolean">%s</val>' % xbool(vm.template)
    if path == 'config.uuid':
        return '<val xsi:type="xsd:string">%s</val>' % vm.uuid
    if path == 'config.instanceUuid':
        return None if S.kind == 'esxi' and S.prof['api'] == '6.0' else '<val xsi:type="xsd:string">%s</val>' % vm.inst
    if path == 'config.guestFullName':
        return '<val xsi:type="xsd:string">%s</val>' % x(vm.guest)
    if path == 'config.files.vmPathName':
        return '<val xsi:type="xsd:string">%s</val>' % x(vm_datastore_path(vm, vm.vmx))
    if path == 'config.hardware.numCPU':
        return '<val xsi:type="xsd:int">%d</val>' % vm.cpu
    if path == 'config.hardware.memoryMB':
        return '<val xsi:type="xsd:int">%d</val>' % vm.mem
    if path == 'config.hardware.device':
        return device_xml(vm)
    if path == 'layoutEx':
        return '<val xsi:type="VirtualMachineFileLayoutEx">%s</val>' % layout_xml(vm, 'layoutEx')
    if path == 'layoutEx.file':
        return '<val xsi:type="ArrayOfVirtualMachineFileLayoutExFileInfo">%s</val>' % layout_xml(vm, 'layoutEx.file', True)
    if path == 'snapshot':
        return snaptree_xml(vm)
    raise Fault('InvalidProperty', 'The property %s is not valid for VirtualMachine' % path)


def task_prop(t, path):
    if path != 'info':
        raise Fault('InvalidProperty', 'bad task property')
    st = t['state']
    res = ''
    err = ''
    if st == 'success' and t.get('result'):
        res = '<result type="%s" xsi:type="ManagedObjectReference">%s</result>' % (t['result'][0], x(t['result'][1]))
    if st == 'error':
        f = t['error']
        err = ('<error xsi:type="LocalizedMethodFault"><fault xsi:type="%s"><faultMessage><key>%s</key><message>%s</message></faultMessage></fault>'
               '<localizedMessage>%s</localizedMessage></error>' % (f[0], f[0], x(f[1]), x(f[1])))
    return ('<val xsi:type="TaskInfo"><key>%s</key><task type="Task">%s</task><name>%s</name><descriptionId>%s</descriptionId><entity type="VirtualMachine">%s</entity>'
            '<entityName>%s</entityName><state>%s</state><cancelled>%s</cancelled><cancelable>true</cancelable>%s%s<reason xsi:type="TaskReasonUser"><userName>%s</userName></reason>'
            '<queueTime>%s</queueTime><startTime>%s</startTime></val>'
            % (t['id'], t['id'], t['name'], t['name'], t['vm'], x(t['vmname']), st, xbool(t.get('cancelled')), err, res, x(S.user), iso(t['ctime']), iso(t['ctime'])))


def dc_prop(dc, path):
    if path == 'name':
        return '<val xsi:type="xsd:string">%s</val>' % x(dc[1])
    if path == 'vmFolder':
        return '<val type="Folder" xsi:type="ManagedObjectReference">%s</val>' % S.inv[dc[0]]['vmFolder']
    if path == 'hostFolder':
        return '<val type="Folder" xsi:type="ManagedObjectReference">%s</val>' % S.inv[dc[0]]['hostFolder']
    if path == 'datastore':
        return '<val xsi:type="ArrayOfManagedObjectReference">%s</val>' % ''.join(mref('Datastore', DS_IDS[n]) for n in sorted(DS_IDS))
    raise Fault('InvalidProperty', 'bad datacenter property')


# ---------------------------------------------------------------------------------------------------------------- SOAP
def envelope(body):
    return '<?xml version="1.0" encoding="UTF-8"?>\n<soapenv:Envelope %s><soapenv:Body>%s</soapenv:Body></soapenv:Envelope>' % (NS_ENV, body)


def fault_xml(f):
    return envelope('<soapenv:Fault><faultcode>ServerFaultCode</faultcode><faultstring>%s</faultstring><detail><%sFault xmlns="urn:vim25" xsi:type="%s">%s</%sFault></detail></soapenv:Fault>'
                    % (x(f.msg), f.ftype, f.ftype, '<object type="Folder">%s</object>' % root_ref() if f.ftype == 'NotAuthenticated' else '', f.ftype))


def root_ref():
    return 'ha-folder-root' if S.kind == 'esxi' else 'group-d1'


def lname(tag):
    return tag.split('}', 1)[1] if '}' in tag else tag


def child(e, name):
    for c in e:
        if lname(c.tag) == name:
            return c
    return None


def children(e, name):
    return [c for c in e if lname(c.tag) == name]


def ctext(e, name, default=''):
    c = child(e, name)
    return (c.text or '').strip() if c is not None else default


def about_xml():
    p = S.prof
    if S.kind == 'esxi':
        full, ver, build, api_type, line = 'VMware ESXi %s build-%s' % (p['ev'], p['eb']), p['ev'], p['eb'], 'HostAgent', 'embeddedEsx'
        name = 'VMware ESXi'
        extra = '<licenseProductName>VMware ESX Server</licenseProductName><licenseProductVersion>%s</licenseProductVersion>' % p['ev'][:3]
    else:
        full, ver, build, api_type, line = 'VMware vCenter Server %s build-%s' % (p['cv'], p['cb']), p['cv'], p['cb'], 'VirtualCenter', 'vpx'
        name = 'VMware vCenter Server'
        extra = '<instanceUuid>%s</instanceUuid><licenseProductName>VMware VirtualCenter Server</licenseProductName><licenseProductVersion>%s</licenseProductVersion>' % (uuid.UUID(int=77), p['cv'][:3])
    unknown = '<futureField>9</futureField>' if verkey(p['api']) >= (8,) else ''
    return ('<about><name>%s</name><fullName>%s</fullName><vendor>VMware, Inc.</vendor><version>%s</version><build>%s</build><localeVersion>INTL</localeVersion>'
            '<localeBuild>000</localeBuild><osType>vmnix-x86</osType><productLineId>%s</productLineId><apiType>%s</apiType><apiVersion>%s</apiVersion>%s%s</about>'
            % (name, full, ver, build, line, api_type, p['api'], extra, unknown))


def service_content():
    esx = S.kind == 'esxi'
    return ('<returnval><rootFolder type="Folder">%s</rootFolder><propertyCollector type="PropertyCollector">%s</propertyCollector>%s'
            '<setting type="OptionManager">%s</setting><userDirectory type="UserDirectory">ha-user-directory</userDirectory>'
            '<sessionManager type="SessionManager">%s</sessionManager><fileManager type="FileManager">ha-nfc-file-manager</fileManager>'
            '<viewManager type="ViewManager">ViewManager</viewManager><customFieldsManager type="CustomFieldsManager">ha-customfields-manager</customFieldsManager></returnval>'
            % (root_ref(), 'ha-property-collector' if esx else 'propertyCollector', about_xml(), 'HostAgentSettings' if esx else 'VpxSettings', 'ha-sessionmgr' if esx else 'SessionManager'))


def session_alive(sid, touch=True):
    """a real host drops a session after 30 minutes without a call; `session_idle_s` shortens that for the tests"""
    with LOCK:
        if sid is None or sid not in S.sessions:
            return False
        idle = fault_get('session_idle_s')
        now = time.time()
        if idle and now - S.sessions[sid] > idle:
            del S.sessions[sid]
            record('fault', 'session-idle')
            return False
        if touch:
            S.sessions[sid] = now
        return True


def check_session(h):
    sid = h.cookie_session()
    if not session_alive(sid):
        raise Fault('NotAuthenticated', 'The session is not authenticated.')
    return sid


NFS_DS = 'NFS \u00fcn\u00ef #3 (\u00d1) \u0434\u0430\u043d\u043d\u044b\u0435'
DS_IDS = {'datastore1': 'datastore-1', 'datastore2': 'datastore-2', 'vsanDatastore': 'datastore-3', NFS_DS: 'datastore-4'}
DS_TYPES = {'vsanDatastore': 'vsan', NFS_DS: 'NFS'}


def build_inventory():
    S.inv = {}
    esx = S.kind == 'esxi'
    for i, (dcid, dcname) in enumerate(S.dcs):
        if esx:
            ids = dict(vmFolder='ha-folder-vm', hostFolder='ha-folder-host', compute='ha-compute-res', root='ha-root-pool', host='ha-host', pool2='ha-pool-prod')
            extra_folders = []
        else:
            n = 10 + i * 20
            ids = dict(vmFolder='group-v%d' % n, hostFolder='group-h%d' % (n + 1), compute='domain-c%d' % (n + 2), root='resgroup-%d' % (n + 3), host='host-%d' % (n + 4), pool2='resgroup-%d' % (n + 5))
            extra_folders = [('group-v%d' % (n + 6), 'Servers', ids['vmFolder']), ('group-v%d' % (n + 7), 'Linux', 'group-v%d' % (n + 6))]
        inv = dict(ids)
        inv['folders'] = [(ids['vmFolder'], 'vm', 'datacenter-parent')] + extra_folders
        inv['pools'] = [(ids['root'], 'Resources', ('ComputeResource', ids['compute'])), (ids['pool2'], 'Production', ('ResourcePool', ids['root']))]
        inv['hostname'] = 'esxi%d.lab' % (i + 1)
        S.inv[dcid] = inv


def mref(t, i):
    return '<ManagedObjectReference type="%s" xsi:type="ManagedObjectReference">%s</ManagedObjectReference>' % (t, x(i))


def ds_free(name):
    ov = fault_get('ds_free')
    if isinstance(ov, dict) and name in ov:
        return ov[name]
    import shutil
    return shutil.disk_usage(S.root).free


def inv_objects(dcid, vtype):
    inv = S.inv[dcid]
    if vtype == 'Datastore':
        return [('Datastore', DS_IDS[n]) for n in sorted(DS_IDS)]
    if vtype == 'ComputeResource':
        return [('ComputeResource', inv['compute'])]
    if vtype == 'ResourcePool':
        return [('ResourcePool', p[0]) for p in inv['pools']]
    if vtype == 'HostSystem':
        return [('HostSystem', inv['host'])]
    if vtype == 'Folder':
        return [('Folder', f[0]) for f in inv['folders']]
    return []


def inv_prop(t, i, path):
    if t == 'Datastore':
        name = [n for n, v in DS_IDS.items() if v == i][0]
        if path == 'name':
            return '<val xsi:type="xsd:string">%s</val>' % x(name)
        if path == 'summary.capacity':
            import shutil
            return '<val xsi:type="xsd:long">%d</val>' % shutil.disk_usage(S.root).total
        if path == 'summary.freeSpace':
            return '<val xsi:type="xsd:long">%d</val>' % ds_free(name)
        if path == 'summary.type':
            return '<val xsi:type="xsd:string">%s</val>' % DS_TYPES.get(name, 'VMFS')
        if path == 'summary.accessible':
            return '<val xsi:type="xsd:boolean">true</val>'
    else:
        for dcid, inv in S.inv.items():
            if t == 'ComputeResource' and i == inv['compute']:
                if path == 'name':
                    return '<val xsi:type="xsd:string">%s</val>' % x(inv['hostname'])
                if path == 'resourcePool':
                    return '<val type="ResourcePool" xsi:type="ManagedObjectReference">%s</val>' % inv['root']
                if path == 'host':
                    return '<val xsi:type="ArrayOfManagedObjectReference">%s</val>' % mref('HostSystem', inv['host'])
            if t == 'HostSystem' and i == inv['host']:
                if path == 'name':
                    return '<val xsi:type="xsd:string">%s</val>' % x(inv['hostname'])
                if path == 'parent':
                    return '<val type="ComputeResource" xsi:type="ManagedObjectReference">%s</val>' % inv['compute']
            if t == 'ResourcePool':
                for pid, pname, par in inv['pools']:
                    if pid == i:
                        if path == 'name':
                            return '<val xsi:type="xsd:string">%s</val>' % x(pname)
                        if path == 'parent':
                            return '<val type="%s" xsi:type="ManagedObjectReference">%s</val>' % par
            if t == 'Folder':
                for fid, fname, par in inv['folders']:
                    if fid == i:
                        if path == 'name':
                            return '<val xsi:type="xsd:string">%s</val>' % x(fname)
                        if path == 'parent':
                            return '<val type="Folder" xsi:type="ManagedObjectReference">%s</val>' % par if par != 'datacenter-parent' else '<val type="Datacenter" xsi:type="ManagedObjectReference">%s</val>' % dcid
                        if path == 'childType':
                            return '<val xsi:type="ArrayOfString"><string>VirtualMachine</string><string>Folder</string></val>'
    raise Fault('InvalidProperty', 'The property %s is not valid for %s' % (path, t))


def view_members(view):
    ctype, cid, vtype = view['ctype'], view['cid'], view['vtype']
    if vtype == 'Datacenter':
        return [('Datacenter', d[0]) for d in S.dcs]
    if vtype in ('Datastore', 'ComputeResource', 'ResourcePool', 'HostSystem', 'Folder'):
        out = []
        for d in S.dcs:
            if ctype == 'Datacenter' and d[0] != cid:
                continue
            out += inv_objects(d[0], vtype)
        return out
    if vtype == 'VirtualMachine':
        out = []
        for vm in S.vms.values():
            if ctype == 'Datacenter':
                dc = [d for d in S.dcs if d[0] == cid]
                if not dc or dc[0][1] != vm.dc:
                    continue
            out.append(('VirtualMachine', vm.id))
        return out
    return []


def find_dc(ref):
    for d in S.dcs:
        if d[0] == ref:
            return d
    return None


def collect_objects(spec_set):
    """-> list of (type, id, [paths]) in order"""
    out = []
    for fs in children(spec_set, 'specSet'):
        props = {}
        for ps in children(fs, 'propSet'):
            t = ctext(ps, 'type')
            paths = [(c.text or '').strip() for c in children(ps, 'pathSet')]
            props[t] = (paths, ctext(ps, 'all') == 'true')
        for os_ in children(fs, 'objectSet'):
            ob = child(os_, 'obj')
            otype, oid = ob.get('type'), (ob.text or '').strip()
            if otype == 'ContainerView' and child(os_, 'selectSet') is not None:
                view = S.views.get(oid)
                if view is None:
                    raise Fault('ManagedObjectNotFound', 'The object has already been deleted or has not been completely created')
                objs = view_members(view)
            else:
                objs = [(otype, oid)]
            for t, i in objs:
                if t in props:
                    out.append((t, i, props[t][0]))
    return out


def object_content(t, i, paths):
    props = ''
    if t == 'VirtualMachine':
        vm = S.vms.get(i)
        if vm is None:
            raise Fault('ManagedObjectNotFound', 'The object has already been deleted or has not been completely created')
        for p in paths:
            v = vm_prop(vm, p)
            if v is not None:
                props += '<propSet><name>%s</name>%s</propSet>' % (x(p), v)
    elif t == 'Datacenter':
        dc = find_dc(i)
        for p in paths:
            props += '<propSet><name>%s</name>%s</propSet>' % (x(p), dc_prop(dc, p))
    elif t == 'Task':
        task = S.tasks.get(i)
        if task is None:
            raise Fault('ManagedObjectNotFound', 'The object has already been deleted or has not been completely created')
        advance_task(task)
        for p in paths:
            props += '<propSet><name>%s</name>%s</propSet>' % (x(p), task_prop(task, p))
    elif t in ('Datastore', 'ComputeResource', 'ResourcePool', 'HostSystem', 'Folder'):
        for p in paths:
            props += '<propSet><name>%s</name>%s</propSet>' % (x(p), inv_prop(t, i, p))
    else:
        raise Fault('InvalidType', 'bad type %s' % t)
    return '<obj type="%s">%s</obj>%s' % (t, x(i), props)


def advance_task(t):
    """tasks take a few polls: queued -> running -> done (the effect happens when it completes)"""
    if t['state'] in ('success', 'error'):
        return
    if fault_get('task_hang'):
        t['state'] = 'running'
        return
    slow = fault_get('task_slow')       # {name: 'CreateSnapshot_Task', sec: N}: the task ends after N seconds whatever the client does (CancelTask is refused)
    if slow and slow.get('name', '') in t['name'] and time.time() - t['ctime'] < slow.get('sec', 5):
        t['state'] = 'running'
        return
    t['polls'] += 1
    if t['polls'] == 1:
        t['state'] = 'queued' if random.random() < 0.3 else 'running'
        return
    if t['polls'] < 1 + fault_get('task_polls', 1):
        t['state'] = 'running'
        return
    try:
        t['run']()
        t['state'] = 'success'
    except Fault as f:
        t['state'] = 'error'; t['error'] = (f.ftype, f.msg)


class _Ent:
    def __init__(self, i, n):
        self.id, self.name = i, n


def new_task(name, vm, run, result=None):
    tid = 'task-%d' % nid('task') if S.kind == 'vcenter' else 'haTask-%s-vim.%s-%d' % (vm.id, name, nid('task'))
    t = dict(id=tid, name=name, vm=vm.id, vmname=vm.name, run=run, state='queued', polls=0, ctime=time.time(), result=None)
    S.tasks[tid] = t
    slow = fault_get('task_slow')
    if slow and slow.get('name', '') in name:
        def fire():
            time.sleep(slow.get('sec', 5) + 0.05)
            with LOCK:
                for _ in range(6):
                    advance_task(t)
        threading.Thread(target=fire, daemon=True).start()
    return t


def m_create_snapshot(req, h):
    vm = S.vms.get(ctext_ref(req))
    if vm is None:
        raise Fault('ManagedObjectNotFound', 'The object has already been deleted or has not been completely created')
    name, desc = ctext(req, 'name'), ctext(req, 'description')
    memory, quiesce = ctext(req, 'memory') == 'true', ctext(req, 'quiesce') == 'true'
    record('snap', 'CreateSnapshot', json.dumps(dict(vm=vm.name, name=name, quiesce=quiesce, memory=memory)))
    lic = fault_get('license_block')
    if lic == 'call':
        raise Fault('RestrictedVersion', 'Current license or ESXi version prohibits execution of the requested operation.')
    if quiesce and vm.power != 'poweredOn':
        raise Fault('InvalidState', 'The operation is not allowed in the current state.')

    def run():
        if lic == 'task':
            raise Fault('RestrictedVersion', 'Current license or ESXi version prohibits execution of the requested operation.')
        qf = fault_get('quiesce_fail')
        if quiesce and qf:
            if qf == 'quiesce':
                raise Fault('ApplicationQuiesceFault', 'Failed to quiesce the virtual machine. The guest operating system reported an error.')
            raise Fault('ToolsUnavailable', 'Cannot quiesce this virtual machine because VMware Tools is not currently available.')
        tf = fault_get('task_fail')
        if tf:
            raise Fault('FileFault', str(tf))
        sn = make_snapshot(vm, name, desc, memory, quiesce)
        vm.layout_lag = fault_get('layout_lag', 0)
        t['result'] = ('VirtualMachineSnapshot', sn.ref)
        S.events.append(('create', vm.name, name))
    t = new_task('CreateSnapshot_Task', vm, run)
    if fault_get('expire_after_task_start') and fault_take_inner('expire_after_task_start'):
        S.sessions.clear()              # the session dies right after the task was started: the client holds a task id and no session
    return '<CreateSnapshot_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></CreateSnapshot_TaskResponse>' % x(t['id'])


def ds_file_path(spec):
    """'[ds] a/b' -> (ds, local path); Fault for a bad / unsafe one"""
    if not spec.startswith('['):
        raise Fault('InvalidDatastorePath', 'Invalid datastore path \'%s\'.' % spec)
    ds, _, rest = spec[1:].partition(']')
    rest = rest.lstrip(' ')
    parts = [q for q in rest.split('/') if q != '']
    if ds not in DS_IDS or any(q in ('..', '.') for q in parts) or '\\' in rest:
        raise Fault('InvalidDatastorePath', 'Invalid datastore path \'%s\'.' % spec)
    return ds, os.path.join(S.root, ds, *parts), '/'.join(parts)


def m_make_directory(req, h):
    name = ctext(req, 'name')
    parents = ctext(req, 'createParentDirectories') == 'true'
    record('fm', 'MakeDirectory', name)
    ds, path, rel = ds_file_path(name)
    race = fault_get('mkdir_race')      # {times: N}: another client creates the folder (with a file of its own in it) an instant before: the first N MakeDirectory calls lose the race
    if race and not os.path.exists(path) and fault_take_inner('mkdir_race'):
        os.makedirs(path, exist_ok=True)
        with open(os.path.join(path, 'other-clients-file.txt'), 'wb') as f:
            f.write(b'precious')
    if os.path.exists(path):
        raise Fault('FileAlreadyExists', 'Cannot complete the operation because the file or folder %s already exists' % name)
    if not parents and not os.path.isdir(os.path.dirname(path)):
        raise Fault('FileNotFound', 'File %s was not found' % os.path.dirname(name))
    os.makedirs(path)
    return '<MakeDirectoryResponse xmlns="urn:vim25"></MakeDirectoryResponse>'


def m_delete_ds_file(req, h):
    name = ctext(req, 'name')
    record('fm', 'DeleteDatastoreFile', name)
    ds, path, rel = ds_file_path(name)
    ent = _Ent('ha-nfc-file-manager', name)

    def run():
        import shutil
        if not os.path.exists(path):
            raise Fault('FileNotFound', 'File %s was not found' % name)
        if os.path.isdir(path):
            shutil.rmtree(path)
        else:
            os.remove(path)
    t = new_task('DeleteDatastoreFile_Task', ent, run)
    return '<DeleteDatastoreFile_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></DeleteDatastoreFile_TaskResponse>' % x(t['id'])


def parse_vmx(text):
    d = {}
    order = []
    for line in text.split('\n'):
        k, eq, v = line.partition('=')
        if not eq:
            continue
        k = k.strip(); v = v.strip().strip('"')
        v = re.sub(r'\|([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), v)        # |XX escapes of the .vmx format
        d[k.lower()] = v; order.append((k, v))
    return d, order


def descriptor_info(path):
    txt = open(path, 'rb').read(8192).decode('utf-8', 'replace')
    ext, parent, sectors = None, None, 0
    for line in txt.split('\n'):
        if line.startswith('RW '):
            parts = line.split('"')
            sectors = int(line.split()[1]); ext = parts[1]
        if line.startswith('parentFileNameHint'):
            parent = line.split('"')[1]
    return ext, parent, sectors


def register_vm_from_vmx(dcname, spec, name):
    ds, path, rel = ds_file_path(spec)
    if not os.path.isfile(path) or not rel.endswith('.vmx'):
        raise Fault('FileNotFound', 'File %s was not found' % spec)
    vdir, vmxname = os.path.split(rel)
    d, order = parse_vmx(open(path, 'rb').read().decode('utf-8', 'replace'))
    nm = name or order_get(order, 'displayName') or vmxname[:-4]
    for v in S.vms.values():
        if v.name == nm and v.dc == dcname:
            raise Fault('DuplicateName', 'The name \'%s\' already exists.' % nm)
    vm = VM()
    vm.id = ('%d' % nid('vm')) if S.kind == 'esxi' else 'vm-%d' % nid('vm')
    vm.name = nm; vm.dc = dcname; vm.ds = ds; vm.dir = vdir; vm.uuid = d.get('uuid.bios', str(uuid.uuid4())); vm.inst = str(uuid.uuid4())
    vm.power = 'poweredOff'; vm.template = False; vm.conn = 'connected'; vm.guest = 'Other Linux (64-bit)'; vm.cpu = 2; vm.mem = 1024
    vm.files = {}; vm.disks = []; vm.snaps = []; vm.vmx = vmxname; vm.vmx_extra = []; vm.layout_lag = 0
    base = os.path.join(S.root, ds, vdir)
    for fn in sorted(os.listdir(base)):
        if os.path.isdir(os.path.join(base, fn)):
            continue
        ft = None
        if fn.endswith('.vmx'): ft = 'config'
        elif fn.endswith('.vmxf'): ft = 'extendedConfig'
        elif fn.endswith('.nvram'): ft = 'nvram'
        elif fn.endswith('.vmsd'): ft = 'snapshotList'
        elif fn.endswith('.vmsn'): ft = 'snapshotData'
        elif fn.endswith('.vmem'): ft = 'snapshotMemory'
        elif fn.endswith('.vswp'): ft = 'swap'
        elif fn.endswith('.log'): ft = 'log'
        if ft:
            register(vm, fn, ft)
    unit = 0
    for k, v in order:
        if not (k.lower().startswith('scsi') and k.lower().endswith('.filename')):
            continue
        dsx, ddir = ds, vdir
        fnm = v
        if v.startswith('['):
            dsx, _, r2 = v[1:].partition(']'); r2 = r2.lstrip(' ')
            ddir, _, fnm = r2.rpartition('/')
        chain = []
        cur = fnm
        while cur:
            dp = os.path.join(S.root, dsx, ddir, cur)
            if not os.path.isfile(dp):
                raise Fault('FileNotFound', 'File [%s] %s/%s was not found' % (dsx, ddir, cur))
            ext, parent, sectors = descriptor_info(dp)
            chain.append((cur, ext, sectors))
            cur = parent
        chain.reverse()
        disk = Disk(2000 + unit, 'Hard disk %d' % (unit + 1), chain[0][2] * 512, False, chain[0][0][:-5])
        disk.ds = dsx if dsx != ds else None
        for desc, ext, _ in chain:
            lv = Level(desc, ext, len(disk.levels) == 0)
            register(vm, desc, 'diskDescriptor', disk.ds); register(vm, ext, 'diskExtent', disk.ds)
            disk.levels.append(lv)
        vm.disks.append(disk); unit += 1
    S.vms[vm.id] = vm
    return vm


def order_get(order, key):
    for k, v in order:
        if k.lower() == key.lower():
            return v
    return ''


def m_register_vm(req, h):
    folder = ctext_ref(req)
    path = ctext(req, 'path'); name = ctext(req, 'name')
    pool = child(req, 'pool'); host = child(req, 'host')
    pool = (pool.text or '').strip() if pool is not None else ''
    record('reg', 'RegisterVM', json.dumps(dict(path=path, name=name, folder=folder, pool=pool)))
    dcname = None
    for dcid, inv in S.inv.items():
        if folder in [f[0] for f in inv['folders']]:
            dcname = [d[1] for d in S.dcs if d[0] == dcid][0]; dcinv = inv
    if dcname is None:
        raise Fault('ManagedObjectNotFound', 'The object \'vim.Folder:%s\' has already been deleted or has not been completely created' % folder)
    if S.kind == 'vcenter' and not pool and ctext(req, 'asTemplate') != 'true':
        raise Fault('InvalidArgument', 'A specified parameter was not correct: pool')
    if pool and pool not in [p[0] for p in dcinv['pools']]:
        raise Fault('ManagedObjectNotFound', 'The object \'vim.ResourcePool:%s\' has already been deleted or has not been completely created' % pool)
    ent = _Ent(folder, 'folder')

    def run():
        rf = fault_get('register_fail')
        if rf:
            raise Fault(rf, {'InvalidDatastorePath': 'Invalid datastore path \'%s\'.' % path, 'AlreadyExists': 'The specified key, name, or identifier already exists.',
                             'DuplicateName': 'The name \'%s\' already exists.' % name}.get(rf, 'RegisterVM failed (%s)' % rf))
        vm = register_vm_from_vmx(dcname, path, name)
        t['result'] = ('VirtualMachine', vm.id)
        S.events.append(('register', vm.name, path))
    t = new_task('RegisterVM_Task', ent, run)
    return '<RegisterVM_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></RegisterVM_TaskResponse>' % x(t['id'])


def m_power_on(req, h):
    vm = S.vms.get(ctext_ref(req))
    if vm is None:
        raise Fault('ManagedObjectNotFound', 'The object has already been deleted or has not been completely created')
    record('vm', 'PowerOnVM', vm.name)

    def run():
        if fault_get('poweron_fail'):
            raise Fault('InsufficientResourcesFault', 'Insufficient resources: the VM cannot be powered on.')
        vm.power = 'poweredOn'
    t = new_task('PowerOnVM_Task', vm, run)
    return '<PowerOnVM_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></PowerOnVM_TaskResponse>' % x(t['id'])


def ctext_ref(req):
    c = child(req, '_this')
    return (c.text or '').strip() if c is not None else ''


def m_remove_snapshot(req, h):
    ref = ctext_ref(req)
    consolidate = ctext(req, 'consolidate', 'true') == 'true'
    found = None
    for vm in S.vms.values():
        for sn in vm.snaps:
            if sn.ref == ref:
                found = (vm, sn)
    record('snap', 'RemoveSnapshot', json.dumps(dict(ref=ref, consolidate=consolidate, found=bool(found))))
    if not found:
        raise Fault('ManagedObjectNotFound', 'The object \'vim.vm.Snapshot:%s\' has already been deleted or has not been completely created' % ref)
    vm, sn = found
    if fault_get('remove_fail'):
        t = new_task('RemoveSnapshot_Task', vm, lambda: (_ for _ in ()).throw(Fault('FileFault', str(fault_get('remove_fail')))))
        return '<RemoveSnapshot_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></RemoveSnapshot_TaskResponse>' % x(t['id'])

    def run():
        if sn not in vm.snaps:
            raise Fault('ManagedObjectNotFound', 'snapshot already removed')
        remove_snapshot(vm, sn, consolidate)
        S.events.append(('remove', vm.name, sn.name))
    t = new_task('RemoveSnapshot_Task', vm, run)
    if fault_get('expire_after_task_start') and fault_take_inner('expire_after_task_start'):
        S.sessions.clear()
    return '<RemoveSnapshot_TaskResponse xmlns="urn:vim25"><returnval type="Task">%s</returnval></RemoveSnapshot_TaskResponse>' % x(t['id'])


def retrieve(req, ex):
    objs = collect_objects(req)
    maxo = 0
    if ex:
        opt = child(req, 'options')
        if opt is not None:
            maxo = int(ctext(opt, 'maxObjects', '0') or 0)
    contents = [object_content(t, i, p) for t, i, p in objs]
    return contents, maxo


def m_retrieve_ex(req, h):
    contents, maxo = retrieve(req, True)
    if not contents:
        return '<RetrievePropertiesExResponse xmlns="urn:vim25"></RetrievePropertiesExResponse>'
    return ex_page(contents, maxo)


def ex_page(contents, maxo):
    page, rest = (contents[:maxo], contents[maxo:]) if maxo else (contents, [])
    tok = ''
    if rest:
        tok = 'tok-%d' % nid('tok')
        S.tokens[tok] = (rest, maxo)
    return ('<%s xmlns="urn:vim25"><returnval>%s%s</returnval></%s>'
            % ('RetrievePropertiesExResponse', ('<token>%s</token>' % tok) if tok else '', ''.join('<objects>%s</objects>' % c for c in page), 'RetrievePropertiesExResponse'))


def m_continue_ex(req, h):
    tok = ctext(req, 'token')
    if tok not in S.tokens:
        raise Fault('InvalidProperty', 'The token is invalid')
    rest, maxo = S.tokens.pop(tok)
    return ex_page(rest, maxo).replace('RetrievePropertiesExResponse', 'ContinueRetrievePropertiesExResponse')


def m_retrieve(req, h):
    if fault_get('no_retrieve_ex') is None:
        pass
    contents, _ = retrieve(req, False)
    return '<RetrievePropertiesResponse xmlns="urn:vim25">%s</RetrievePropertiesResponse>' % ''.join('<returnval>%s</returnval>' % c for c in contents)


def m_create_view(req, h):
    c = child(req, 'container')
    vid = 'session[%08x]%d' % (random.getrandbits(32), nid('view'))
    S.views[vid] = dict(ctype=c.get('type'), cid=(c.text or '').strip(), vtype=ctext(req, 'type'), recursive=ctext(req, 'recursive') == 'true')
    return '<CreateContainerViewResponse xmlns="urn:vim25"><returnval type="ContainerView">%s</returnval></CreateContainerViewResponse>' % x(vid)


def m_destroy_view(req, h):
    S.views.pop(ctext_ref(req), None)
    return '<DestroyViewResponse xmlns="urn:vim25"></DestroyViewResponse>'


def m_login(req, h):
    u, p = ctext(req, 'userName'), ctext(req, 'password')
    record('auth', 'Login', 'user=' + u)
    if u != S.user or p != S.password:
        raise Fault('InvalidLogin', 'Cannot complete login due to an incorrect user name or password.')
    sid = '52%s-%s' % (uuid.uuid4().hex[:30], nid('sess'))
    S.sessions[sid] = time.time()
    h.new_cookie = sid
    return ('<LoginResponse xmlns="urn:vim25"><returnval><key>%s</key><userName>%s</userName><fullName>Administrator</fullName><loginTime>%s</loginTime><lastActiveTime>%s</lastActiveTime>'
            '<locale>en</locale><messageLocale>en</messageLocale><extensionSession>false</extensionSession><ipAddress>127.0.0.1</ipAddress><userAgent>VM2Backup</userAgent><callCount>0</callCount></returnval></LoginResponse>'
            % (sid, x(u), iso(time.time()), iso(time.time())))


def m_logout(req, h):
    sid = h.cookie_session()
    S.sessions.pop(sid, None)
    record('auth', 'Logout')
    return '<LogoutResponse xmlns="urn:vim25"></LogoutResponse>'


def m_cancel(req, h):
    t = S.tasks.get(ctext_ref(req))
    if t is not None and t['state'] in ('queued', 'running'):
        t['cancelled'] = True
        if fault_get('task_hang'):
            t['state'] = 'error'; t['error'] = ('RequestCanceled', 'The task was canceled by a user.')
    return '<CancelTaskResponse xmlns="urn:vim25"></CancelTaskResponse>'


def m_rename_snapshot(req, h):
    ref = ctext_ref(req)
    record('snap', 'RenameSnapshot', ref)
    for vm in S.vms.values():
        for sn in vm.snaps:
            if sn.ref == ref:
                if ctext(req, 'name'):
                    sn.name = ctext(req, 'name')
                sn.desc = ctext(req, 'description')
                return '<RenameSnapshotResponse xmlns="urn:vim25"></RenameSnapshotResponse>'
    raise Fault('ManagedObjectNotFound', 'The object \'vim.vm.Snapshot:%s\' has already been deleted or has not been completely created' % ref)


def m_refresh(req, h):
    return '<RefreshStorageInfoResponse xmlns="urn:vim25"></RefreshStorageInfoResponse>'


METHODS = {'CreateSnapshot_Task': m_create_snapshot, 'RemoveSnapshot_Task': m_remove_snapshot, 'RetrievePropertiesEx': m_retrieve_ex,
           'ContinueRetrievePropertiesEx': m_continue_ex, 'RetrieveProperties': m_retrieve, 'CreateContainerView': m_create_view, 'DestroyView': m_destroy_view,
           'CancelTask': m_cancel, 'RenameSnapshot': m_rename_snapshot, 'RefreshStorageInfo': m_refresh, 'MakeDirectory': m_make_directory, 'DeleteDatastoreFile_Task': m_delete_ds_file,
           'RegisterVM_Task': m_register_vm, 'PowerOnVM_Task': m_power_on}


def soap(h, body):
    action = (h.headers.get('SOAPAction') or '').strip('"')
    if action.startswith('urn:vim25/'):
        v = action[len('urn:vim25/'):]
        if verkey(v) and verkey(v) < verkey(S.prof['minsoap']):
            record('fault', 'soap-version', v)
            raise Fault('InvalidRequest', 'The SOAP request version %s is not supported by this server (minimum %s).' % (v, S.prof['minsoap']), 500)
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        raise Fault('InvalidRequest', 'Invalid SOAP request', 500)
    b = child(root, 'Body')
    if b is None or len(b) == 0:
        raise Fault('InvalidRequest', 'Empty SOAP body', 500)
    req = b[0]
    method = lname(req.tag)
    record('soap', method)
    with LOCK:
        S.soap_calls += 1
        n = fault_get('expire_after_calls')
        if n:
            S.faults['expire_after_calls'] = n - 1
            if n - 1 == 0:
                S.sessions.clear(); S.faults.pop('expire_after_calls', None)
        if fault_get('http503_soap') and method not in ('RetrieveServiceContent',) and fault_take('http503_soap'):
            raise Fault('ServerBusy', 'service unavailable', 503)
        if method == 'RetrieveServiceContent':
            return '<RetrieveServiceContentResponse xmlns="urn:vim25">%s</RetrieveServiceContentResponse>' % service_content()
        if method == 'Login':
            return m_login(req, h)
        sid = check_session(h)
        if fault_get('no_retrieve_ex') and method in ('RetrievePropertiesEx', 'ContinueRetrievePropertiesEx'):
            raise Fault('MethodNotFound', 'The method %s is not supported.' % method)
        sf = fault_get('soap_fault')
        if sf and sf.get('method', method) == method and fault_take_inner('soap_fault'):
            record('fault', 'soap-fault', method)
            raise Fault(sf.get('type', 'RuntimeFault'), sf.get('msg', 'injected fault'), sf.get('http', 500))
        if method == 'Logout':
            return m_logout(req, h)
        fn = METHODS.get(method)
        if fn is None:
            raise Fault('MethodNotFound', 'The method %s is not supported.' % method)
        return fn(req, h)


# ---------------------------------------------------------------------------------------------------------------- HTTP handler
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'VMware-Mock'
    timeout = 120

    def log_message(self, fmt, *a):
        pass

    def cookie_session(self):
        c = self.headers.get('Cookie') or ''
        for part in c.split(';'):
            k, _, v = part.strip().partition('=')
            if k == 'vmware_soap_session':
                return v.strip('"')
        return None

    def send_bytes(self, status, data, ctype='text/xml; charset=utf-8', extra=None):
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def read_body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n else b''

    def hang_up(self):
        self.close_connection = True
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    RAW_KINDS = 16

    def raw_out(self, kind):
        """a hostile / broken HTTP answer in place of the real one, then the connection is dropped"""
        record('fault', 'raw_http', str(kind))
        ok = b'HTTP/1.1 200 OK\r\n'
        k = kind % self.RAW_KINDS
        raws = [
            ok + b'Content-Length: -5\r\n\r\nabc',
            ok + b'Content-Length: 99999999999999999999999999\r\n\r\nabcdef',
            ok + b'Transfer-Encoding: chunked\r\n\r\nffffffffffffffffff\r\nxx',
            ok + b'Transfer-Encoding: chunked\r\n\r\n-1\r\nxx\r\n0\r\n\r\n',
            b'garbage garbage\r\n\r\n',
            b'HTTP/1.1 99999 Whatever\r\nContent-Length: 0\r\n\r\n',
            ok + b'X-Big: ' + b'a' * 200000 + b'\r\nContent-Length: 0\r\n\r\n',
            ok + b''.join(b'X-H%d: v\r\n' % i for i in range(20000)) + b'Content-Length: 0\r\n\r\n',
            ok + b'Content-Length: 5\x00\r\n\r\nab\x00cd',
            b'',
            b'HTTP/1.1 100 Continue\r\n\r\n' * 3000,
            ok + b'Content-Length: 5\r\nContent-Length: 7\r\n\r\nabcdefg',
            b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc',
            ok + b'Transfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n',
            b'\x16\x03\x01\x00\x05hello',
            ok + b'Content-Type: text/xml\r\nContent-Length: 40\r\n\r\n<soapenv:Envelope><soapenv:Body>',
        ]
        try:
            self.wfile.write(raws[k]); self.wfile.flush()
        except OSError:
            pass
        return self.hang_up()

    def do_POST(self):
        if not self.path.startswith('/__mock/') and (S.dead or tick_event()):
            return self.hang_up()
        body = self.read_body()
        if self.path.startswith('/__mock/'):
            return self.control(body)
        if self.path != '/sdk':
            return self.send_bytes(404, b'not found', 'text/plain')
        self.new_cookie = None
        rw = fault_get('raw_http')
        if rw and rw.get('target') == 'soap' and fault_take_inner('raw_http'):
            return self.raw_out(rw.get('kind', 0))
        if fault_get('slow_soap'):
            time.sleep(fault_get('slow_soap') / 1000.0)
        try:
            out = soap(self, body)
            extra = []
            if self.new_cookie:
                extra.append(('Set-Cookie', 'vmware_soap_session="%s"; Path=/; HttpOnly; Secure' % self.new_cookie))
            q = (fault_get('http_quirk') or {}).get('mode', '')
            if q == 'soap_chunked':
                data = envelope(out).encode('utf-8')
                self.send_response(200); self.send_header('Content-Type', 'text/xml; charset=utf-8'); self.send_header('Transfer-Encoding', 'chunked')
                for k, v in extra:
                    self.send_header(k, v)
                self.end_headers()
                for i in range(0, len(data), 1500):
                    c = data[i:i + 1500]
                    self.wfile.write(b'%X\r\n' % len(c) + c + b'\r\n')
                self.wfile.write(b'0\r\n\r\n')
            elif q == 'conn_close':
                self.close_connection = True
                self.send_bytes(200, envelope(out).encode('utf-8'), extra=extra + [('Connection', 'close')])
            else:
                self.send_bytes(200, envelope(out).encode('utf-8'), extra=extra)
        except Fault as f:
            self.send_bytes(f.http, fault_xml(f).encode('utf-8') if f.http != 503 else b'service unavailable')

    def do_GET(self):
        if self.path.startswith('/__mock/'):
            return self.control(b'')
        if S.dead or tick_event():
            return self.hang_up()
        if self.path.startswith('/folder'):
            return self.folder()
        if self.path == '/' or self.path.startswith('/ui'):
            return self.send_bytes(200, b'<html><body>Welcome to VMware ESXi (mock)</body></html>', 'text/html')
        self.send_bytes(404, b'not found', 'text/plain')

    do_HEAD = do_GET

    def do_PUT(self):
        if S.dead or tick_event():
            return self.hang_up()
        if self.path.startswith('/folder'):
            return self.folder()
        self.read_body()
        self.send_bytes(404, b'not found', 'text/plain')

    # ---- datastore files
    def authorized(self):
        sid = self.cookie_session()
        if session_alive(sid, touch=False):                # a file transfer does not count as activity of the SOAP session
            return True
        a = self.headers.get('Authorization') or ''
        if a.startswith('Basic '):
            try:
                u, _, p = base64.b64decode(a[6:]).decode('utf-8').partition(':')
                return u == S.user and p == S.password
            except Exception:
                return False
        return False

    def folder(self):
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        rel = urllib.parse.unquote(u.path[len('/folder'):]).lstrip('/')
        if self.command == 'PUT':
            data_len = int(self.headers.get('Content-Length') or 0)
        record(self.command.lower(), rel, 'range=' + (self.headers.get('Range') or ''))
        if not self.authorized():
            if self.command == 'PUT':
                self.read_body()
            return self.send_bytes(401, b'Unauthorized', 'text/plain', [('WWW-Authenticate', 'Basic realm="VMware HTTP server"')])
        ds = (q.get('dsName') or [''])[0]
        dcp = (q.get('dcPath') or [''])[0]
        if dcp not in [d[1] for d in S.dcs] or ds not in DS_IDS:
            if self.command == 'PUT':
                self.read_body()
            return self.send_bytes(404, b'Not Found', 'text/plain')
        parts = rel.split('/')
        if any(p in ('..', '.') or p == '' and i < len(parts) - 1 for i, p in enumerate(parts)) or '\\' in rel:
            return self.send_bytes(400, b'Bad path', 'text/plain')
        path = os.path.join(S.root, ds, *parts)
        if self.command == 'PUT':
            return self.put(path, rel, ds)
        rw = fault_get('raw_http')
        if rw and rw.get('target') == 'folder' and self.command in ('GET', 'HEAD') and fault_take_inner('raw_http'):
            return self.raw_out(rw.get('kind', 0))
        pf = fault_get('file_503')
        if pf and pf.get('match', '') in rel and fault_take_inner('file_503'):
            return self.send_bytes(503, b'Service Unavailable', 'text/plain')
        fs = fault_get('file_status')       # {match, status, count}: the datastore answers a GET with this status (500 I/O error, 502/504 proxy) count times
        if fs and self.command == 'GET' and fs.get('match', '') in rel and fault_take_inner('file_status'):
            record('fault', 'file_status', rel)
            return self.send_bytes(fs.get('status', 500), b'datastore I/O error', 'text/plain')
        if self.command == 'HEAD' and fault_get('no_head'):
            return self.send_bytes(405, b'', 'text/plain')
        if os.path.isdir(path):                         # a folder: the listing page of the host
            names = sorted(os.listdir(path))
            return self.send_bytes(200, ('<html><body><h1>Index of %s</h1><table>%s</table></body></html>' % (x(rel), ''.join('<tr><td><a href="%s">%s</a></td></tr>' % (x(urllib.parse.quote(n)), x(n)) for n in names))).encode('utf-8'), 'text/html')
        if not os.path.isfile(path):
            return self.send_bytes(404, b'File not found', 'text/plain')
        size = os.path.getsize(path)
        mt = os.path.getmtime(path)
        hodd = fault_get('serve_odd')           # head=True: the HEAD answer is the proxy's HTML error page too
        if hodd and hodd.get('head') and self.command == 'HEAD' and hodd.get('match', '') in rel:
            self.send_response(200); self.send_header('Content-Type', 'text/html'); self.send_header('Content-Length', '137'); self.end_headers()
            return
        odd = fault_get('serve_odd')            # {match, mode: html200 | trunc | zero_len, bytes, times}: a reverse proxy / a damaged datastore answering 200 anyway
        if odd and odd.get('match', '') in rel and self.command == 'GET' and S.counters.get('serve_odd:%s' % odd.get('match'), 0) < odd.get('times', 1 << 30):
            S.counters['serve_odd:%s' % odd.get('match')] = S.counters.get('serve_odd:%s' % odd.get('match'), 0) + 1
            record('fault', 'serve_odd', rel)
            if odd.get('mode') == 'html200':
                return self.send_bytes(200, b'<html><head><title>502 Bad Gateway</title></head><body><h1>Bad Gateway</h1>' + b'x' * 100 + b'</body></html>', 'text/html')
            if odd.get('mode') == 'trunc':
                size = min(size, odd.get('bytes', 100))
        if self.command == 'GET' and (rel.endswith('-rdmp.vmdk') or rel.endswith('-rdm.vmdk')):
            return self.send_bytes(500, b'Internal Server Error: the raw device cannot be read', 'text/plain')
        start, end, status = 0, size - 1, 200
        rng = self.headers.get('Range')
        if rng and not fault_get('no_range') and self.command == 'GET':
            r = rng.replace('bytes=', '').split('-')
            try:
                start = int(r[0]) if r[0] else max(0, size - int(r[1]))
                end = int(r[1]) if len(r) > 1 and r[1] and r[0] else size - 1
            except ValueError:
                return self.send_bytes(400, b'bad range', 'text/plain')
            if start >= size:
                return self.send_bytes(416, b'', 'text/plain', [('Content-Range', 'bytes */%d' % size)])
            end = min(end, size - 1)
            status = 206
            S.ranges.append((rel, rng, 206))
        elif rng:
            S.ranges.append((rel, rng, 200))
        n = end - start + 1
        quirk = (fault_get('http_quirk') or {}).get('mode', '')
        qmatch = (fault_get('http_quirk') or {}).get('match', '')
        if qmatch and qmatch not in rel:
            quirk = ''
        if quirk == 'redirect' and self.command != 'HEAD' or quirk == 'redirect_head':
            # vCenter style: the datastore file is served by the host (307 to an absolute URL, or only a path with a token)
            if 'moved=1' not in (u.query or ''):
                loc = self.path + '&moved=1'
                if (fault_get('http_quirk') or {}).get('absolute'):
                    loc = 'https://%s:%d%s' % ((fault_get('http_quirk') or {}).get('host', os.environ.get('VM2B_SERVER_HOST', '127.0.0.1')), self.server.server_address[1], loc)
                record('fault', 'redirect', rel)
                return self.send_bytes(307, b'', 'text/plain', [('Location', loc)])
        if self.command == 'GET' and quirk == 'slow_start':
            time.sleep((fault_get('http_quirk') or {}).get('ms', 800) / 1000.0)
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        chunked = self.command == 'GET' and quirk == 'chunked'
        closedel = self.command == 'GET' and quirk == 'close_delimited'
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        elif closedel:
            self.send_header('Connection', 'close'); self.close_connection = True
        else:
            self.send_header('Content-Length', str(n))
        if quirk == 'conn_close':
            self.send_header('Connection', 'close'); self.close_connection = True
        self.send_header('Last-Modified', self.date_time_string(mt))
        self.send_header('Accept-Ranges', 'bytes')
        if status == 206:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.end_headers()
        if self.command == 'HEAD':
            return
        if chunked:
            odd = random.Random(n)
            cd = fault_get('drop_after')
            cdrop = cd.get('bytes', 1000) if cd and cd.get('match', '') in rel and n > cd.get('bytes', 1000) and fault_take_inner('drop_after') else None
            with open(path, 'rb') as f:
                f.seek(start); left = n; sent_c = 0
                while left > 0:
                    k = min(left, odd.choice([1, 7, 4096, 65536, 100000]))
                    if cdrop is not None:
                        k = min(k, max(0, cdrop - sent_c))
                        if k == 0:
                            record('fault', 'drop', rel)
                            return self.hang_up()
                    d = f.read(k)
                    if not d:
                        break
                    self.wfile.write(b'%x;ext=1\r\n' % len(d) + d + b'\r\n'); left -= len(d); sent_c += len(d)
            self.wfile.write(b'0\r\nX-Trailer: 1\r\n\r\n')
            return
        drop = fault_get('drop_after')
        drop_at = None
        if drop and drop.get('match', '') in rel and n > drop.get('bytes', 1000) and fault_take_inner('drop_after'):
            drop_at = drop.get('bytes', 1000)
        slow = fault_get('slow_ms', 0)
        sent = 0
        with open(path, 'rb') as f:
            f.seek(start)
            while sent < n:
                k = min(n - sent, 256 << 10)
                if drop_at is not None:
                    k = min(k, max(0, drop_at - sent))
                    if k == 0:
                        self.close_connection = True
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        record('fault', 'drop', rel)
                        return
                if S.dead or tick_event():
                    return self.hang_up()
                data = f.read(k)
                if not data:
                    break
                self.wfile.write(data)
                sent += len(data)
                if slow:
                    time.sleep(slow / 1000.0)
        if sent < n:
            self.close_connection = True

    def handle_expect_100(self):
        # a real host refuses an unauthorized upload before the body is sent
        if self.path.startswith('/folder') and not self.authorized():
            self.send_response(401)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return False
        return super().handle_expect_100()

    def put(self, path, rel, ds):
        n = int(self.headers.get('Content-Length') or 0)

        def drain():
            left = n
            while left > 0:
                d = self.rfile.read(min(left, 1 << 20))
                if not d:
                    break
                left -= len(d)
        pf = fault_get('put_readonly')
        if pf:
            drain(); return self.send_bytes(403, b'Forbidden: the datastore is read only for this license', 'text/plain')
        pf = fault_get('put_503')
        if pf and pf.get('match', '') in rel and fault_take_inner('put_503'):
            drain(); return self.send_bytes(503, b'Service Unavailable', 'text/plain')
        pf = fault_get('put_507')
        if (pf and pf.get('match', '') in rel and fault_take_inner('put_507')) or n > ds_free(ds):
            drain(); return self.send_bytes(507, b'Insufficient Storage', 'text/plain')
        if not os.path.isdir(os.path.dirname(path)):
            drain(); return self.send_bytes(404, b'Not Found', 'text/plain')
        pf = fault_get('put_lost')          # {match, mode: discard | truncate, count}: answered 201 / 200, but the file is not (completely) there
        if pf and pf.get('match', '') in rel and n > 0 and fault_take_inner('put_lost'):
            record('fault', 'put_lost', rel)
            if pf.get('mode') == 'truncate':
                half = n // 2
                with open(path, 'wb') as f:
                    left = n
                    while left > 0:
                        d = self.rfile.read(min(left, 1 << 20))
                        if not d:
                            break
                        left -= len(d)
                        if half > 0:
                            f.write(d[:half]); half -= min(half, len(d))
            else:
                drain()
            return self.send_bytes(201, b'', 'text/plain')
        existed = os.path.exists(path)
        drop = fault_get('put_drop')
        drop_at = drop.get('bytes', 1000) if (drop and drop.get('match', '') in rel and n > drop.get('bytes', 1000) and fault_take_inner('put_drop')) else None
        left = n; got = 0
        with open(path, 'wb') as f:
            while left > 0:
                k = min(left, 256 << 10)
                if drop_at is not None:
                    k = min(k, max(0, drop_at - got))
                    if k == 0:
                        self.close_connection = True
                        try:
                            self.connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        record('fault', 'put-drop', rel)
                        return
                if S.dead or tick_event():
                    return self.hang_up()
                d = self.rfile.read(k)
                if not d:
                    break
                f.write(d); left -= len(d); got += len(d)
                if fault_get('slow_ms'):
                    time.sleep(fault_get('slow_ms') / 1000.0)
        if left > 0:
            self.close_connection = True
            return
        record('put', 'done', rel)
        rw = fault_get('raw_http')
        if rw and rw.get('target') == 'put' and fault_take_inner('raw_http'):
            return self.raw_out(rw.get('kind', 0))      # the file is stored, the answer is garbage
        self.send_bytes(200 if existed else 201, b'', 'text/plain')

    # ---- control API
    def control(self, body):
        cmd = self.path[len('/__mock/'):].split('?')[0]
        try:
            a = json.loads(body.decode('utf-8')) if body else {}
        except ValueError:
            return self.send_bytes(400, b'bad json', 'text/plain')
        out = control(cmd, a, urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query))
        self.send_bytes(200, json.dumps(out).encode('utf-8'), 'application/json')


def fault_take_inner(name):
    """faults given as {'count': n, ...}: count -1 = always"""
    with LOCK:
        v = S.faults.get(name)
        if not v:
            return False
        c = v.get('count', -1)
        if c == 0:
            return False
        if c > 0:
            v['count'] = c - 1
        return True


def state_json():
    with LOCK:
        vms = {}
        for vm in S.vms.values():
            vms[vm.name] = dict(id=vm.id, power=vm.power, snapshots=vm_snapshot_names(vm), dir=vm.dir, ds=vm.ds, dc=vm.dc, files=sorted(vm.files))
        leaked = [(v.name, s.name) for v in S.vms.values() for s in v.snaps if s.name.startswith('VM2Backup-')]
        return dict(vms=vms, dead=S.dead, evn=S.evn, sessions=len(S.sessions), leaked=leaked, events=S.events[-200:], counters=S.counters, soap_calls=S.soap_calls, log_len=len(S.log),
                    ranges=S.ranges[-200:], faults={k: v for k, v in S.faults.items()}, profile=S.prof['api'], kind=S.kind)


def control(cmd, a, q):
    with LOCK:
        if cmd == 'state':
            return state_json()
        if cmd == 'log':
            since = int((q.get('since') or ['0'])[0])
            return dict(log=S.log[since:], next=len(S.log))
        if cmd == 'create_vm':
            vm = create_vm(a)
            return dict(id=vm.id, dir=vm.dir, ds=vm.ds, path=vm_path(vm))
        if cmd == 'delete_vm':
            vm = next((v for v in S.vms.values() if v.name == a['name']), None)
            if vm:
                import shutil
                shutil.rmtree(vm_path(vm), ignore_errors=True)
                del S.vms[vm.id]
            return dict(ok=bool(vm))
        if cmd == 'faults':
            for k, v in a.items():
                if v is None:
                    S.faults.pop(k, None)
                else:
                    S.faults[k] = v
            return dict(faults=S.faults)
        if cmd == 'reset':
            S.faults.clear(); S.log.clear(); S.counters.clear(); S.events.clear(); S.ranges.clear(); S.sessions.clear(); S.dead = False; S.evn = 0
            return dict(ok=True)
        if cmd == 'revive':
            S.dead = False; S.evn = 0
            return dict(ok=True)
        if cmd == 'expire':
            n = len(S.sessions); S.sessions.clear()
            return dict(expired=n)
        if cmd == 'guestwrite':
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            disk = vm.disks[a.get('disk', 0)]
            data = random.Random(a.get('seed', 1)).randbytes(a.get('length', 4096))
            apply_write(vm, disk, a.get('offset', 0), data)
            return dict(ok=True, level=len(disk.levels) - 1)
        if cmd == 'power':
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            vm.power = a['state']
            return dict(ok=True)
        if cmd == 'snapshot':       # an instant snapshot (a user's own, or a leftover of a crashed backup)
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            sn = make_snapshot(vm, a['name'], a.get('desc', ''), False, False)
            S.events.append(('create', vm.name, sn.name))
            return dict(ref=sn.ref)
        if cmd == 'revert':         # revert to a snapshot: the running delta is dropped and a new one starts on the snapshot's chain (the tree branches at the next snapshot)
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            sn = next((s for s in vm.snaps if s.name == a['name']), None)
            if not sn:
                return dict(ok=False)
            for di, disk in enumerate(vm.disks):
                if getattr(disk, 'indep', False):
                    continue
                for lv in disk.levels:
                    if all(lv not in other.chains[di] for other in vm.snaps):
                        unregister(vm, lv.desc); unregister(vm, lv.ext)
                disk.snapn += 1
                n = disk.snapn
                ext = '%s-%06d-%s.vmdk' % (disk.name, n, S.prof['delta'])
                lv = Level('%s-%06d.vmdk' % (disk.name, n), ext, False)
                register(vm, ext, 'diskExtent', disk.ds)
                with open(vm_path(vm, ext), 'wb') as f:
                    f.write(b'\0' * 512)
                write_text(vm, lv.desc, descriptor_text(disk, lv, sn.chains[di][-1].desc), 'diskDescriptor', disk.ds)
                disk.levels = list(sn.chains[di]) + [lv]
            vm.cur = sn; vm.branched = True
            rewrite_vmsd(vm); write_vmx(vm)
            return dict(ok=True)
        if cmd == 'remove_snapshot':
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            sn = next((s for s in vm.snaps if s.name == a['name']), None)
            if sn:
                remove_snapshot(vm, sn, True)
                S.events.append(('remove', vm.name, sn.name))
            return dict(ok=bool(sn))
        if cmd == 'unlink':         # the file vanishes from the datastore (storage vMotion, a cleanup by hand) while layoutEx still lists it
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            names = [n for n in vm.files if n == a['file'] or (a.get('suffix') and n.endswith(a['file']))]
            for n in names:
                try:
                    os.remove(vm_path(vm, n))
                except OSError:
                    pass
            return dict(ok=bool(names), names=names)
        if cmd == 'touch':          # modifies a file (the VM "changed")
            vm = next(v for v in S.vms.values() if v.name == a['vm'])
            if a['file'] == vm.vmx:
                vm.vmx_extra.append('guestOS.touched%d = "TRUE"' % len(vm.vmx_extra)); write_vmx(vm)
            else:
                with open(vm_path(vm, a['file']), 'ab') as f:
                    f.write(b'changed')
            return dict(ok=True)
    return dict(error='unknown command ' + cmd)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    request_queue_size = 128
    allow_reuse_address = True

    def get_request(self):
        sock, addr = self.socket.accept()
        return sock, addr

    def finish_request(self, request, client_address):
        try:
            request = self.ctx.wrap_socket(request, server_side=True)
        except (ssl.SSLError, OSError):
            try:
                request.close()
            except OSError:
                pass
            return
        super().finish_request(request, client_address)


def make_cert(d, host, mode=''):
    pre = os.environ.get('VM2B_PREMADE_CERTS')
    if pre:                                    # гость Windows: нет модуля cryptography, сертификаты заранее сделаны на хосте (make_premade_certs.py)
        n = os.path.join(pre, mode or 'std')
        return n + '.pem', n + '.key', open(n + '.fp').read().strip()
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    import ipaddress
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host)])
    now = datetime.datetime.now(datetime.timezone.utc)
    nb, na = now - datetime.timedelta(days=1), now + datetime.timedelta(days=30)
    san = [x509.DNSName('localhost'), x509.IPAddress(ipaddress.ip_address('127.0.0.1'))] + ([x509.IPAddress(ipaddress.ip_address(os.environ['VM2B_SERVER_HOST']))] if os.environ.get('VM2B_SERVER_HOST') else [])
    if mode == 'expired':
        nb, na = now - datetime.timedelta(days=400), now - datetime.timedelta(days=35)
    elif mode == 'wrongcn':
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'esxi-other.example.com')]); san = [x509.DNSName('esxi-other.example.com')]
    elif mode == 'sanip':                      # no common name at all, only the IP in the SAN (what recent ESXi / vCenter generate for an IP-only host)
        name = x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, 'VMware')]); san = [x509.IPAddress(ipaddress.ip_address(os.environ.get('VM2B_SERVER_HOST', '127.0.0.1')))]
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(nb).not_valid_after(na)
            .add_extension(x509.SubjectAlternativeName(san), critical=False)
            .sign(key, hashes.SHA256()))
    cp, kp = os.path.join(d, 'mock.pem'), os.path.join(d, 'mock.key')
    open(cp, 'wb').write(cert.public_bytes(serialization.Encoding.PEM))
    open(kp, 'wb').write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
    return cp, kp, hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--profile', default='7.0'); ap.add_argument('--kind', default='esxi'); ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--root', required=True); ap.add_argument('--user', default='root'); ap.add_argument('--password', default='pw')
    ap.add_argument('--info', required=True); ap.add_argument('--tls-only'); ap.add_argument('--dcs', default='')
    ap.add_argument('--seed', type=int, default=0); ap.add_argument('--cert', default='')
    a = ap.parse_args()
    random.seed(a.seed or None)
    S.prof = PROFILES[a.profile]; S.kind = a.kind; S.root = os.path.abspath(a.root); S.user = a.user; S.password = a.password
    if a.kind == 'esxi':
        S.dcs = [('ha-datacenter', 'ha-datacenter')]
    else:
        names = [n for n in a.dcs.split(',') if n] or ['Datacenter']
        S.dcs = [('datacenter-%d' % (i + 2), n) for i, n in enumerate(names)]
    build_inventory()
    os.makedirs(S.root, exist_ok=True)
    for ds in DS_IDS:
        os.makedirs(os.path.join(S.root, ds), exist_ok=True)
    d = os.path.join(S.root, '.mock'); os.makedirs(d, exist_ok=True)
    cp, kp, fp = make_cert(d, 'localhost', a.cert)
    vers = {'1.0': ssl.TLSVersion.TLSv1, '1.1': ssl.TLSVersion.TLSv1_1, '1.2': ssl.TLSVersion.TLSv1_2, '1.3': ssl.TLSVersion.TLSv1_3}
    lo, hi = S.prof['tls']
    if a.tls_only:
        lo = hi = a.tls_only
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cp, kp)
    if lo in ('1.0', '1.1'):
        ctx.set_ciphers('ALL:@SECLEVEL=0')
    ctx.minimum_version = vers[lo]; ctx.maximum_version = vers[hi]
    srv = Server((os.environ.get('VM2B_BIND_ADDR', '127.0.0.1'), a.port), Handler)
    srv.ctx = ctx
    with open(a.info, 'w') as f:
        json.dump(dict(fingerprint='sha256:' + fp, port=a.port, profile=a.profile, kind=a.kind, api=S.prof['api'], tls=[lo, hi], dcs=[x[1] for x in S.dcs]), f)
    srv.serve_forever()


if __name__ == '__main__':
    main()
