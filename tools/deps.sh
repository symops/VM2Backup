#!/bin/bash
# Third-party libraries: fetch, cross-build (static, MinGW-w64), build.   See docs/DEV.md.
#   tools/deps.sh fetch [name...]            download the tarballs into deps/src/ and verify the SHA-256 (refuses on mismatch)
#   tools/deps.sh build [x64|x86|all] [name...]   build into deps/out/<arch>/{include,lib}; incremental (stamp = version + script hash)
#   tools/deps.sh list
# Each library has tools/deps/<name>.sh (a line "# deps: openssl" in it makes it rebuild when OpenSSL changes)   `build <arch> <srcdir> <prefix>`  (writes <prefix>/../<name>.flags with CFLAGS=/LIBS= lines).
# Environment: DEPS_JOBS (make -j, default nproc).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOCK=$ROOT/deps/deps.lock; SRC=$ROOT/deps/src; OUT=$ROOT/deps/out; WORK=$ROOT/deps/build
JOBS=${DEPS_JOBS:-$(nproc)}
export DEPS_JOBS=$JOBS
die() { echo "deps: $*" >&2; exit 1; }

entries() { grep -v '^[[:space:]]*#' "$LOCK" | grep -v '^[[:space:]]*$'; }
field() { entries | awk -v n="$1" -v f="$2" '$1==n{print $f}'; }       # field <name> <column>
names() { entries | awk '{print $1}'; }
selected() { if [ $# -gt 0 ]; then for n in "$@"; do [ -n "$(field $n 1)" ] || die "unknown library $n"; echo $n; done; else names; fi; }
tarball() { basename "$(field $1 3)"; }

fetch_one() {
    local n=$1 url sha f got
    url=$(field $n 3); sha=$(field $n 4); f=$SRC/$(tarball $n)
    mkdir -p "$SRC"
    if [ -f "$f" ] && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$sha" ]; then return 0; fi
    echo "deps: downloading $url"
    curl -fsSL --retry 3 -o "$f.tmp" "$url" || { rm -f "$f.tmp"; die "download of $n failed"; }
    got=$(sha256sum "$f.tmp" | cut -d' ' -f1)
    if [ "$got" != "$sha" ]; then rm -f "$f.tmp"; die "SHA-256 MISMATCH for $n: expected $sha, got $got (file refused)"; fi
    mv "$f.tmp" "$f"
}

# A library is rebuilt when its version, its script or the version/script of a library it declares ("# deps: openssl" in its script) changed.
stamp_of() {
    local n=$1 st="$(field $n 2) $(sha256sum "$ROOT/tools/deps/$n.sh" | cut -c1-16) $(field $n 4 | cut -c1-16)" d
    for d in $(sed -n 's/^# deps:[[:space:]]*//p' "$ROOT/tools/deps/$n.sh"); do
        [ -n "$(field $d 1)" ] && st="$st | $d $(field $d 2) $(sha256sum "$ROOT/tools/deps/$d.sh" | cut -c1-16)"
    done
    echo "$st"
}

build_one() {   # build_one <arch> <name>
    local a=$1 n=$2 pre=$OUT/$1/ st want dir
    pre=$OUT/$a; st=$pre/.stamp-$n; want=$(stamp_of $n)
    [ -x "$ROOT/tools/deps/$n.sh" ] || [ -f "$ROOT/tools/deps/$n.sh" ] || die "tools/deps/$n.sh is missing"
    if [ -f "$st" ] && [ "$(cat $st)" = "$want" ] && [ -f "$pre/$n.flags" ]; then echo "deps: $n $(field $n 2) [$a] up to date"; return 0; fi
    fetch_one $n
    dir=$WORK/$a/$n; rm -rf "$dir"; mkdir -p "$dir" "$pre/include" "$pre/lib"
    tar -xf "$SRC/$(tarball $n)" -C "$dir" --strip-components=1 || die "cannot unpack $n"
    echo "deps: building $n $(field $n 2) for $a"
    rm -f "$st"
    ( cd "$dir" && bash "$ROOT/tools/deps/$n.sh" build $a "$dir" "$pre" ) > "$WORK/$a/$n.log" 2>&1 \
        || { tail -20 "$WORK/$a/$n.log"; die "build of $n [$a] FAILED (log: $WORK/$a/$n.log)"; }
    [ -f "$pre/$n.flags" ] || die "$n.sh did not write $pre/$n.flags"
    echo "$want" > "$st"
    rm -rf "$dir"
}

cmd_build() {
    local archs=${1:-all}; [ $# -gt 0 ] && shift
    case "$archs" in x64|x86) ;; all) archs="x64 x86";; *) set -- "$archs" "$@"; archs="x64 x86";; esac
    mkdir -p "$WORK"
    for a in $archs; do mkdir -p "$WORK/$a"; for n in $(selected "$@"); do build_one $a $n; done; done
}

# ---- upstream versions -------------------------------------------------------------------------------------------------
cmd=$1; [ $# -gt 0 ] && shift
case "$cmd" in
    fetch) for n in $(selected "$@"); do fetch_one $n; echo "deps: $n $(field $n 2) ok ($(field $n 4 | cut -c1-12)...)"; done;;
    build) cmd_build "$@";;
    list) entries | awk '{printf "%-10s %-10s %s\n",$1,$2,$5}';;
    *) sed -n '2,9p' "$0"; exit 2;;
esac
