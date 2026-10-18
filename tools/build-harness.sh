#!/usr/bin/env bash
# Builds tools/harness.cpp for aarch64 (in the SDK image, like the app).
set -eu
APP=/workspace/apps/comic-reader
P=/workspace/comic-reader-deps/prefix
OUT=/workspace/comic-reader-deps/out
mkdir -p $OUT/harness
cd $OUT/harness
aarch64-linux-gnu-gcc -O2 -std=c99 -c -I$P/include $APP/src/Doc.c $APP/src/compat.c
for f in Library Covers Pages Transfer Net Update Lookup; do
  aarch64-linux-gnu-g++ -O2 -std=c++14 -c -I$APP/src -I$P/include $APP/src/$f.cpp
done
aarch64-linux-gnu-g++ -O2 -std=c++14 -c -I$APP/src -I$P/include $APP/tools/harness.cpp
aarch64-linux-gnu-g++ -s -o $OUT/harness-bin *.o $P/lib/libmupdf.a $P/lib/libmupdf-third.a $P/lib/libarchive.a $P/lib/libwebpdecoder.a -lpthread -lm
ls -la $OUT/harness-bin
