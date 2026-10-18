#!/usr/bin/env bash
# Builds Comic Reader's static libraries for the cabinet: libarchive (RAR/7z
# for MuPDF's CBR support) and MuPDF, with the SDK's own compiler (GCC 5.4,
# glibc 2.26 target). Run in the SDK image with the build folder at /workspace:
#   docker run --rm --platform linux/amd64 -v <build folder>:/workspace \
#     atgames-external-sdk:glibc-2.26-sdl2-v1 bash /workspace/apps/comic-reader/tools/build-deps.sh
# Sources go in /workspace/comic-reader-deps/dl (fetched on the Mac first, see
# fetch-deps.sh); output in /workspace/comic-reader-deps/prefix.
set -eu
D=/workspace/comic-reader-deps
APP=/workspace/apps/comic-reader
P=$D/prefix
J=$(nproc)
MUPDF=mupdf-1.28.5-source
ARCHIVE=libarchive-3.8.10
mkdir -p $D/src $D/out $P/lib $P/include

(cd $D/dl && sha256sum -c $APP/tools/deps.sha256)

if [ ! -f $P/lib/libarchive.a ]; then
  rm -rf $D/src/$ARCHIVE && tar -xJf $D/dl/$ARCHIVE.tar.xz -C $D/src
  cd $D/src/$ARCHIVE
  ./configure --host=aarch64-linux-gnu --prefix=$P --disable-shared --enable-static \
    --disable-bsdtar --disable-bsdcpio --disable-bsdcat --disable-bsdunzip \
    --disable-acl --disable-xattr --without-openssl --without-xml2 --without-expat \
    --without-lzma --without-zstd --without-lz4 --without-bz2lib --without-libb2 \
    --without-zlib --without-mbedtls --without-nettle --without-cng \
    CFLAGS="-O2" > $D/out/libarchive.configure.log 2>&1 || { tail -30 $D/out/libarchive.configure.log; exit 1; }
  make -j$J > $D/out/libarchive.log 2>&1 || { grep -m10 error $D/out/libarchive.log; exit 1; }
  make install > /dev/null
  # MuPDF includes "libarchive/archive.h".
  mkdir -p $P/include/libarchive && cp $P/include/archive*.h $P/include/libarchive/
fi

if [ ! -f $P/lib/libmupdf.a ]; then
  rm -rf $D/src/$MUPDF
  # freeglut and zxing-cpp aren't built, and their symlinks can't be made on
  # some shared folders.
  tar -xzf $D/dl/$MUPDF.tar.gz -C $D/src --exclude=$MUPDF/thirdparty/freeglut --exclude=$MUPDF/thirdparty/zxing-cpp
  cd $D/src/$MUPDF
  for p in $APP/tools/patches/mupdf-*.patch; do patch -p1 < "$p"; done
  make -j$J build=release OS=Linux \
    CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ AR=aarch64-linux-gnu-ar \
    HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_OBJCOPY=no HAVE_LIBCRYPTO=no \
    brotli=no mujs=no html=no xps=no svg=no extract=no tofu_cjk=yes \
    archive=yes HAVE_SYS_LIBARCHIVE=yes SYS_LIBARCHIVE_CFLAGS="-I$P/include" \
    XCFLAGS="-O2" \
    libs > $D/out/mupdf.log 2>&1 || { grep -m15 -E "error|Error" $D/out/mupdf.log; exit 1; }
  B=build/release-tofu_cjk-archive
  cp $B/libmupdf.a $B/libmupdf-third.a $P/lib/
  rm -rf $P/include/mupdf && cp -R include/mupdf $P/include/
fi
ls -la $P/lib
