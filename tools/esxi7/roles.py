#!/usr/bin/env python3
"""Раунд 3, пункт D: пользователи и роли на настоящем vCenter 7.0.3 для поиска минимального набора привилегий.  Пароли пользователей: .winvm/esxi/secret-vc-<имя> (права 600, не печатаются), administrator: secret-vc-sso.
   roles.py user <имя>                      локальный пользователь SSO <имя>@vsphere.local (dir-cli на VCSA; пароль случайный)
   roles.py role <роль> <прив,прив,...>     создаёт роль или ЗАМЕНЯЕТ её набор привилегий
   roles.py grant <имя> <роль> [корень|dc|ds:<имя>|vm:<имя>|folder:<id>]   назначает роль пользователю (propagate), по умолчанию на корневую папку; прежние права пользователя на этой сущности заменяются
   roles.py drop <имя>                      убирает права пользователя на корне и удаляет пользователя;  roles.py rmrole <роль>
   roles.py privs                           список всех привилегий vCenter (идентификаторы)"""
import importlib.util, os, re, secrets, string, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ESX = os.path.join(HERE, '..', '..', '.winvm', 'esxi')
spec = importlib.util.spec_from_file_location('pv', os.path.join(HERE, 'probe-vc.py')); pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
adm = pv.sess('10.213.77.11', 'administrator@vsphere.local', 'secret-vc-sso'); AM = adm.auth
root = re.search(r'<rootFolder[^>]*>([^<]+)', adm.sc).group(1); pc = re.search(r'propertyCollector[^>]*>([^<]+)', adm.sc).group(1)
sso = open(os.path.join(ESX, 'secret-vc-sso')).read().strip()
def pw_of(name):
    f = os.path.join(ESX, 'secret-vc-' + name)
    if not os.path.exists(f):
        fd = os.open(f, os.O_WRONLY | os.O_CREAT, 0o600); os.write(fd, (''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(12)) + 'Aa1@').encode()); os.close(fd)
    return open(f).read().strip()
def role_id(name):
    st, t = adm.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>AuthorizationManager</type><pathSet>roleList</pathSet></propSet><objectSet><obj type="AuthorizationManager">%s</obj></objectSet></specSet></RetrieveProperties>' % (pc, AM))
    for m in re.finditer(r'<AuthorizationRole[^>]*>(.*?)</AuthorizationRole>', t, re.S):
        b = m.group(1); n = re.search(r'<name>([^<]*)</name>', b); i = re.search(r'<roleId>(-?\d+)</roleId>', b)
        if n and i and n.group(1) == name: return i.group(1)
def entity(spec_):
    if spec_ in (None, 'корень', 'root'): return 'Folder', root
    k, _, v = spec_.partition(':')
    if k == 'dc':
        st, t = adm.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>Datacenter</type><pathSet>name</pathSet></propSet><objectSet><obj type="Folder">%s</obj><skip>true</skip><selectSet xsi:type="TraversalSpec"><name>t</name><type>Folder</type><path>childEntity</path><skip>false</skip></selectSet></objectSet></specSet></RetrieveProperties>' % (pc, root))
        return 'Datacenter', re.search(r'<obj type="Datacenter">([^<]+)', t).group(1)
    if k == 'vm': return 'VirtualMachine', pv.vc_vm(adm, v)
    if k == 'folder': return 'Folder', v
    raise SystemExit('сущность?')
cmd, *a = sys.argv[1:]
if cmd == 'user':
    name = a[0]; p = pw_of(name)
    script = ('/usr/lib/vmware-vmafd/bin/dir-cli user create --account %s --first-name VM2 --last-name %s --user-password "%s" --login administrator@vsphere.local --password "%s" 2>&1 | tail -1\n' % (name, name.replace('-', ''), p, sso))
    r = subprocess.run([os.path.join(HERE, 'vcsa-ssh.sh'), 'sh -s'], input=script, capture_output=True, text=True); print('user', name, re.sub(re.escape(p) + '|' + re.escape(sso), '***', r.stdout.strip())[-100:])
elif cmd == 'role':
    name, privs = a[0], [x for x in a[1].split(',') if x]; rid = role_id(name); ptxt = ''.join('<privIds>%s</privIds>' % x for x in privs)
    if rid: st, t = adm.call('<UpdateAuthorizationRole xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><roleId>%s</roleId><newName>%s</newName>%s</UpdateAuthorizationRole>' % (AM, rid, name, ptxt))
    else: st, t = adm.call('<AddAuthorizationRole xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><name>%s</name>%s</AddAuthorizationRole>' % (AM, name, ptxt))
    print('role', name, len(privs), 'прив.', pv.fault(t)[:160] if 'Fault' in t else 'OK')
elif cmd == 'grant':
    user, role = a[0], a[1]; et, eid = entity(a[2] if len(a) > 2 else None); rid = role_id(role)
    adm.call('<RemoveEntityPermission xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="%s">%s</entity><user>VSPHERE.LOCAL\\%s</user><isGroup>false</isGroup></RemoveEntityPermission>' % (AM, et, eid, user))
    st, t = adm.call('<SetEntityPermissions xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="%s">%s</entity><permission><principal>VSPHERE.LOCAL\\%s</principal><group>false</group><roleId>%s</roleId><propagate>true</propagate></permission></SetEntityPermissions>' % (AM, et, eid, user, rid))
    print('grant', user, role, et, eid, pv.fault(t)[:160] if 'Fault' in t else 'OK')
elif cmd == 'revoke':
    user = a[0]; et, eid = entity(a[1] if len(a) > 1 else None)
    st, t = adm.call('<RemoveEntityPermission xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="%s">%s</entity><user>VSPHERE.LOCAL\\%s</user><isGroup>false</isGroup></RemoveEntityPermission>' % (AM, et, eid, user)); print('revoke', et, eid, pv.fault(t)[:100] if 'Fault' in t else 'OK')
elif cmd == 'rmrole':
    rid = role_id(a[0]); st, t = adm.call('<RemoveAuthorizationRole xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><roleId>%s</roleId><failIfUsed>false</failIfUsed></RemoveAuthorizationRole>' % (AM, rid)); print('rmrole', a[0], 'OK' if 'Fault' not in t else pv.fault(t)[:100])
elif cmd == 'drop':
    user = a[0]; adm.call('<RemoveEntityPermission xmlns="urn:vim25"><_this type="AuthorizationManager">%s</_this><entity type="Folder">%s</entity><user>VSPHERE.LOCAL\\%s</user><isGroup>false</isGroup></RemoveEntityPermission>' % (AM, root, user))
    r = subprocess.run([os.path.join(HERE, 'vcsa-ssh.sh'), 'sh -s'], input='/usr/lib/vmware-vmafd/bin/dir-cli user delete --account %s --login administrator@vsphere.local --password "%s" 2>&1 | tail -1\n' % (user, sso), capture_output=True, text=True); print('drop', user, r.stdout.strip().replace(sso, '***')[-80:])
    f = os.path.join(ESX, 'secret-vc-' + user); os.path.exists(f) and os.remove(f)
elif cmd == 'privs':
    st, t = adm.call('<RetrieveProperties xmlns="urn:vim25"><_this type="PropertyCollector">%s</_this><specSet><propSet><type>AuthorizationManager</type><pathSet>privilegeList</pathSet></propSet><objectSet><obj type="AuthorizationManager">%s</obj></objectSet></specSet></RetrieveProperties>' % (pc, AM))
    print('\n'.join(sorted(set(re.findall(r'<privId>([^<]+)</privId>', t)))))
