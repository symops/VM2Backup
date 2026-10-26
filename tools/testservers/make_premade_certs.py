# Делает на хосте сертификаты для esxi_mock.py, который работает в госте Windows без модуля cryptography.
# usage: make_premade_certs.py <каталог>   -> std/expired/wrongcn/sanip: .pem .key .fp (sha256 DER)
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.pop('VM2B_PREMADE_CERTS', None)
import esxi_mock
d = sys.argv[1]; os.makedirs(d, exist_ok=True)
for mode in ('', 'expired', 'wrongcn', 'sanip'):
    cp, kp, fp = esxi_mock.make_cert(d, 'localhost', mode)
    n = os.path.join(d, mode or 'std')
    os.replace(cp, n + '.pem'); os.replace(kp, n + '.key'); open(n + '.fp', 'w').write(fp + '\n')
