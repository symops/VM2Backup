#!/bin/bash
# Native AddressSanitizer fuzz of the private-key parsers of libssh2 (the Wine tests cannot see heap overflows / double frees; Wine's heap is forgiving).
# Builds libssh2 (the version in deps/deps.lock, with the patches of tools/deps/libssh2.sh) against the system OpenSSL with -fsanitize=address,undefined, then mutates
# ssh-keygen keys of every kind (truncate, flip, insert, 'A'-fill) and loads them.  Any sanitizer report = a bug.  Needs: gcc, libssl-dev, ssh-keygen.
#   tools/fuzz-ssh-keys.sh [iterations per worker (default 300)] [workers (default 4)] [first seed (default 1)]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IT=${1:-300}; W=${2:-4}; S0=${3:-1}
T=$(mktemp -d /tmp/vm2b-keyfuzz.XXXXXX); trap 'rm -rf "$T"' EXIT
tar -xzf "$ROOT"/deps/src/libssh2-*.tar.gz -C "$T"; L=$(echo "$T"/libssh2-*)
( cd "$L" && sed -n '/^# PATCHES-BEGIN/,/^# PATCHES-END/p' "$ROOT/tools/deps/libssh2.sh" > "$T/patches.sh" && bash -e "$T/patches.sh" &&
  ./configure --disable-shared --enable-static --with-crypto=openssl --without-libz --disable-examples-build --disable-docker-tests --disable-debug \
      CFLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" >/dev/null &&
  make -j"$(nproc)" -C src >/dev/null )
gcc -g -O1 -fsanitize=address,undefined -I "$L/include" "$ROOT/tools/testservers/fuzz_keys.c" "$L/src/.libs/libssh2.a" -lssl -lcrypto -o "$T/fuzz_keys"
K=$T/keys; mkdir -p "$K"; P='pass phrase 123'
keygen() { ssh-keygen -q -t "$1" $4 -N "$5" -f "$K/$2" $3 </dev/null; }
keygen ed25519 u_ed25519 "" "" ""; keygen ed25519 u_ed25519_pw "" "" "$P"; keygen rsa u_rsa "" "-b 2048" ""; keygen rsa u_rsa_pem "-m PEM" "-b 2048" ""
keygen rsa u_rsa_pkcs8 "-m PKCS8" "-b 2048" ""; keygen ecdsa u_ecdsa256 "" "-b 256" ""; keygen ecdsa u_ecdsa384_pw "" "-b 384" "$P"; keygen ecdsa u_ecdsa521 "" "-b 521" ""
keygen ecdsa u_ecdsa_pem "-m PEM" "-b 256" ""; keygen ed25519 u_ed25519_pkcs8 "-m PKCS8" "" ""
PIDS=""; for i in $(seq 0 $((W-1))); do
  ( ASAN_OPTIONS=detect_leaks=0:handle_sigalrm=1:handle_abort=1 "$T/fuzz_keys" $((S0+i)) "$IT" "$K" "$T/mut$i.key" > "$T/out$i.txt" 2>&1; echo $? > "$T/rc$i" ) &
  PIDS="$PIDS $!"
done
wait $PIDS
bad=0; for i in $(seq 0 $((W-1))); do if [ "$(cat "$T/rc$i")" != 0 ] || ! grep -q '^done' "$T/out$i.txt"; then bad=1; echo "worker $i (seed $((S0+i))) FAILED:"; head -25 "$T/out$i.txt"; fi; done
[ $bad = 0 ] && echo "key fuzz: $W workers x $IT iterations x 10 key kinds, no sanitizer report"
exit $bad
