#!/usr/bin/env python3
# Раунд 4: что отвечает интерфейс /folder vCenter пользователю с ролью на малых сущностях: GET каталога ВМ, HEAD .vmx, PUT и DELETE пробного файла.
#   r4-folder-probe.py <пользователь SSO без домена> <датастор> <каталог ВМ> [секрет-файл]     (пароль не печатается)
import base64, os, ssl, sys, urllib.request, urllib.error
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.winvm', 'esxi')
u, ds, d = sys.argv[1:4]; sec = sys.argv[4] if len(sys.argv) > 4 else 'secret-vc-' + u
pw = open(os.path.join(ROOT, sec)).read().strip()
auth = 'Basic ' + base64.b64encode(('%s@vsphere.local:%s' % (u, pw)).encode()).decode()
ctx = ssl._create_unverified_context()
def req(m, path, data=None):
    r = urllib.request.Request('https://10.213.77.11/folder/%s?dcPath=vm2b-dc&dsName=%s' % (path, ds), data=data, method=m, headers={'Authorization': auth})
    try: return urllib.request.urlopen(r, context=ctx, timeout=60).status
    except urllib.error.HTTPError as e: return e.code
print('GET каталога', req('GET', d + '/'), '| HEAD .vmx', req('HEAD', d + '/' + d + '.vmx'), '| PUT пробы', req('PUT', d + '/r4probe.txt', b'x'), '| DELETE пробы', req('DELETE', d + '/r4probe.txt'))
