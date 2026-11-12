#!/usr/bin/env python3
"""Раунд 3, пункт E: папка ВМ и пул ресурсов в vCenter (для проверки восстановления «в папку и пул»).  vc-inv.py <имя папки> <id родительского пула или 'resgroup-34'> <имя пула>"""
import importlib.util, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('pv', os.path.join(HERE, 'probe-vc.py')); pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
s = pv.sess('10.213.77.11', 'administrator@vsphere.local', 'secret-vc-sso')
folder, parent, pool = sys.argv[1:4]
st, t = s.call('<CreateFolder xmlns="urn:vim25"><_this type="Folder">group-v4</_this><name>%s</name></CreateFolder>' % folder); print('папка', folder, pv.fault(t)[:120] if 'Fault' in t else 'OK')
alloc = '<reservation>0</reservation><expandableReservation>true</expandableReservation><limit>-1</limit><shares><shares>1000</shares><level>normal</level></shares>'
st, t = s.call('<CreateResourcePool xmlns="urn:vim25"><_this type="ResourcePool">%s</_this><name>%s</name><spec><cpuAllocation>%s</cpuAllocation><memoryAllocation>%s</memoryAllocation></spec></CreateResourcePool>' % (parent, pool, alloc, alloc)); print('пул', pool, pv.fault(t)[:120] if 'Fault' in t else 'OK')
