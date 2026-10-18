#!/usr/bin/env bash
# Builds the release files from a built ELF:
#   comic-reader-v<version>.zip   the app: ELF, png, xml, licenses, and the guide
#                                 preloaded in manuals/ (the updater replaces it too)
#   comic-reader-sample-comics.zip (with a library) public-domain books to try
#   SHA256SUMS.txt
#
#   tools/make-release.sh <version> <comic-reader.elf> <out dir> [sample library]
#
# The sample library holds comics/ and manuals/ and must hold public-domain
# books only (it's published). The guide is left out of it (it's in the app zip).
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:?usage: make-release.sh <version> <elf> <out dir> [sample library]}
ELF=${2:?elf}
OUT=${3:?out dir}
LIB=${4:-}
grep -q "#define COMIC_READER_VERSION \"$VER\"" "$HERE/src/Version.h" || { echo "src/Version.h isn't $VER"; exit 1; }
strings "$ELF" | grep -qx "$VER" || { echo "$ELF isn't version $VER"; exit 1; }
[ -f "$HERE/docs/Comic Reader Guide.pdf" ] || { echo "no docs/Comic Reader Guide.pdf"; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
T=$(mktemp -d)
A=$T/app/external/comic-reader
mkdir -p "$A/manuals" "$A/comics"
cp "$ELF" "$A/comic-reader.elf"
cp "$HERE/assets/metadata/comic-reader.png" "$HERE/assets/metadata/comic-reader.xml" "$A/"
cp -R "$HERE/licenses" "$A/"
cp "$HERE/docs/Comic Reader Guide.pdf" "$A/manuals/"
cp "$HERE/docs/comics-README.txt" "$A/comics/README.txt"
rm -f "$OUT/comic-reader-v$VER.zip"
(cd "$T/app" && zip -qr -X "$OUT/comic-reader-v$VER.zip" external -x "*.DS_Store")
SUMS="comic-reader-v$VER.zip"
if [ -n "$LIB" ]; then
  S=$T/sample/external/comic-reader
  mkdir -p "$S"
  cp -R "$LIB/comics" "$LIB/manuals" "$S/"
  rm -f "$S/manuals/Comic Reader Guide.pdf" "$S/comics/README.txt"
  rm -f "$OUT/comic-reader-sample-comics.zip"
  (cd "$T/sample" && zip -qr -X "$OUT/comic-reader-sample-comics.zip" external -x "*.DS_Store")
  SUMS="$SUMS comic-reader-sample-comics.zip"
fi
(cd "$OUT" && shasum -a 256 $SUMS > SHA256SUMS.txt && cat SHA256SUMS.txt)
unzip -l "$OUT/comic-reader-v$VER.zip" | grep -E "elf|Guide|README" 
rm -rf "$T"
