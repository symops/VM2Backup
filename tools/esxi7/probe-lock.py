#!/usr/bin/env python3
"""Проба блокировки ВМ на НАСТОЯЩЕМ ESXi: AuthorizationManager.DisableMethods / EnableMethods / QueryDisabledMethods.
   probe-lock.py <Vmid или имя>  [list|lock|unlock|foreign <Method>]   Пароль читается из .winvm/esxi/secret, нигде не печатается."""
import ssl, sys, re, urllib.request, os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
PW = open(os.path.join(ROOT, '.winvm', 'esxi', 'secret')).read().strip()
HOST = os.environ.get('ESXI_HOST', '10.213.77.10')
CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE
ENV = '<?xml version="1.0" encoding="UTF-8"?><soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><soapenv:Body>%s</soapenv:Body></soapenv:Envelope>'
class Sess:
    def __init__(self):
        self.cookie = None
        r = self.call('<RetrieveServiceContent xmlns="urn:vim25"><_this type="ServiceInstance">ServiceInstance</_this></RetrieveServiceContent>')
        self.sc = r[1]
        sm = re.search(r'<sessionManager[^>]*>([^<]+)', self.sc).group(1)
        self.call('<Login xmlns="urn:vim25"><_this type="SessionManager">%s</_this><userName>root</userName><password>%s</password></Login>' % (sm, PW))
        self.auth = re.search(r'<authorizationManager[^>]*>([^<]+)', self.sc).group(1)
        self.pc = re.search(r'<propertyCollector[^>]*>([^<]+)', self.sc).group(1)
    def call(self, body):
        req = urllib.request.Request('https://%s/sdk' % HOST, data=(ENV % body).encode(), headers={'Content-Type': 'text/xml', 'SOAPAction': '"urn:vim25/7.0"'})
        if self.cookie: req.add_header('Cookie', self.cookie)
        try:
            r = urllib.request.urlopen(req, context=CTX, timeout=60); txt = r.read().decode(); st = r.status
            c = r.headers.get('Set-Cookie')
            if c: self.cookie = c.split(';')[0]
        except urllib.error.HTTPError as e:
            txt = e.read().decode(); st = e.code
        return st, txt
    def vm(self, name):
        st, t = self.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>VirtualMachine</type><pathSet>name</pathSet></propSet><objectSet><obj type="Folder">ha-folder-root</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>a</name><type>Folder</type><path>childEntity</path><skip>true</skip><selectSet><name>a</name></selectSet><selectSet xsi:type="TraversalSpec"><name>b</name><type>Datacenter</type><path>vmFolder</path><skip>true</skip><selectSet><name>a</name></selectSet></selectSet></selectSet></objectSet></specSet></RetrieveProperties>' % self.pc)
        for m in re.finditer(r'<obj type="VirtualMachine">([^<]+)</obj><propSet><name>name</name><val[^>]*>([^<]*)</val>', t):
            if m.group(2) == name: return m.group(1)
        raise SystemExit('no VM ' + name + ': ' + t[:300])
METHODS = os.environ.get('LOCK_METHODS', 'PowerOnVM_Task,ResetVM_Task,SuspendVM_Task,RebootGuest,StandbyGuest,ReconfigVM_Task,CreateSnapshot_Task,CreateSnapshotEx_Task,RemoveAllSnapshots_Task,RevertToCurrentSnapshot_Task,RelocateVM_Task,MigrateVM_Task,CloneVM_Task,MarkAsTemplate,UnregisterVM,Destroy_Task,CustomizeVM_Task,UpgradeVM_Task,ConsolidateVMDisks_Task,PromoteDisks_Task,InstantClone_Task,ExportVm,Rename_Task,AcquireTicket').split(',')
def short(t): return re.sub(r'\s+', ' ', t)[:1500]
if __name__ == '__main__':
    s = Sess(); vm = sys.argv[1] if sys.argv[1].isdigit() else s.vm(sys.argv[1]);   # на отдельном хосте MoRef ВМ = Vmid из vim-cmd vmsvc/getallvms
    act = sys.argv[2] if len(sys.argv) > 2 else 'list'
    src = os.environ.get('LOCK_SOURCE', 'VM2Backup-0123456789-on-1')
    if act == 'list':
        print('VM', vm, 'auth', s.auth); print(short(s.call('<QueryDisabledMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity></QueryDisabledMethods>' % (s.auth, vm))[1]))
    elif act == 'lock':
        ms = ''.join('<method><method>%s</method><reasonId>VM2Backup: held for a backup</reasonId></method>' % m for m in METHODS)
        print(short(s.call('<DisableMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity>%s<sourceId>%s</sourceId></DisableMethods>' % (s.auth, vm, ms, src))[1]))
    elif act == 'unlock':
        print(short(s.call('<EnableMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity><sourceId>%s</sourceId></EnableMethods>' % (s.auth, vm, src))[1]))
    elif act == 'foreign':
        m = sys.argv[3]
        extra = {'PowerOnVM_Task': '', 'ReconfigVM_Task': '<spec><annotation>x</annotation></spec>', 'CreateSnapshot_Task': '<name>x</name><memory>false</memory><quiesce>false</quiesce>'}.get(m, '')
        print(short(s.call('<%s xmlns="urn:vim25"><_this type="VirtualMachine">%s</_this>%s</%s>' % (m, vm, extra, m))[1]))
