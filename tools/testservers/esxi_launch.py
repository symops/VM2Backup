# Starts one ESXi / vCenter emulator per version profile (6.0 .. 9.0, each as esxi and as vcenter, plus an ESXi 6.0 that speaks TLS 1.0 only)
# on free ports of 23000-23999 and writes the --servers json and a file with the PIDs.   usage: esxi_launch.py <rundir> <servers.json> <pids-file> [seed]
import json, os, random, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
run, out_json, pid_file = sys.argv[1:4]
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else None)
os.makedirs(run, exist_ok=True)


def free(p):
    s = socket.socket()
    try:
        s.bind((os.environ.get('VM2B_BIND_ADDR', '127.0.0.1'), p)); return True
    except OSError:
        return False
    finally:
        s.close()


used = set()


LO, HI = [int(x) for x in os.environ.get('VM2B_ESXI_PORTS', '23000-23999').split('-')]      # e.g. VM2B_ESXI_PORTS=24500-24999


def take():
    for _ in range(2000):
        p = rnd.randint(LO, HI)
        if p not in used and free(p):
            used.add(p); return p
    raise SystemExit('no free ports in %d-%d' % (LO, HI))


sys.path.insert(0, HERE)
if os.name == 'nt':                            # запуск внутри гостя Windows: данные на его локальном диске, без UNC и без модуля cryptography
    def wine(p):
        return p
else:
    import net_common as _nc

    def wine(p):
        return _nc.guest_path(p)


specs = []
for prof in ('6.0', '6.5', '6.7', '7.0', '8.0', '9.0'):
    for kind in ('esxi', 'vcenter'):
        specs.append(dict(profile=prof, kind=kind, name='%s-%s' % (prof, kind), tls_only=None))
specs.append(dict(profile='6.0', kind='esxi', name='6.0-esxi-tls10only', tls_only='1.0'))
for prof, mode in (('7.0', 'expired'), ('8.0', 'wrongcn'), ('8.0', 'sanip')):       # certificate oddities (own tests; a pinned fingerprint must still work)
    specs.append(dict(profile=prof, kind='esxi', name='%s-esxi-cert-%s' % (prof, mode), tls_only=None, cert=mode))
procs, entries = [], []
pw = 'pw-%d-ä' % rnd.randint(1000, 9999)
with open(pid_file, 'w') as pf:
    for s in specs:
        port = take(); d = os.path.join(run, s['name']); os.makedirs(d, exist_ok=True)
        info = os.path.join(d, 'info.json')
        cmd = [sys.executable, '-I', os.path.join(HERE, 'esxi_mock.py'), '--profile', s['profile'], '--kind', s['kind'], '--port', str(port), '--root', os.path.join(d, 'ds'),
               '--user', 'administrator@vsphere.local' if s['kind'] == 'vcenter' else 'root', '--password', pw, '--info', info, '--seed', str(rnd.randint(1, 10 ** 6))]
        if s['kind'] == 'vcenter':
            # datacenter names as real sites have them: spaces, non-ASCII, a percent sign (they go into /folder?dcPath=... and into SOAP)
            cmd += ['--dcs', {'7.0': 'Data Center 1,\u0414\u0430\u0442\u0430-\u0446\u0435\u043d\u0442\u0440 2', '9.0': 'DC 100%,East & West (\u00fc)'}.get(s['profile'], 'DC1,DC2')]
        if s['tls_only']:
            cmd += ['--tls-only', s['tls_only']]
        if s.get('cert'):
            cmd += ['--cert', s['cert']]
        f = open(os.path.join(d, 'mock.log'), 'wb')
        p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, **({} if os.name == 'nt' else {'start_new_session': True}))
        procs.append(p); pf.write('%d\n' % p.pid); pf.flush()
        s.update(port=port, info=info, root=os.path.join(d, 'ds'))
for s in specs:
    for _ in range(100):
        if os.path.exists(s['info']) and os.path.getsize(s['info']) > 5:
            break
        time.sleep(0.1)
    else:
        raise SystemExit('mock %s did not start (see its mock.log)' % s['name'])
    i = json.load(open(s['info']))
    entries.append(dict(profile=s['profile'], name=s['name'], kind=s['kind'], version=i['api'], host=os.environ.get('VM2B_SERVER_HOST', os.environ.get('VM2B_BIND_ADDR', '127.0.0.1')), port=s['port'],
                        user='administrator@vsphere.local' if s['kind'] == 'vcenter' else 'root', password=pw, fingerprint=i['fingerprint'],
                        tls_min='1.0' if (s['profile'] in ('6.0', '6.5')) else '1.2', root=wine(s['root']), dcs=i['dcs'], tls_only=s['tls_only'] or '', cert=s.get('cert', '')))
json.dump(dict(esxi=entries), open(out_json, 'w'), indent=1)
