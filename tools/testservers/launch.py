# Starts every test server of the FTP / TLS / HTTP tests of VM2Backup on free ports of 21000-21999, writes the --servers json and a file
# with the PIDs of the servers (the caller stops them by PID).   usage: launch.py <rundir> <servers.json> <pids-file> [seed]
import hashlib, json, os, random, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import net_common as common
from cryptography import x509
from cryptography.hazmat.primitives import serialization

run, out_json, pid_file = sys.argv[1:4]
rnd = random.Random(int(sys.argv[4]) if len(sys.argv) > 4 else None)
os.makedirs(run, exist_ok=True)
certdir = os.path.join(run, 'certs')
common.make_certs(certdir)


def free(port):
    s = socket.socket()
    try:
        s.bind((common.BIND, port)); return True
    except OSError:
        return False
    finally:
        s.close()


used = set()


def take(n=1):
    """n consecutive free ports inside 21000-21999"""
    for _ in range(2000):
        p = rnd.randint(21000, 21999 - n)
        if all(q not in used and free(q) for q in range(p, p + n)):
            used.update(range(p, p + n)); return p
    raise SystemExit('no free ports in 21000-21999')


def fp(name):
    der = x509.load_pem_x509_certificate(open(os.path.join(certdir, name + '.pem'), 'rb').read()).public_bytes(serialization.Encoding.DER)
    return 'sha256:' + hashlib.sha256(der).hexdigest()


def wine(p):
    return common.guest_path(p)


py = sys.executable
procs = []


def start(args, log):
    f = open(os.path.join(run, log), 'wb')
    p = subprocess.Popen([py] + args, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
    procs.append(p)
    with open(pid_file, 'a') as pf: pf.write('%d\n' % p.pid)      # at once: a failed start must not leave servers behind
    return p


closed = take()                      # reserved, never bound: "connection refused"
servers = {}
# --- TLS echo
tp = {k: take() for k in ('port', 'port10', 'port13', 'port_self', 'port_wrong', 'port_starttls', 'port_expired', 'port_notyet', 'port_sanip', 'port_cnonly', 'port_chain', 'port_chainleaf', 'port_othercaleaf')}
start([os.path.join(HERE, 'tls_echo.py'), certdir,
       '%d:server:-:-' % tp['port'], '%d:server:1.0:1.0' % tp['port10'], '%d:server:1.3:1.3' % tp['port13'],
       '%d:selfsigned:-:-' % tp['port_self'], '%d:wrongname:-:-' % tp['port_wrong'], '%d:server:-:-:starttls' % tp['port_starttls'],
       '%d:expired:-:-' % tp['port_expired'], '%d:notyet:-:-' % tp['port_notyet'], '%d:sanip:-:-' % tp['port_sanip'], '%d:cnonly:-:-' % tp['port_cnonly'],
       '%d:chain:-:-' % tp['port_chain'], '%d:chainleaf:-:-' % tp['port_chainleaf'], '%d:othercaleaf:-:-' % tp['port_othercaleaf']], 'tls.log')
servers['tls'] = dict(host=common.NAME if (common.HOST == '127.0.0.1' or common.NAME != 'localhost') else common.HOST, ip=common.HOST, closed_port=closed, fp=fp('server'), fp_self=fp('selfsigned'), fp_expired=fp('expired'), fp_chain=fp('chain'), ca_file=wine(os.path.join(certdir, 'ca.pem')), **tp)
# --- HTTP
hp, hps = take(), take()
start([os.path.join(HERE, 'http_server.py'), certdir, str(hp), '%d:tls:server' % hps], 'http.log')
servers['http'] = dict(host=common.HOST, port=hp, tls_port=hps, closed_port=closed, fp=fp('server'), ca_file=wine(os.path.join(certdir, 'ca.pem')))
# --- FTP family
user, pw = 'vm2b', 'pw-%d-ä' % rnd.randint(1000, 9999)
ftproot = os.path.join(run, 'ftproot')
variants = [('ftp', 'plain', []), ('ftpes', 'ftpes', []), ('ftps', 'ftps', []),
            ('ftp_private', 'plain', ['--masquerade', '10.99.88.77']), ('ftp_noepsv', 'plain', ['--no-epsv']),
            ('ftp_mlsd', 'plain', ['--list', 'mlsd']), ('ftp_list', 'plain', ['--list', 'list']), ('ftp_idle', 'plain', ['--idle', '3'])]
for key, mode, extra in variants:
    port = take(); pasv = take(24)
    root = os.path.join(ftproot, key); os.makedirs(root, exist_ok=True)
    start([os.path.join(HERE, 'ftp_server.py'), '--port', str(port), '--mode', mode, '--root', root, '--user', user, '--password', pw,
           '--certdir', certdir, '--pasv', '%d-%d' % (pasv, pasv + 23)] + extra, key + '.log')
    e = dict(host=common.HOST, port=port, user=user, password=pw, dir='bk', root=wine(root))
    if mode != 'plain':
        e['fingerprint'] = fp('server')
    servers[key] = e
# --- quirk servers (ftp_quirk.py): one process, one listener per profile; "expect" is what connect() must do
QUIRKS = [   # name, mode, quirks, expect (ok | perm | transient | hostile)
    ('bare', 'plain', 'nofeat,nomlsd,nosize,nomdtm,nonlst,list=unix', 'ok'),
    ('iis', 'plain', 'list=dos,nonlst,nomlsd,utf8off,banner=multi', 'ok'),
    ('eplf', 'plain', 'list=eplf,nonlst,nomlsd,nofeat', 'ok'),
    ('epsv_refuse', 'plain', 'epsv=refuse,pasv=zero', 'ok'),
    ('epsv_garbage', 'plain', 'epsv=garbage,pasv=nat172', 'ok'),
    ('pasv_nat', 'plain', 'pasv=nat,nosize', 'ok'),
    ('nlst_paths', 'plain', 'nlst=paths,nomlsd', 'ok'),
    ('nlst_dot', 'plain', 'nlst=dotpaths,nomlsd', 'ok'),
    ('nlst450', 'plain', 'nlst=empty450,nomlsd', 'ok'),
    ('noover', 'plain', 'rnto_noover,stor_noover', 'ok'),
    ('banner120', 'plain', 'banner=120,nomdtm', 'ok'),
    ('banner_spaces', 'plain', 'banner=spaces', 'ok'),
    ('nopass', 'plain', 'login=nopass', 'ok'),
    ('slow', 'plain', 'slow=2000000', 'ok'),
    ('es_order', 'ftpes', 'pbsz=order,nosize', 'ok'),
    ('es_reuse', 'ftpes', 'reuse', 'ok'),
    ('s_reuse', 'ftps', 'reuse', 'ok'),
    ('es_nonotify', 'ftpes', 'nonotify', 'ok'),
    ('es_slowread', 'ftpes', 'slowread=40,hupend', 'ok'),
    ('s_slowread', 'ftps', 'slowread=40,hupend,nosize', 'ok'),
    ('s_nonotify', 'ftps', 'nonotify,list=dos,nonlst,nomlsd', 'ok'),
    ('big', 'plain', 'bigdir=50000', 'ok'),
    ('lines5k', 'plain', 'hostile=lines5k', 'ok'),
    ('pasv_public', 'plain', 'pasv=public,epsv=refuse', 'ok'),
    ('size_long', 'plain', 'sizelie=long', 'data'),
    ('size_short', 'plain', 'sizelie=short', 'data'),
    ('listbomb', 'plain', 'listbomb', 'data'),
    ('pasv_dead', 'plain', 'pasv=dead,epsv=refuse', 'data'),
    ('acct', 'plain', 'login=acct', 'perm'),
    ('busy421', 'plain', 'login=busy421', 'transient'),
    ('denied', 'plain', 'login=denied', 'perm'),
    ('banner421', 'plain', 'banner=421', 'transient'),
    ('banner554', 'plain', 'banner=554', 'perm'),
    ('prot_refuse', 'ftpes', 'prot=refuse', 'perm'),
    ('h_garbage', 'plain', 'hostile=garbage', 'hostile'),
    ('h_hugeline', 'plain', 'hostile=hugeline', 'hostile'),
    ('h_endless', 'plain', 'hostile=endless', 'hostile'),
    ('h_lines100k', 'plain', 'hostile=lines100k', 'hostile'),
    ('h_nocrlf', 'plain', 'hostile=nocrlf', 'hostile'),
    ('h_dribble', 'plain', 'hostile=dribble', 'hostile'),
    ('h_silent', 'plain', 'hostile=silent', 'hostile'),
    ('h_replyloop', 'plain', 'hostile=replyloop', 'hostile'),
    ('h_close', 'plain', 'hostile=bannerthenclose', 'hostile'),
]
qroot = os.path.join(run, 'ftpq')
qspecs, qinfo = [], []
for name, mode, quirks, expect in QUIRKS:
    qp = take()
    qspecs += ['--spec', '%s:%d:%s:%s' % (name, qp, mode, quirks)]
    e = dict(name=name, port=qp, type=mode, expect=expect, quirks=quirks)
    if mode != 'plain': e['fingerprint'] = fp('server')
    qinfo.append(e)
start([os.path.join(HERE, 'ftp_quirk.py'), '--root', qroot, '--user', user, '--password', pw, '--certdir', certdir] + qspecs, 'ftpq.log')
servers['ftpq'] = dict(host=common.HOST, user=user, password=pw, root=wine(qroot), profiles=qinfo)
servers['closed_port'] = closed
servers['ca_file'] = wine(os.path.join(certdir, 'ca.pem'))
# wait until everything accepts connections
ports = [tp[k] for k in tp] + [hp, hps] + [servers[k]['port'] for k, _, _ in variants] + [e['port'] for e in qinfo]
t0 = time.time()
for p in ports:
    while True:
        try:
            socket.create_connection((common.BIND, p), 0.5).close(); break
        except OSError:
            if time.time() - t0 > 120:
                raise SystemExit('server on port %d did not start (see logs in %s)' % (p, run))
            time.sleep(0.1)
with open(out_json, 'w') as f:
    json.dump(servers, f, indent=1)
