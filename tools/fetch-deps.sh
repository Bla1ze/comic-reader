#!/usr/bin/env bash
# Downloads MuPDF and libarchive sources into <build folder>/comic-reader-deps/dl
# and checks them against tools/deps.sha256. Run on the Mac before build-deps.sh.
#   tools/fetch-deps.sh /Volumes/1TB/.agsdk-build
set -eu
ROOT="${1:?usage: tools/fetch-deps.sh <build folder>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DL="$ROOT/comic-reader-deps/dl"
mkdir -p "$DL"
cd "$DL"
[ -f mupdf-1.28.5-source.tar.gz ] || curl -fLO https://mupdf.com/downloads/archive/mupdf-1.28.5-source.tar.gz
[ -f libarchive-3.8.10.tar.xz ] || curl -fLO https://github.com/libarchive/libarchive/releases/download/v3.8.10/libarchive-3.8.10.tar.xz
shasum -a 256 -c "$HERE/deps.sha256"
