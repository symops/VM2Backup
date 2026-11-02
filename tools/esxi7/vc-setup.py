#!/usr/bin/env python3
# Настройка vCenter (VCSA) после первой загрузки: датацентр vm2b-dc и в нём ESXi-хост 10.213.77.10 (SOAP API, как это делает клиент vSphere).  Пароли — из файлов
# .winvm/esxi/secret-vc-sso (administrator@vsphere.local) и .winvm/esxi/secret (root хоста), нигде не печатаются.  Лицензия не вводится (evaluation 60 дней).
#   tools/esxi7/vc-setup.py
import hashlib, http.cookiejar, os, re, ssl, sys, time, urllib.request
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.winvm', 'esxi')
sso = open(os.path.join(root, 'secret-vc-sso')).read().strip(); hostpw = open(os.path.join(root, 'secret')).read().strip()
VC, HOST = '10.213.77.11', '10.213.77.10'
ctx = ssl._create_unverified_context(); cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), urllib.request.HTTPCookieProcessor(cj))
def call(m, inner, ver='7.0'):
    env = ('<?xml version="1.0" encoding="UTF-8"?><soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
           'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><soapenv:Body><%s xmlns="urn:vim25">%s</%s></soapenv:Body></soapenv:Envelope>' % (m, inner, m))
    r = urllib.request.Request('https://%s/sdk' % VC, env.encode(), {'Content-Type': 'text/xml', 'SOAPAction': '"urn:vim25/%s"' % ver})
    try: return op.open(r, timeout=120).read().decode()
    except urllib.error.HTTPError as e: return e.read().decode()
sc = call('RetrieveServiceContent', '<_this type="ServiceInstance">ServiceInstance</_this>')
g = lambda t: re.search('<%s[^>]*>([^<]*)</%s>' % (t, t), sc).group(1)
r = call('Login', '<_this type="SessionManager">%s</_this><userName>administrator@vsphere.local</userName><password>%s</password>' % (g('sessionManager'), sso))
if 'Fault' in r: sys.exit('вход в vCenter не удался')
root_folder = g('rootFolder')
def wait(task):
    for _ in range(300):
        s = call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>Task</type><pathSet>info.state</pathSet><pathSet>info.error</pathSet></propSet><objectSet><obj type="Task">%s</obj></objectSet></specSet><options/>' % (g('propertyCollector'), task))
        if '>success<' in s: return True
        if '>error<' in s: print('ошибка задачи:', re.sub(r'\s+', ' ', re.findall(r'<localizedMessage>([^<]*)', s)[:1] and re.findall(r'<localizedMessage>([^<]*)', s)[0] or s[:300])); return False
        time.sleep(2)
    return False
pc = g('propertyCollector')
def datacenters():
    s = call('RetrieveProperties', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>Datacenter</type><pathSet>name</pathSet><pathSet>hostFolder</pathSet></propSet>'
             '<objectSet><obj type="Folder">%s</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>t</name><type>Folder</type><path>childEntity</path><skip>false</skip></selectSet></objectSet></specSet>' % (pc, root_folder))
    out = {}
    for m in re.finditer(r'<returnval>(.*?)</returnval>', s, re.S):
        b = m.group(1); n = re.search(r'<name>name</name><val[^>]*>([^<]*)', b); h = re.search(r'<name>hostFolder</name><val[^>]*>([^<]*)', b)
        if n and h: out[n.group(1)] = h.group(1)
    return out
dcs = datacenters()
if 'vm2b-dc' not in dcs:
    r = call('CreateDatacenter', '<_this type="Folder">%s</_this><name>vm2b-dc</name>' % root_folder)
    if 'Fault' in r: print('датацентр:', re.findall(r'<faultstring>([^<]*)', r)[:1]); sys.exit(1)
    dcs = datacenters()
hf = dcs['vm2b-dc']
import subprocess
der = ssl.get_server_certificate((HOST, 443)); der = ssl.PEM_cert_to_DER_cert(der)
thumb = ':'.join('%02X' % b for b in hashlib.sha1(der).digest())
spec = '<spec><hostName>%s</hostName><port>443</port><sslThumbprint>%s</sslThumbprint><userName>root</userName><password>%s</password><force>true</force></spec>' % (HOST, thumb, hostpw)
r = call('AddStandaloneHost_Task', '<_this type="Folder">%s</_this>%s<addConnected>true</addConnected>' % (hf, spec))
t = re.search(r'returnval type="Task">([^<]*)', r)
if not t:
    print('хост:', re.findall(r'<faultstring>([^<]*)', r)[:1]); sys.exit(1)
print('датацентр vm2b-dc создан, хост добавлен:', wait(t.group(1)))
