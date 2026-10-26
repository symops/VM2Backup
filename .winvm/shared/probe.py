import ssl, http.client, json, time, sys, glob
info = json.load(open(r'C:\vm2b\esxi\servers.json'))['esxi'][0]
c = ssl._create_unverified_context()
t0 = time.time()
h = http.client.HTTPSConnection('127.0.0.1', info['port'], context=c, timeout=300)
h.request('POST', '/__mock/create_vm', json.dumps({'name': 'PROBE', 'disks': [{'size': 1073741824, 'thin': True, 'seed': 77}], 'power': 'poweredOn', 'ds': 'datastore1', 'template': False, 'conn': 'connected', 'seed': 1}), {'Content-Type': 'application/json'})
r = h.getresponse(); b = r.read()
print('create_vm', r.status, round(time.time() - t0, 1), 's', b[:300])
