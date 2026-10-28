#!/bin/bash
# ТЕСТОВЫЙ центр сертификации и сертификат подписи кода (RSA 3072 / SHA-256, EKU 1.3.6.1.5.5.7.3.3) для проверки подписи на тестовых ВМ.
#   tools/make-test-cert.sh [каталог]     по умолчанию .winvm/shared/pki (в .gitignore; ключи и PFX в репозиторий не попадают)
# Результат: test-root.pem / test-root.cer (корень, его можно ставить в доверенные на ГОСТЕ), test-sign.pfx (+ test-sign.pass: пароль), test-sign.cer.
# Такой сертификат НЕ даёт репутации SmartScreen / Smart App Control: это только проверка механики подписи (docs/SIGNING.md).
set -eu
D=${1:-$(cd "$(dirname "$0")/.." && pwd)/.winvm/shared/pki}
mkdir -p "$D"; chmod 700 "$D"; cd "$D"; umask 077
[ -f test-sign.pfx ] && { echo "уже есть: $D/test-sign.pfx"; exit 0; }
head -c 24 /dev/urandom | base64 | tr -d '/+=\n' > test-sign.pass
cat > ca.cnf <<'C'
[req]
distinguished_name=dn
prompt=no
x509_extensions=v3
[dn]
O=VM2Backup TEST
CN=VM2Backup TEST Root CA
[v3]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
C
cat > leaf.cnf <<'C'
[req]
distinguished_name=dn
prompt=no
[dn]
O=Symo (TEST)
CN=Symo TEST Code Signing
[v3]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=critical,1.3.6.1.5.5.7.3.3
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
C
openssl req -x509 -newkey rsa:3072 -nodes -keyout test-root.key -out test-root.pem -days 3650 -sha256 -config ca.cnf
openssl req -new -newkey rsa:3072 -nodes -keyout test-sign.key -out test-sign.csr -sha256 -config leaf.cnf
openssl x509 -req -in test-sign.csr -CA test-root.pem -CAkey test-root.key -CAcreateserial -out test-sign.pem -days 1825 -sha256 -extfile leaf.cnf -extensions v3
openssl pkcs12 -export -inkey test-sign.key -in test-sign.pem -certfile test-root.pem -out test-sign.pfx -passout file:test-sign.pass
openssl x509 -in test-root.pem -outform der -out test-root.cer
openssl x509 -in test-sign.pem -outform der -out test-sign.cer
rm -f test-sign.csr ca.cnf leaf.cnf test-root.srl
echo "готово: $D"; openssl x509 -in test-sign.pem -noout -subject -issuer -dates -ext extendedKeyUsage
