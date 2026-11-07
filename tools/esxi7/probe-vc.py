#!/usr/bin/env python3
"""Факты о блокировке ВМ на НАСТОЯЩЕМ vCenter, пока копия удерживает ВМ (cold-scenario.sh vclimits).  Свои сессии: administrator@vsphere.local (ЧУЖАЯ сессия) и root хоста (прямой вызов на хост).
   probe-vc.py held <имя ВМ>      QueryDisabledMethods, отказы чужой сессии (MethodDisabled: точный текст), прямой вызов на хост в обход vCenter
Пароли из .winvm/esxi/secret-vc-sso и secret, нигде не печатаются.  Опасные вызовы (UnregisterVM, Destroy_Task) делаются ТОЛЬКО если QueryDisabledMethods показал их отключёнными."""
import importlib.util, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
def sess(host, user, secret):
    os.environ['ESXI_HOST'], os.environ['ESXI_USER'], os.environ['ESXI_SECRET'] = host, user, secret
    spec = importlib.util.spec_from_file_location('pl_%s' % host.replace('.', '_'), os.path.join(HERE, 'probe-lock.py')); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m.Sess()
def short(t): return re.sub(r'\s+', ' ', t)
FIX = os.environ.get('FIXDIR')
def save(name, xml):      # образец настоящего ответа vCenter 7.0.3.02500 для tests/fixtures/vcenter7-real (первая строка - HTTP-статус, как у остальных образцов; секретов в ответах нет)
    if FIX: os.makedirs(FIX, exist_ok=True); open(os.path.join(FIX, name), 'w').write(('HTTP 500\n' if '<soapenv:Fault>' in xml else 'HTTP 200\n') + xml)
def fault(t):
    m = re.search(r'<faultstring>([^<]*)', t); d = re.search(r'<detail>\s*<(\w+)', t)
    return ('FAULT %s: %s' % (d.group(1) if d else '?', m.group(1)[:200])) if m else 'OK ' + short(t)[-160:]
def vc_vm(s, name):
    st, t = s.call('<CreateContainerView xmlns="urn:vim25"><_this type="ViewManager">%s</_this><container type="Folder">%s</container><type>VirtualMachine</type><recursive>true</recursive></CreateContainerView>' % (re.search(r'<viewManager[^>]*>([^<]+)', s.sc).group(1), re.search(r'<rootFolder[^>]*>([^<]+)', s.sc).group(1)))
    view = re.search(r'<returnval type="ContainerView">([^<]+)', t).group(1)
    st, t = s.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>VirtualMachine</type><pathSet>name</pathSet></propSet><objectSet><obj type="ContainerView">%s</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>t</name><type>ContainerView</type><path>view</path><skip>false</skip></selectSet></objectSet></specSet></RetrieveProperties>' % (s.pc, view))
    for m in re.finditer(r'<obj type="VirtualMachine">([^<]+)</obj><propSet><name>name</name><val[^>]*>([^<]*)</val>', t):
        if m.group(2) == name: return m.group(1)
    raise SystemExit('нет ВМ ' + name)
EXTRA = {'ReconfigVM_Task': '<spec><annotation>foreign session</annotation></spec>', 'CreateSnapshot_Task': '<name>foreign</name><memory>false</memory><quiesce>false</quiesce>',
         'RelocateVM_Task': '', 'MarkAsTemplate': '', 'CloneVM_Task': ''}
if __name__ == '__main__':
    name = sys.argv[2]
    vc = sess('10.213.77.11', 'administrator@vsphere.local', 'secret-vc-sso'); vm = vc_vm(vc, name)
    print('vCenter VM', vm)
    st, q = vc.call('<QueryDisabledMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity></QueryDisabledMethods>' % (vc.auth, vm))
    save('QueryDisabledMethods-held.resp.xml', q)
    print('QueryDisabledMethods:', short(q)[short(q).find('<QueryDisabledMethodsResponse') if '<QueryDisabledMethodsResponse' in q else 0:][:1800])
    st, pw = vc.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>VirtualMachine</type><pathSet>disabledMethod</pathSet><pathSet>runtime.powerState</pathSet></propSet><objectSet><obj type="VirtualMachine">%s</obj></objectSet></specSet></RetrieveProperties>' % (vc.pc, vm))
    print('disabledMethod / power:', short(pw)[-1500:])
    # НАШИ блокировки = записи QueryDisabledMethods, у которых sourceId начинается с VM2Backup-; остальные (VirtualCenter / InvalidState) - состояние ВМ, не наша блокировка
    ours = set(m.group(1) for m in re.finditer(r'<returnval><method>([^<]+)</method>(.*?)</returnval>', q, re.S) if re.search(r'<sourceId>VM2Backup-', m.group(2)))
    print('наши отключённые методы (%d): %s' % (len(ours), ', '.join(sorted(ours))))
    if not ours: raise SystemExit('ВМ НЕ удерживается нашей блокировкой: опасные и меняющие вызовы не делаются')
    dis = ours
    for m in ('PowerOnVM_Task', 'ReconfigVM_Task', 'CreateSnapshot_Task', 'UnregisterVM', 'Destroy_Task'):
        if m in ('UnregisterVM', 'Destroy_Task') and m not in dis: print('  %-22s НЕ вызван: не показан отключённым' % m); continue
        st, t = vc.call('<%s xmlns="urn:vim25"><_this type="VirtualMachine">%s</_this>%s</%s>' % (m, vm, EXTRA.get(m, ''), m)); print('  чужая сессия vCenter  %-22s %s' % (m, fault(t))); save('%s-MethodDisabled.fault.xml' % m, t) if m == 'PowerOnVM_Task' else None
    host = sess('10.213.77.10', 'root', 'secret')
    hid = os.popen("%s \"vim-cmd vmsvc/getallvms | grep ' %s ' | cut -d' ' -f1\"" % (os.path.join(HERE, 'ssh.sh'), name)).read().strip().split('\n')[-1]
    for m in (('ReconfigVM_Task',) if os.environ.get('PROBE_NO_HOSTPOWER') else ('PowerOnVM_Task', 'ReconfigVM_Task')):      # PROBE_NO_HOSTPOWER=1: не включать ВМ мимо vCenter (сценарий, который должен дойти до конца)
        st, t = host.call('<%s xmlns="urn:vim25"><_this type="VirtualMachine">%s</_this>%s</%s>' % (m, hid, EXTRA.get(m, ''), m)); print('  прямой вызов на ХОСТ  %-22s %s' % (m, fault(t)))
    if not os.environ.get('PROBE_NO_HOSTPOWER'): print('  vim-cmd power.on на хосте:', os.popen("%s \"vim-cmd vmsvc/power.on %s 2>&1 | tail -3\"" % (os.path.join(HERE, 'ssh.sh'), hid)).read().strip().replace('\n', ' | '))
