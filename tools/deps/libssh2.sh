#!/bin/bash
# libssh2 for MinGW-w64, static, OpenSSL backend (the OpenSSL of deps/out/<arch>), no zlib.  build <x64|x86> <srcdir> <prefix>
# deps: openssl
set -e
[ "$1" = build ] || exit 2
ARCH=$2; SRCDIR=$3; PREFIX=$4
if [ "$ARCH" = x86 ]; then HOST=i686-w64-mingw32; else HOST=x86_64-w64-mingw32; fi
[ -f "$PREFIX/lib/libcrypto.a" ] || { echo "libssh2 needs openssl built first"; exit 1; }
cd "$SRCDIR"
# PATCHES-BEGIN (tools/fuzz-ssh-keys.sh applies exactly these lines to a native ASan build)
# Upstream 1.11.1 bug: _libssh2_swap_bytes(buf, 0) starts at j = len - 1 = ULONG_MAX and reads/writes buf[0xFFFFFFFF] (a page fault).  An OpenSSH ECDSA
# private key whose exponent is all zero (a damaged key file) gets there: the whole process died.  An empty buffer has nothing to swap.
sed -i 's/for(i = 0, j = len - 1; i < j; i++, j--) {/for(i = 0, j = len ? len - 1 : 0; i < j; i++, j--) {/' src/openssl.c
grep -q 'j = len ? len - 1 : 0' src/openssl.c || { echo "libssh2 patch (swap_bytes) did not apply"; exit 1; }
# Three more found by fuzzing damaged key files natively under ASan: _libssh2_base64_decode allocates (n/4)*3+1 but writes up to (n/4)*3+2 bytes when n % 4 is 2
# or 3 (a truncated key file: heap overflow); and the decrypted OpenSSH key blob is not NUL-terminated although strlen() is run on strings inside it.
sed -i 's|LIBSSH2_ALLOC(session, ((src_len / 4) \* 3) + 1);|LIBSSH2_ALLOC(session, ((src_len / 4) * 3) + 3);|' src/misc.c
grep -q '((src_len / 4) \* 3) + 3' src/misc.c || { echo "libssh2 patch (base64) did not apply"; exit 1; }
sed -i 's|out_buf->data = LIBSSH2_CALLOC(session, decrypted.len);|out_buf->data = LIBSSH2_CALLOC(session, decrypted.len + 1);|' src/pem.c
grep -q 'decrypted.len + 1' src/pem.c || { echo "libssh2 patch (pem) did not apply"; exit 1; }
# And a DOUBLE FREE: the three error exits of the bcrypt key derivation in pem.c free `key` and then `goto out`, which frees it again (an encrypted OpenSSH key with
# no / a wrong passphrase, or a damaged kdf block; heap corruption that kills the process some calls later).
sed -i '/^ *LIBSSH2_FREE(session, key);$/{N;/goto out;/s/^ *LIBSSH2_FREE(session, key);\n//}' src/pem.c
[ "$(grep -c 'LIBSSH2_FREE(session, key);' src/pem.c)" = 1 ] || { echo "libssh2 patch (pem double free) did not apply"; exit 1; }
# PATCHES-END
./configure --host=$HOST --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-static --disable-shared \
    --with-crypto=openssl --with-libssl-prefix="$PREFIX" --without-libz --disable-examples-build --disable-docker-tests --disable-tests --disable-debug \
    CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" LIBS="-lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32" CFLAGS="-O2 -fno-ident" >/dev/null
make -j${DEPS_JOBS:-4} -C src
make -C src install >/dev/null
make install-data >/dev/null 2>&1 || true
rm -rf "$PREFIX/lib/pkgconfig" "$PREFIX/lib"/*.la "$PREFIX/share"
cat > "$PREFIX/libssh2.flags" <<EOT
CFLAGS=-DVM2B_HAVE_LIBSSH2 -DLIBSSH2_API=
LIBS=-lssh2 -lssl -lcrypto -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32
EOT
