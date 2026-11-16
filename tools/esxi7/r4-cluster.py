#!/usr/bin/env python3
# Раунд 4: датацентр vm2b-dc, кластер vm2b-cl (DRS выключен / manual / fullyAutomated, HA выключен) и в нём оба вложенных хоста (AddHost_Task на кластер).
#   tools/esxi7/r4-cluster.py create [drs=off|manual|auto]     создаёт всё недостающее
#   tools/esxi7/r4-cluster.py drs off|manual|auto              переключает DRS кластера
# Пароли хостов - из .winvm/esxi/secret и .winvm/esxi2/secret, vCenter - secret-vc-sso (нигде не печатаются).
import hashlib, os, re, ssl, sys
import vcl
vcl.login()
def drs_xml(mode):
    if mode == 'off': return '<drsConfig><enabled>false</enabled></drsConfig>'
    beh = 'manual' if mode == 'manual' else 'fullyAutomated'
    return '<drsConfig><enabled>true</enabled><defaultVmBehavior>%s</defaultVmBehavior></drsConfig>' % beh
def dc():
    r = vcl.call('RetrievePropertiesEx', '<_this type="PropertyCollector">%s</_this><specSet><propSet><type>Datacenter</type><pathSet>name</pathSet><pathSet>hostFolder</pathSet></propSet><objectSet><obj type="Folder">%s</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>t</name><type>Folder</type><path>childEntity</path><skip>false</skip></selectSet></objectSet></specSet><options/>' % (vcl.g('propertyCollector'), vcl.g('rootFolder')))
    out = {}
    for m in re.finditer(r'<obj type="Datacenter">([^<]*)</obj>(.*?)</returnval>', r, re.S):
        h = re.search(r'<name>hostFolder</name><val[^>]*>([^<]*)', m.group(2)); n = re.search(r'<name>name</name><val[^>]*>([^<]*)', m.group(2))
        if h and n: out[n.group(1)] = (m.group(1), h.group(1))
    return out
cmd = sys.argv[1] if len(sys.argv) > 1 else 'create'
if cmd == 'create':
    mode = (sys.argv[2] if len(sys.argv) > 2 else 'drs=manual').split('=')[1]
    d = dc()
    if 'vm2b-dc' not in d:
        r = vcl.call('CreateDatacenter', '<_this type="Folder">%s</_this><name>vm2b-dc</name>' % vcl.g('rootFolder')); print('датацентр:', vcl.fault(r) or 'создан'); d = dc()
    hf = d['vm2b-dc'][1]
    cl = [c for c in vcl.find('ClusterComputeResource') if c[1] == 'vm2b-cl']
    if not cl:
        r = vcl.call('CreateClusterEx', '<_this type="Folder">%s</_this><name>vm2b-cl</name><spec>%s</spec>' % (hf, drs_xml(mode)))
        print('кластер:', vcl.fault(r) or 'создан'); cl = [c for c in vcl.find('ClusterComputeResource') if c[1] == 'vm2b-cl']
    cid = cl[0][0]
    have = [h[1] for h in vcl.find('HostSystem')]
    for host, sec in (('10.213.77.10', 'esxi/secret'), ('10.213.77.12', 'esxi2/secret')):
        if host in have: continue
        pw = open(os.path.join(vcl.ROOT, '..', sec)).read().strip()
        thumb = ':'.join('%02X' % b for b in hashlib.sha1(ssl.PEM_cert_to_DER_cert(ssl.get_server_certificate((host, 443)))).digest())
        r = vcl.call('AddHost_Task', '<_this type="ClusterComputeResource">%s</_this><spec><hostName>%s</hostName><port>443</port><sslThumbprint>%s</sslThumbprint><userName>root</userName><password>%s</password><force>true</force></spec><asConnected>true</asConnected>' % (cid, host, thumb, pw))
        t = vcl.task_of(r)
        if not t: print('хост', host, vcl.fault(r)); continue
        ok, info = vcl.wait(t); print('хост', host, 'добавлен в кластер:', ok, info.get('error', ''))
elif cmd == 'drs':
    cid = [c for c in vcl.find('ClusterComputeResource') if c[1] == 'vm2b-cl'][0][0]
    r = vcl.call('ReconfigureComputeResource_Task', '<_this type="ClusterComputeResource">%s</_this><spec xsi:type="ClusterConfigSpecEx">%s</spec><modify>true</modify>' % (cid, drs_xml(sys.argv[2])))
    t = vcl.task_of(r); print('DRS ->', sys.argv[2], vcl.wait(t) if t else vcl.fault(r))
