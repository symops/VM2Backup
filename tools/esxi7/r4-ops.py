#!/usr/bin/env python3
# Раунд 4: операции над ВМ через vCenter для сценариев "во время копии" (миграция, снимки, заблокированные методы).  Вывод короткий; пароли не печатаются.
#   r4-ops.py vms                                  ВМ: имя, id, хост, питание, снимки
#   r4-ops.py migrate <вм> <ip хоста>              MigrateVM_Task (vMotion включённой / холодная миграция выключенной)
#   r4-ops.py relocate <вм> <ip хоста> [датастор]  RelocateVM_Task (хост и, если указан, датастор)
#   r4-ops.py wait-snap <вм> [секунд]              ждёт, пока у ВМ появится снимок (признак того, что копия началась)
#   r4-ops.py disabled <вм>                        QueryDisabledMethods
#   r4-ops.py try <вм> <метод>                     вызывает метод (MigrateVM_Task/RelocateVM_Task/PowerOffVM_Task/...) от администратора и печатает ответ сервера
import re, sys, time
import vcl
vcl.login()
def vm_id(name):
    for i, n in vcl.find('VirtualMachine'):
        if n == name: return i
    sys.exit('нет ВМ ' + name)
def host_id(ip):
    for i, n in vcl.find('HostSystem'):
        if n == ip: return i
    sys.exit('нет хоста ' + ip)
def host_pool(hid):
    p = vcl.retrieve('HostSystem', hid, ['parent'])['parent']
    typ = 'ClusterComputeResource' if p.startswith('domain-c') else 'ComputeResource'
    return vcl.retrieve(typ, p, ['resourcePool'])['resourcePool']
def ds_id(name):
    for i, n in vcl.find('Datastore'):
        if n == name: return i
    sys.exit('нет датастора ' + name)
def finish(r, quiet=False):
    t = vcl.task_of(r)
    if not t: print('ОТКАЗ:', vcl.fault(r) or r[:300]); return False
    t0 = time.time(); ok, info = vcl.wait(t); print('задача %s за %.1f с: %s %s' % (t, time.time() - t0, 'успех' if ok else 'ОШИБКА', info.get('error', ''))); return ok
c = sys.argv[1]
if c == 'vms':
    for i, n in vcl.find('VirtualMachine'):
        p = vcl.retrieve('VirtualMachine', i, ['runtime.host', 'runtime.powerState', 'snapshot', 'config.files.vmPathName'])
        hn = vcl.retrieve('HostSystem', p.get('runtime.host', '').split()[0], ['name']).get('name', '?') if p.get('runtime.host') else '?'
        print(n, i, hn, p.get('runtime.powerState'), 'снимки:' + ('да' if p.get('snapshot') else 'нет'), p.get('config.files.vmPathName', ''))
elif c == 'migrate':
    v = vm_id(sys.argv[2]); h = host_id(sys.argv[3])
    finish(vcl.call('MigrateVM_Task', '<_this type="VirtualMachine">%s</_this><pool type="ResourcePool">%s</pool><host type="HostSystem">%s</host><priority>defaultPriority</priority>' % (v, host_pool(h), h)))
elif c == 'relocate':
    v = vm_id(sys.argv[2]); h = host_id(sys.argv[3]); d = ('<datastore type="Datastore">%s</datastore>' % ds_id(sys.argv[4])) if len(sys.argv) > 4 else ''
    finish(vcl.call('RelocateVM_Task', '<_this type="VirtualMachine">%s</_this><spec>%s<pool type="ResourcePool">%s</pool><host type="HostSystem">%s</host></spec><priority>defaultPriority</priority>' % (v, d, host_pool(h), h)))
elif c == 'wait-snap':
    v = vm_id(sys.argv[2]); lim = int(sys.argv[3]) if len(sys.argv) > 3 else 600; t0 = time.time()
    while time.time() - t0 < lim:
        if vcl.retrieve('VirtualMachine', v, ['snapshot']).get('snapshot'): print('снимок появился через %.0f с' % (time.time() - t0)); break
        time.sleep(1)
    else: print('снимка нет'); sys.exit(1)
elif c == 'disabled':
    v = vm_id(sys.argv[2]); am = vcl.g('authorizationManager')
    r = vcl.call('QueryDisabledMethods', '<_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity>' % (am, v))
    src = re.findall(r'<sourceId>([^<]*)', r); m = re.findall(r'<method>([^<]*)', r)
    print('отключено методов:', len(set(m)), 'источники:', sorted(set(src))[:3], vcl.fault(r) or '')
    print(sorted(set(m)))
elif c == 'try':
    v = vm_id(sys.argv[2]); m = sys.argv[3]
    extra = {'PowerOffVM_Task': '', 'MigrateVM_Task': '<priority>defaultPriority</priority>', 'RelocateVM_Task': '<spec/><priority>defaultPriority</priority>', 'ReconfigVM_Task': '<spec><annotation>foreign</annotation></spec>', 'CreateSnapshot_Task': '<name>foreign</name><memory>false</memory><quiesce>false</quiesce>', 'RemoveAllSnapshots_Task': '', 'Destroy_Task': '', 'UnregisterVM': '', 'PowerOnVM_Task': ''}[m]
    r = vcl.call(m, '<_this type="VirtualMachine">%s</_this>%s' % (v, extra)); print(m, '->', vcl.fault(r) or ('задача ' + str(vcl.task_of(r))))
    t = vcl.task_of(r)
    if t: print(vcl.wait(t)[1].get('error', 'успех')); 
