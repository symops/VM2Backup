# Shared helpers of the VM2Backup test servers: test certificates, fingerprints, deterministic data.
import datetime, hashlib, ipaddress, os, ssl
BIND = os.environ.get('VM2B_BIND_ADDR', '127.0.0.1')   # where the test servers listen (default loopback; the Windows VM harness: the Docker bridge address)
HOST = os.environ.get('VM2B_SERVER_HOST', BIND)        # the address a client must use to reach them (goes into certificates and servers.json)
NAME = os.environ.get('VM2B_SERVER_NAME', 'localhost')  # the host NAME the certificates are made for (the Windows VM: a name its hosts file maps to the bridge address)


def guest_path(p):
    """A host path as the test program sees it: VM2B_GUEST_MAP=<host prefix>=<guest prefix> (the Windows VM reaches the host folder as a UNC path); default: Z:<path> for Wine."""
    m = os.environ.get('VM2B_GUEST_MAP', '')
    if m:
        hp, gp = m.split('=', 1)
        if p == hp or p.startswith(hp.rstrip('/') + '/'):
            return gp.rstrip('\\') + p[len(hp.rstrip('/')):].replace('/', '\\')
    return 'Z:' + p.replace('/', '\\')
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID


def _key():
    return rsa.generate_private_key(public_exponent=65537, key_size=2048)


def _write(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def _pem_cert(c):
    return c.public_bytes(serialization.Encoding.PEM)


def _pem_key(k):
    return k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption())


def _cert(subject_cn, issuer_cert, issuer_key, key, sans, is_ca=False, days=30, start_days=-1):
    now = datetime.datetime.now(datetime.timezone.utc)
    subj = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, subject_cn)])
    issuer = issuer_cert.subject if issuer_cert is not None else subj
    b = (x509.CertificateBuilder().subject_name(subj).issuer_name(issuer).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=start_days))
         .not_valid_after(now + datetime.timedelta(days=days)))
    b = b.add_extension(x509.BasicConstraints(ca=is_ca, path_length=None), critical=True)
    if sans:
        names = []
        for s in sans:
            try:
                names.append(x509.IPAddress(ipaddress.ip_address(s)))
            except ValueError:
                names.append(x509.DNSName(s))
        b = b.add_extension(x509.SubjectAlternativeName(names), critical=False)
    if not is_ca:
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    return b.sign(issuer_key or key, hashes.SHA256())


def make_certs(d):
    """Creates in directory d: ca.pem; server (CA signed, SAN localhost/127.0.0.1); selfsigned; wrongname (CA signed, SAN other.example);
    expired (CA signed, expired).  Returns {name: (cert_path, key_path, sha256_hex)}."""
    os.makedirs(d, exist_ok=True)
    ca_key = _key()
    ca = _cert('VM2B Test CA', None, None, ca_key, [], is_ca=True, days=60)
    _write(os.path.join(d, 'ca.pem'), _pem_cert(ca))
    out = {}

    def emit(name, cert, key):
        cp, kp = os.path.join(d, name + '.pem'), os.path.join(d, name + '.key')
        _write(cp, _pem_cert(cert) if name != 'server' else _pem_cert(cert))
        _write(kp, _pem_key(key))
        out[name] = (cp, kp, hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest())

    k = _key(); emit('server', _cert(NAME, ca, ca_key, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else [])), k)
    k = _key(); emit('selfsigned', _cert(NAME, None, None, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else [])), k)
    k = _key(); emit('wrongname', _cert('other.example', ca, ca_key, k, ['other.example']), k)
    # the certificate matrix of the tls tests
    k = _key(); emit('expired', _cert(NAME, ca, ca_key, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else []), days=-1, start_days=-60), k)
    k = _key(); emit('notyet', _cert(NAME, ca, ca_key, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else []), days=30, start_days=2), k)
    k = _key(); emit('sanip', _cert(HOST, ca, ca_key, k, [HOST]), k)
    k = _key(); emit('cnonly', _cert(NAME, ca, ca_key, k, []), k)
    ik = _key(); inter = _cert('VM2B Test Intermediate', ca, ca_key, ik, [], is_ca=True, days=50)
    k = _key(); leaf = _cert(NAME, inter, ik, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else []))
    emit('chainleaf', leaf, k)                                  # the leaf alone: the intermediate is missing (partial chain)
    _write(os.path.join(d, 'chain.pem'), _pem_cert(leaf) + _pem_cert(inter)); _write(os.path.join(d, 'chain.key'), _pem_key(k))
    out['chain'] = (os.path.join(d, 'chain.pem'), os.path.join(d, 'chain.key'), hashlib.sha256(leaf.public_bytes(serialization.Encoding.DER)).hexdigest())
    # a CA that is NOT the test CA signs a server certificate for the right name (the bundle does not contain it)
    ok = _key(); other = _cert('Other Root', None, None, ok, [], is_ca=True, days=60)
    k = _key(); emit('othercaleaf', _cert(NAME, other, ok, k, [NAME, '127.0.0.1'] + ([HOST] if HOST != '127.0.0.1' else [])), k)
    return out


def tls_context(cert, key, vmin=None, vmax=None, seclevel0=False):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    if seclevel0:
        ctx.set_ciphers('ALL:@SECLEVEL=0')
    vers = {'1.0': ssl.TLSVersion.TLSv1, '1.1': ssl.TLSVersion.TLSv1_1, '1.2': ssl.TLSVersion.TLSv1_2, '1.3': ssl.TLSVersion.TLSv1_3}
    if vmin:
        ctx.minimum_version = vers[vmin]
    if vmax:
        ctx.maximum_version = vers[vmax]
    return ctx


_BLOCK = bytes((i * 131 + 17) & 0xff for i in range(65536))


def pattern(offset, n):
    """Deterministic bytes [offset, offset+n) of an endless stream (cheap to produce, not trivially compressible)."""
    out = bytearray()
    while n > 0:
        o = offset % 65536
        k = min(n, 65536 - o)
        out += _BLOCK[o:o + k]
        offset += k
        n -= k
    return bytes(out)
