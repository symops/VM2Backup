#!/usr/bin/env python3
"""Какие права нужны пользователю продукта для блокировки ВМ на настоящем vCenter: локальный пользователь SSO vm2bbk, две роли (без и с Global.DisableMethods / Global.EnableMethods),
   ответы DisableMethods / EnableMethods.  Пароли (administrator и vm2bbk) только в файлах .winvm/esxi/secret-vc-sso / secret-vc-bk, нигде не печатаются.   probe-priv.py <имя ВМ>"""
import importlib.util, os, re, subprocess, sys, secrets, string
HERE = os.path.dirname(os.path.abspath(__file__)); ESX = os.path.join(HERE, '..', '..', '.winvm', 'esxi')
spec = importlib.util.spec_from_file_location('pv', os.path.join(HERE, 'probe-vc.py')); pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
name = sys.argv[1]; short = pv.short; fault = pv.fault
bk = os.path.join(ESX, 'secret-vc-bk')
if not os.path.exists(bk):
    fd = os.open(bk, os.O_WRONLY | os.O_CREAT, 0o600); os.write(fd, (''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(12)) + 'Aa1@').encode()); os.close(fd)
bkpw = open(bk).read().strip(); sso = open(os.path.join(ESX, 'secret-vc-sso')).read().strip()
# пользователь SSO создаётся скриптом, который передаётся на VCSA через stdin (пароли не попадают в командную строку этой машины)
script = ('/usr/lib/vmware-vmafd/bin/dir-cli user create --account vm2bbk --first-name VM2 --last-name Backup --user-password "%s" --login administrator@vsphere.local --password "%s" 2>&1 | tail -1\n' % (bkpw, sso))
r = subprocess.run([os.path.join(HERE, 'vcsa-ssh.sh'), 'sh -s'], input=script, capture_output=True, text=True); print('создание пользователя:', re.sub(bkpw + '|' + sso, '***', r.stdout.strip())[-120:])
adm = pv.sess('10.213.77.11', 'administrator@vsphere.local', 'secret-vc-sso'); vm = pv.vc_vm(adm, name)
base = ['System.Anonymous', 'System.View', 'System.Read', 'VirtualMachine.Interact.PowerOn', 'VirtualMachine.Interact.PowerOff', 'VirtualMachine.Config.AdvancedConfig', 'VirtualMachine.Config.Settings', 'VirtualMachine.State.CreateSnapshot']
def role(nm, privs):
    st, t = adm.call('<AddAuthorizationRole xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><name>%s</name>%s</AddAuthorizationRole>' % (adm.auth, nm, ''.join('<privIds>%s</privIds>' % p for p in privs)))
    m = re.search(r'<returnval>(-?\d+)</returnval>', t); print('роль', nm, 'id', m.group(1) if m else fault(t)); return m.group(1) if m else None
r1 = role('VM2B-nolock', base); r2 = role('VM2B-lock', base + ['Global.DisableMethods', 'Global.EnableMethods'])
root = re.search(r'<rootFolder[^>]*>([^<]+)', adm.sc).group(1)
def perm(rid, on_root=False):      # Global.* привилегии проверяются на КОРНЕВОЙ папке: права на самой ВМ для них не считаются
    ent = ('Folder', root, 'true') if on_root else ('VirtualMachine', vm, 'false')
    st, t = adm.call('<SetEntityPermissions xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="%s">%s</entity><permission><principal>VSPHERE.LOCAL\\vm2bbk</principal><group>false</group><roleId>%s</roleId><propagate>%s</propagate></permission></SetEntityPermissions>' % (adm.auth, ent[0], ent[1], rid, ent[2])); return fault(t)
def run(label):
    u = pv.sess('10.213.77.11', 'vm2bbk@vsphere.local', 'secret-vc-bk')
    ms = '<method><method>PowerOnVM_Task</method><reasonId>x</reasonId></method>'
    st, t = u.call('<DisableMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity>%s<sourceId>VM2Backup-0123456789-off-1</sourceId></DisableMethods>' % (u.auth, vm, ms)); print(label, 'DisableMethods:', fault(t)); pv.save('DisableMethods-%s.xml' % ('ok' if 'Fault' not in t else 'NoPermission'), t)
    st, t = u.call('<EnableMethods xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity><method>PowerOnVM_Task</method><sourceId>VM2Backup-0123456789-off-1</sourceId></EnableMethods>' % (u.auth, vm)); print(label, 'EnableMethods :', fault(t)); pv.save('EnableMethods-%s.xml' % ('ok' if 'Fault' not in t else 'NoPermission'), t)
try:
    if r1: print('права: роль без Global.DisableMethods ->', perm(r1)); run('без Global.DisableMethods')
    if r2: print('права: роль без Global.* на ВМ, с Global.DisableMethods / Global.EnableMethods на ВМ (не на корне) ->', perm(r2)); run('Global.* только на ВМ')
    if r2: print('права: та же роль на КОРНЕВОЙ папке (propagate) ->', perm(r2, True)); run('Global.* на корне')
finally:
    adm.call('<RemoveEntityPermission xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="VirtualMachine">%s</entity><user>VSPHERE.LOCAL\\vm2bbk</user><isGroup>false</isGroup></RemoveEntityPermission>' % (adm.auth, vm)); adm.call('<RemoveEntityPermission xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="Folder">%s</entity><user>VSPHERE.LOCAL\\vm2bbk</user><isGroup>false</isGroup></RemoveEntityPermission>' % (adm.auth, root))
    for rid in (r1, r2):
        if rid: adm.call('<RemoveAuthorizationRole xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><roleId>%s</roleId><failIfUsed>false</failIfUsed></RemoveAuthorizationRole>' % (adm.auth, rid))
    s2 = ('/usr/lib/vmware-vmafd/bin/dir-cli user delete --account vm2bbk --login administrator@vsphere.local --password "%s" 2>&1 | tail -1\n' % sso)
    rr = subprocess.run([os.path.join(HERE, 'vcsa-ssh.sh'), 'sh -s'], input=s2, capture_output=True, text=True); print('удаление пользователя:', rr.stdout.strip().replace(sso, '***')[-100:])
    os.remove(bk)
