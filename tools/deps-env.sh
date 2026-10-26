#!/bin/bash
# tools/deps-env.sh <x64|x86> [cflags|libs]   prints the compiler flags / the link line of every library built into deps/out/<arch>.
# (every tools/deps/<name>.sh writes deps/out/<arch>/<name>.flags with CFLAGS= and LIBS=; libs are listed in deps.lock order, so
#  each .flags LIBS lists the library itself first, then what it needs, e.g. libssh2: -lssh2 -lssl -lcrypto -lws2_32 ...)
D=$(cd "$(dirname "$0")/.." && pwd)/deps; A=${1:-x64}; P=$D/out/$A
[ -d "$P" ] || exit 0
cf="-I$P/include"; ls="-L$P/lib"
for n in $(grep -v '^[[:space:]]*#' $D/deps.lock | awk 'NF{print $1}'); do
    f=$P/$n.flags; [ -f "$f" ] || continue
    c=$(sed -n 's/^CFLAGS=//p' $f); l=$(sed -n 's/^LIBS=//p' $f)
    cf="$cf $c"
    ls="$ls $l"                       # duplicates are harmless for static libs (and needed when a later library needs an earlier one again)
done
case "$2" in cflags) echo "$cf";; libs) echo "$ls";; *) echo "CFLAGS=\"$cf\""; echo "LIBS=\"$ls\"";; esac
