#!/usr/bin/env python3
"""Ставит guestinfo-переменную ВМ на хосте ESXi через ReconfigVM_Task (extraConfig), без доступа в гостя.   vm-guestinfo.py <имя ВМ> <ключ> <значение>
Пароль root хоста из .winvm/esxi/secret, не печатается."""
import importlib.util, os, re, sys, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('pv', os.path.join(HERE, 'probe-vc.py')); pv = importlib.util.module_from_spec(spec); spec.loader.exec_module(pv)
name, key, val = sys.argv[1:4]
hid = subprocess.run([os.path.join(HERE, 'ssh.sh'), "vim-cmd vmsvc/getallvms | grep ' %s ' | cut -d' ' -f1" % name], capture_output=True, text=True).stdout.strip().split('\n')[-1]
s = pv.sess('10.213.77.10', 'root', 'secret')
st, t = s.call('<ReconfigVM_Task xmlns="urn:vim25"><_this type="VirtualMachine">%s</_this><spec><extraConfig><key>%s</key><value xsi:type="xsd:string">%s</value></extraConfig></spec></ReconfigVM_Task>' % (hid, key, val))
print(pv.fault(t)[:200])
