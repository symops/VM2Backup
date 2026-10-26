#!/bin/bash
# sync-pkg.sh <x86|w10|w11>: copies the test payload into shared/pkg (the guest sees it as \\10.0.2.4\qemu\pkg or \\host.lan\Data\pkg):
#   VM2Backup-Setup.exe (dist/, the single-file installer), bin\ (the build folder of the guest's bitness: autotest, service, panel, ...), guest\ (the scripts)
R=/home/symo/Claude/VM2Backup; W=$R/.winvm
case "$1" in x86) A=x86;; w10|w11) A=x64;; *) echo "usage: sync-pkg.sh x86|w10|w11"; exit 1;; esac
rm -rf "$W/shared/pkg"; mkdir -p "$W/shared/pkg/bin" "$W/shared/pkg/guest"
cp "$R/dist/VM2Backup-Setup.exe" "$W/shared/pkg/"
for f in vm2bautotest.exe VM2BackupService.exe VM2Backup.exe VM2Backup-Setup.exe; do cp "$R/build/$A/$f" "$W/shared/pkg/bin/"; done
cp "$W"/guest/* "$W/shared/pkg/guest/"
# эмулятор ESXi для запуска внутри гостя (esxilocal.ps1): python embeddable (кеш .winvm/pyembed), скрипты эмулятора, готовые сертификаты
mkdir -p "$W/shared/pkg/esxi/certs" "$W/shared/pkg/pyembed"
cp "$R"/tools/testservers/esxi_mock.py "$R"/tools/testservers/esxi_launch.py "$W/shared/pkg/esxi/"
[ -f "$W/pyembed/python-embed-win32.zip" ] || curl -sSL -o "$W/pyembed/python-embed-win32.zip" https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-win32.zip
cp "$W/pyembed/python-embed-win32.zip" "$W/shared/pkg/pyembed/"
PY=$R/.venv/bin/python; [ -x "$PY" ] || PY=python3
"$PY" "$R/tools/testservers/make_premade_certs.py" "$W/shared/pkg/esxi/certs"
echo "$A" > "$W/shared/pkg/arch.txt"
ls -la "$W/shared/pkg" "$W/shared/pkg/bin" | head -20
