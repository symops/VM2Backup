#!/bin/bash
# liblzma (xz-utils) for MinGW-w64, static, library only.  build <x64|x86> <srcdir> <prefix>   (prefix = deps/out/<arch>)
set -e
[ "$1" = build ] || exit 2
ARCH=$2; SRCDIR=$3; PREFIX=$4
if [ "$ARCH" = x86 ]; then HOST=i686-w64-mingw32; else HOST=x86_64-w64-mingw32; fi
cd "$SRCDIR"
# Threads: "win95" = CRITICAL_SECTION / events, no winpthreads dependency. Only the LZMA2 encoder is needed (our own streams); the decoders
# stay complete so that an ordinary .xz made by any tool (BCJ/delta filters, LZMA1 in .lzma is not used) can be read.
./configure --host=$HOST --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-static --disable-shared \
    --disable-xz --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links --disable-scripts --disable-doc \
    --disable-nls --disable-symbol-versions --enable-threads=win95 --enable-encoders=lzma1,lzma2,x86 \
    CFLAGS="-O2 -fno-ident" >/dev/null
make -j${DEPS_JOBS:-4} -C src/liblzma
make -C src/liblzma install >/dev/null
rm -rf "$PREFIX/lib/pkgconfig" "$PREFIX/lib"/*.la
cat > "$PREFIX/xz.flags" <<EOT
CFLAGS=-DVM2B_HAVE_LIBLZMA -DLZMA_API_STATIC
LIBS=-llzma
EOT
