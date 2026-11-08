#!/bin/sh
# Builds everything on Linux (clang + MinGW-w64) and assembles the package:
#   dist/VM2Backup-Setup.exe   the release: ONE file - the launcher with the whole package below appended
#                                 (a cabinet, MSZIP; extracted to a temporary folder when it runs)
#   dist/VM2Backup/            the same package unpacked (for development / testing)
#     VM2Backup-Setup.exe      x86 launcher: starts x64\ or x86\VM2Backup-Setup.exe (whichever Windows this is)
#     uninstall.cmd               removal without a window
#     x64\  x86\                  the program and its installer for each bitness
set -e
cd "$(dirname "$0")"
[ -f tools/make_icon.py ] && python3 tools/make_icon.py
# Every package gets a new version, as in Speak2Mic / Show2Cam: 1.0.<day of year>.<minute of day>.
python3 - <<'PY'
import datetime, re
now = datetime.datetime.now()
major, minor, build, rev = 1, 0, now.timetuple().tm_yday, now.hour * 60 + now.minute
p = 'src/version.h'
s = open(p, encoding='utf-8').read()
for name, val in (('MAJOR', major), ('MINOR', minor), ('BUILD', build), ('REV', rev)):
    s = re.sub(r'(#define VM2B_VER_%s\s+)\d+' % name, r'\g<1>%d' % val, s)
s = re.sub(r'(#define VM2B_VER_STR\s+)"[^"]*"', r'\g<1>"%d.%d.%d.%d"' % (major, minor, build, rev), s)
open(p, 'w', encoding='utf-8').write(s)
PY
tools/deps.sh build all          # OpenSSL / libssh2 / liblzma (incremental: only when deps/deps.lock or a lib script changed)
ARCH=x64 ./build.sh
ARCH=x86 ./build.sh

D=dist/VM2Backup
rm -rf dist
mkdir -p $D/x64 $D/x86
cp build/x86/vm2blauncher.exe $D/VM2Backup-Setup.exe
cp scripts/uninstall.cmd $D/
for A in x64 x86; do
    cp build/$A/VM2Backup.exe build/$A/VM2BackupService.exe build/$A/VM2Backup-Setup.exe $D/$A/
    cp scripts/uninstall.cmd $D/$A/
done
cp docs/INSTALL.txt docs/INSTALL.en.txt THIRD-PARTY-NOTICES.md $D/ 2>/dev/null || true
# Необязательная подпись Authenticode (docs/SIGNING.md): включается переменной VM2B_SIGN_PFX (+ VM2B_SIGN_PASSWORD_FILE или VM2B_SIGN_PASSWORD,
# VM2B_SIGN_TIMESTAMP, VM2B_SIGN_URL). Сначала подписываются все exe полезной нагрузки (они попадут в cab уже подписанными), потом - целиком
# итоговый одиночный файл. Без переменных пакет собирается без подписи, как раньше. Пароль в журнал и в командную строку не попадает.
SIGN=0
if [ -n "${VM2B_SIGN_PFX:-}" ]; then
    SIGN=1
    tools/sign.sh --verify $D/VM2Backup-Setup.exe $D/x64/*.exe $D/x86/*.exe
fi
# Windows cmd.exe reads .cmd files in the OEM code page; convert the Russian text to CP866.
for f in $D/uninstall.cmd $D/x64/uninstall.cmd $D/x86/uninstall.cmd; do
    iconv -f UTF-8 -t CP866 "$f" | sed 's/$/\r/' > "$f.tmp" && mv "$f.tmp" "$f"
done

# One file: the launcher + the list of the files with their SHA-256 + the package as a cabinet (MSZIP; Windows extracts it with SetupIterateCabinet) + a trailer
# "VM2BPKG1" + the cabinet's size (little endian, 8 bytes).  The list sits right before the cabinet and ends with "VM2BHSH1" + its size (8 bytes): the launcher
# compares every extracted file with it before the elevated installer starts (security review F7).  See src/vm2blauncher.cpp.
(cd $D && find uninstall.cmd x64 x86 -type f | sort | xargs gcab -c -z ../payload.cab)
(cd $D && find uninstall.cmd x64 x86 -type f | sort) > dist/payload.files
python3 - <<'PY'
import hashlib, struct
launcher = open('build/x86/vm2blauncher.exe', 'rb').read()
cab = open('dist/payload.cab', 'rb').read()
lines = []
for name in open('dist/payload.files').read().split('\n'):
    if not name: continue
    data = open('dist/VM2Backup/' + name, 'rb').read()
    lines.append('%s %d %s\n' % (hashlib.sha256(data).hexdigest(), len(data), name.replace('/', '\\')))
man = ''.join(lines).encode('utf-8') + b''
man_block = man + b'VM2BHSH1' + struct.pack('<Q', len(man))
# the whole file is a multiple of 8 bytes (zeros between the launcher and the list): the signing tool then adds no padding behind the trailer
pad = b'\0' * (-(len(launcher) + len(man_block) + len(cab) + 16) % 8)
open('dist/VM2Backup-Setup.exe', 'wb').write(launcher + pad + man_block + cab + b'VM2BPKG1' + struct.pack('<Q', len(cab)))
PY
rm -f dist/payload.files
rm -f dist/payload.cab
if [ "$SIGN" = 1 ]; then
    tools/sign.sh --verify dist/VM2Backup-Setup.exe
    # a signature that is not there is an error (a missing trusted root is not: osslsigncode then reports "failed" for a test certificate)
    if osslsigncode verify -in dist/VM2Backup-Setup.exe 2>&1 | grep -q 'No signature found'; then echo "ERROR: dist/VM2Backup-Setup.exe has no signature after signing" >&2; exit 1; fi
    echo "SIGNED release build: dist/VM2Backup-Setup.exe carries an Authenticode signature (osslsigncode verify found it)"
else
    # not an error: a development build (and, until a certificate exists, the release) is unsigned; the user then sees "Unknown publisher" in UAC (docs/SIGNING.md)
    echo "WARNING: UNSIGNED release build - dist/VM2Backup-Setup.exe and the programs in it carry no Authenticode signature (set VM2B_SIGN_PFX, see docs/SIGNING.md)" >&2
fi
ls -la dist/VM2Backup-Setup.exe

# Link maps of this build (a crash line in panel.log names module + offset; the map names the function), kept per version.
VER=$(sed -n 's/#define VM2B_VER_STR *"\(.*\)"/\1/p' src/version.h)
mkdir -p "maps/$VER/x64" "maps/$VER/x86"
cp build/x64/*.map "maps/$VER/x64/" 2>/dev/null || true
cp build/x86/*.map "maps/$VER/x86/" 2>/dev/null || true
echo "package: dist/VM2Backup-Setup.exe (version $VER)"
