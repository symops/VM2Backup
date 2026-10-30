#!/bin/bash
# Варианты установщика для проверки подписи на настоящей Windows (фазы signing и signinst, docs/WINDOWS-TESTS.md, раунд 8) -> .winvm/shared/pki/variants/:
#   unsigned.exe       обычный пакет без подписи
#   signed.exe         пакет, подписанный тестовым сертификатом (подписаны и все exe внутри: package.sh с VM2B_SIGN_PFX)
#   tampered.exe       signed.exe с изменённым ПОСЛЕ подписи байтом внутри PE загрузчика (строка ресурса): подпись HashMismatch, пакет цел
#   tampered-cab.exe   signed.exe с изменённым байтом внутри самого пакета (cab): подпись не проходит И данные повреждены (установщик должен сказать об ошибке понятно)
# Порядок: сначала подписанная сборка, потом обычная (dist остаётся обычным: именно он идёт в остальные фазы). Ключи и PFX остаются в .winvm/shared/pki (в .gitignore).
#   tools/winvm-variants.sh
set -eu
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R"
PKI=$R/.winvm/shared/pki; V=$PKI/variants
bash tools/make-test-cert.sh "$PKI" >/dev/null; mkdir -p "$V"
echo "== подписанная сборка"
VM2B_SIGN_PFX=$PKI/test-sign.pfx VM2B_SIGN_PASSWORD_FILE=$PKI/test-sign.pass VM2B_SIGN_CA=$PKI/test-root.pem ./package.sh > "${TMPDIR:-/tmp}/winvm-variants-signed.log" 2>&1
cp dist/VM2Backup-Setup.exe "$V/signed.exe"
echo "== обычная сборка"
./package.sh > "${TMPDIR:-/tmp}/winvm-variants-plain.log" 2>&1
cp dist/VM2Backup-Setup.exe "$V/unsigned.exe"
python3 - "$V/signed.exe" "$V/tampered.exe" "$V/tampered-cab.exe" <<'PY'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
a = bytearray(d); i = a.find('VM2Backup setup'.encode('utf-16le')); assert i > 0; a[i] ^= 0x01        # строка внутри PE загрузчика
open(sys.argv[2], 'wb').write(a)
b = bytearray(d); c = b.find(b'MSCF'); assert c > 0
size = int.from_bytes(b[c + 8:c + 12], 'little'); j = c + size // 2; b[j] ^= 0xFF                      # середина cab (данные MSZIP)
open(sys.argv[3], 'wb').write(b)
PY
ls -la "$V"
for f in unsigned signed tampered tampered-cab; do printf '%s: ' $f; osslsigncode verify -in "$V/$f.exe" -CAfile "$PKI/test-root.pem" 2>&1 | grep -E 'Signature verification|MISMATCH|No signature' | head -1; done
