#!/usr/bin/env python3
# Параметры первой загрузки VCSA 7.0.3 под qemu БЕЗ VMware Tools (нет канала guestinfo.ovfEnv, а /bin/install-parameter читает ТОЛЬКО его): параметры кладутся файлами
# в /etc/vmware/install-defaults/<ключ без guestinfo.cis.> (это первое место, где install-parameter ищет значение) на ДИСК УСТРОЙСТВА до первой загрузки.
# Пароли берутся из файлов .winvm/esxi/secret-vc-sso и secret-vc-root (права 600), нигде не печатаются.
#   tools/esxi7/vcsa-ovfenv.py --seed <папка install-defaults внутри смонтированного диска> [адрес 10.213.77.11] [шлюз 10.213.77.1]
#   tools/esxi7/vcsa-ovfenv.py <куда положить ovfenv.iso> ...        (запасной путь: ISO с ovf-env.xml для скрипта /opt/vmware/bin/ovfenv, install-parameter его не читает)
import os, subprocess, sys, tempfile
from xml.sax.saxutils import quoteattr
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.winvm', 'esxi')
sso = open(os.path.join(root, 'secret-vc-sso')).read().strip()
rpw = open(os.path.join(root, 'secret-vc-root')).read().strip()
seed = sys.argv[1] == '--seed'
args = sys.argv[2:] if seed else sys.argv[1:]
out = args[0]; ip = args[1] if len(args) > 1 else '10.213.77.11'; gw = args[2] if len(args) > 2 else '10.213.77.1'
props = {
 'guestinfo.cis.appliance.net.addr.family': 'ipv4', 'guestinfo.cis.appliance.net.mode': 'static', 'guestinfo.cis.appliance.net.addr': ip,
 'guestinfo.cis.appliance.net.prefix': '24', 'guestinfo.cis.appliance.net.gateway': gw, 'guestinfo.cis.appliance.net.dns.servers': gw,
 'guestinfo.cis.appliance.net.pnid': ip, 'guestinfo.cis.appliance.net.ports': '{}',
 'guestinfo.cis.vmdir.username': 'administrator@vsphere.local', 'guestinfo.cis.vmdir.password': sso, 'guestinfo.cis.vmdir.domain-name': 'vsphere.local',
 'guestinfo.cis.vmdir.site-name': 'Default-First-Site', 'guestinfo.cis.vmdir.first-instance': 'True', 'guestinfo.cis.db.type': 'embedded',
 'guestinfo.cis.appliance.root.passwd': rpw, 'guestinfo.cis.appliance.root.shell': '/bin/bash', 'guestinfo.cis.appliance.ssh.enabled': 'True',
 'guestinfo.cis.appliance.time.tools-sync': 'False', 'guestinfo.cis.deployment.node.type': 'embedded', 'guestinfo.cis.system.vm0.port': '443',
 'guestinfo.cis.silentinstall': 'False', 'guestinfo.cis.lookup_timeout': '900', 'guestinfo.cis.clientlocale': 'en', 'guestinfo.cis.ceip_enabled': 'False',
 'guestinfo.cis.deployment.autoconfig': 'True', 'guestinfo.cis.hadcs.enabled': 'True', 'guestinfo.cis.fips.enabled': 'False', 'guestinfo.cis.desired.state': '{}',
 'domain': 'localdomain', 'searchpath': 'localdomain', 'vmname': 'vm2b-vcsa',
}
if seed:
    os.makedirs(out, exist_ok=True)
    for k, v in props.items():
        if k.startswith('guestinfo.cis.'):
            fn = os.path.join(out, k[len('guestinfo.cis.'):])
            fd = os.open(fn, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            os.write(fd, (v + '\n').encode()); os.close(fd)
    sys.exit(0)
xml = ('<?xml version="1.0" encoding="UTF-8"?>\n<Environment xmlns="http://schemas.dmtf.org/ovf/environment/1" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
       'xmlns:oe="http://schemas.dmtf.org/ovf/environment/1" xmlns:ve="http://www.vmware.com/schema/ovfenv" oe:id="" ve:vCenterId="vm-1">\n'
       ' <PlatformSection><Kind>VMware ESXi</Kind><Version>7.0.3</Version><Vendor>VMware, Inc.</Vendor><Locale>en</Locale></PlatformSection>\n <PropertySection>\n')
for k, v in props.items():
    xml += '  <Property oe:key=%s oe:value=%s/>\n' % (quoteattr(k), quoteattr(v))
xml += ' </PropertySection>\n <ve:EthernetAdapterSection><ve:Adapter ve:mac="52:54:00:12:34:77" ve:network="vm2b" ve:unitNumber="7"/></ve:EthernetAdapterSection>\n</Environment>\n'
with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as d:
    p = os.path.join(d, 'ovf-env.xml'); os.umask(0o077)
    open(p, 'w').write(xml)
    subprocess.check_call(['genisoimage', '-quiet', '-r', '-J', '-V', 'OVF ENV', '-o', out, d])
os.chmod(out, 0o600)
