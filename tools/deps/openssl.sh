#!/bin/bash
# OpenSSL for MinGW-w64, static.  build <x64|x86> <srcdir> <prefix>   (prefix = deps/out/<arch>)
set -e
[ "$1" = build ] || exit 2
ARCH=$2; SRCDIR=$3; PREFIX=$4
if [ "$ARCH" = x86 ]; then TARGET=mingw; CP=i686-w64-mingw32-; else TARGET=mingw64; CP=x86_64-w64-mingw32-; fi
cd "$SRCDIR"
# enable-legacy: RC2/3DES-encrypted PFX files of old tools (static, loaded only for PKCS12 import). Only what the client/server TLS, X.509, PKCS#12, AES-GCM, PBKDF2 and hashes need. TLS 1.0/1.1 stay (old ESXi), SSLv3/DTLS/engines go.
./Configure $TARGET --cross-compile-prefix=$CP --prefix="$PREFIX" --openssldir=C:/vm2b-no-openssl-dir --libdir=lib \
    no-shared no-tests no-apps no-docs enable-legacy no-engine no-dso no-module no-ssl3 no-ssl3-method no-dtls no-srp no-comp \
    no-cmp no-ct no-gost no-idea no-seed no-rc5 no-bf no-cast no-camellia no-aria no-sm2 no-sm3 no-sm4 no-afalgeng no-uplink \
    no-autoload-config no-ui-console no-thread-pool no-quic no-zlib threads \
    no-ocsp no-cms no-ts no-srtp no-nextprotoneg no-siphash no-whirlpool no-mdc2 no-blake2 no-scrypt no-rfc3779 no-ec2m no-psk \
    -Os -ffunction-sections -fdata-sections
make -j${DEPS_JOBS:-4} build_libs
make install_dev DESTDIR= >/dev/null
rm -rf "$PREFIX/lib/cmake" "$PREFIX/lib/pkgconfig" "$PREFIX/lib/ossl-modules"
cat > "$PREFIX/openssl.flags" <<EOT
CFLAGS=-DVM2B_HAVE_OPENSSL
LIBS=-lssl -lcrypto -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32
EOT
