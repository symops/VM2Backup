#!/usr/bin/env python3
# Раунд 4: маленькая библиотека SOAP-вызовов vCenter (vim25) для сценариев стенда: вход, вызов метода, ожидание задачи, чтение свойств.  Пароль берётся из файла
# .winvm/esxi/<секрет> (по умолчанию secret-vc-sso, пользователь administrator@vsphere.local) и нигде не печатается.
import http.cookiejar, os, re, ssl, sys, time, urllib.request, urllib.error
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.winvm', 'esxi')
VC = os.environ.get('VC', '10.213.77.11')
ctx = ssl._create_unverified_context(); cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), urllib.request.HTTPCookieProcessor(cj))
def call(m, inner, ver='7.0', timeout=300):
    env = ('<?xml version="1.0" encoding="UTF-8"?><soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
           'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><soapenv:Body><%s xmlns="urn:vim25">%s</%s></soapenv:Body></soapenv:Envelope>' % (m, inner, m))
    r = urllib.request.Request('https://%s/sdk' % VC, env.encode(), {'Content-Type': 'text/xml', 'SOAPAction': '"urn:vim25/%s"' % ver})
    try: return op.open(r, timeout=timeout).read().decode()
    except urllib.error.HTTPError as e: return e.read().decode()
sc = None
def login(user='administrator@vsphere.local', secret='secret-vc-sso'):
    global sc
    pw = open(os.path.join(ROOT, secret)).read().strip()
    sc = call('RetrieveServiceContent', '<_this type="ServiceInstance">ServiceInstance</_this>')
    r = call('Login', '<_this type="SessionManager">%s</_this><userName>%s</userName><password>%s</password>' % (g('sessionManager'), user, pw))
    if 'Fault' in r: sys.exit('вход в vCenter не удался')
def g(t): return re.search('<%s[^>]*>([^<]*)</%s>' % (t, t), sc).group(1)
def fault(r):
    f = re.findall(r'<faultstring>([^<]*)', r); return f[0] if f else None
def retrieve(typ, obj, paths):
    """свойства одного объекта -> словарь (значения строкой)"""
    ps = ''.join('<pathSet>%s</pathSet>' % p for p in paths)
    r = call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>%s</type>%s</propSet><objectSet><obj type="%s">%s</obj></objectSet></specSet><options/>' % (g('propertyCollector'), typ, ps, typ, obj))
    return dict((m.group(1), re.sub(r'<[^>]+>', ' ', m.group(2)).strip()) for m in re.finditer(r'<name>([^<]*)</name><val[^>]*>(.*?)</val>', r, re.S))
def wait(task, quiet=False, limit=1800):
    t0 = time.time()
    while time.time() - t0 < limit:
        s = retrieve('Task', task, ['info.state', 'info.error', 'info.progress'])
        st = s.get('info.state')
        if st == 'success': return True, s
        if st == 'error':
            f = fault(call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>Task</type><pathSet>info.error</pathSet></propSet><objectSet><obj type="Task">%s</obj></objectSet></specSet><options/>' % (g('propertyCollector'), task)))
            lm = re.findall(r'<localizedMessage>([^<]*)', call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>Task</type><pathSet>info.error</pathSet></propSet><objectSet><obj type="Task">%s</obj></objectSet></specSet><options/>' % (g('propertyCollector'), task)))
            return False, {'error': (lm[:1] or [''])[0], 'raw': s.get('info.error', '')[:200]}
        time.sleep(2)
    return False, {'error': 'timeout'}
def task_of(r):
    m = re.search(r'returnval type="Task">([^<]*)', r); return m.group(1) if m else None
def find(typ, pathname='name'):
    """все объекты типа через контейнерный вид корня: [(id, имя)]"""
    cv = call('CreateContainerView', '<_this type="ViewManager">%s</_this><container type="Folder">%s</container><type>%s</type><recursive>true</recursive>' % (g('viewManager'), g('rootFolder'), typ))
    v = re.search(r'returnval type="ContainerView">([^<]*)', cv).group(1)
    r = call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>%s</type><pathSet>%s</pathSet></propSet><objectSet><obj type="ContainerView">%s</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>tv</name><type>ContainerView</type><path>view</path><skip>false</skip></selectSet></objectSet></specSet><options/>' % (g('propertyCollector'), typ, pathname, v))
    out = [(m.group(1), m.group(2)) for m in re.finditer(r'<obj type="%s">([^<]*)</obj><propSet><name>%s</name><val[^>]*>([^<]*)' % (typ, pathname), r)]
    call('DestroyView', '<_this type="ContainerView">%s</_this>' % v)
    return out
