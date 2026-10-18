#!/usr/bin/env bash
# Screenshots for the guide and the website, taken in the SDK's Mac cabinet
# simulator: each scene starts a fresh copy of the app on a library, presses
# keys (SIM_KEYS) and saves the playfield, backglass and DMD (SIM_SHOT).
#
#   tools/shots.sh <sample library> [scene...]
#
# <sample library> holds comics/ and manuals/ (public-domain books only: these
# pictures are published). Needs the SDK repo beside this one, Homebrew SDL2,
# and Mac builds of MuPDF / libarchive / libwebp (CR_MAC_WRAP: a folder with a
# clang++ wrapper that adds them; see the build kit). Output: docs/images/.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
LIB=${1:?usage: tools/shots.sh <sample library> [scene...]}
shift || true
SIM=${SIM:-$(dirname "$HERE")/sdk-external-apps-main/sim}
WRAP=${CR_MAC_WRAP:-/Volumes/1TB/.agsdk-build/comic-reader-deps/mac/wrap}
OUT=$HERE/docs/images
APPDIR=$SIM/usb/external/comic-reader
mkdir -p "$OUT"

PATH="$WRAP:$PATH" python3 "$SIM/build.py" "$HERE" | tail -1

# scene: name, keys, shot time (s), which screens ("p", "pb", "pbd"), extra env
shot() {
    local name=$1 keys=$2 when=$3 screens=$4 env=${5:-}
    if [ -n "${ONLY:-}" ] && ! echo " $ONLY " | grep -q " $name "; then return; fi
    local try
    for try in 1 2 3; do
        rm -rf "$APPDIR" && mkdir -p "$APPDIR"
        cp -R "$LIB/comics" "$LIB/manuals" "$APPDIR/"
        # No update check from screenshots (and no "update available" in them).
        mkdir -p "$APPDIR/data" && date +%s > "$APPDIR/data/update-check.txt"
        rm -f "$SIM/shots/playfield.bmp" "$SIM/shots/backglass.bmp" "$SIM/shots/dmd.bmp"
        ( cd "$HERE" && env $env SIM_PREBUILT=1 SIM_SHOT="$when" SIM_KEYS="$keys" "$SIM/run.sh" "$HERE" >/dev/null 2>&1 & pid=$!
          # Until this run's screenshots exist (or it's clearly stuck).
          for i in $(seq 1 $(python3 -c "print(int($when * 2) + 30)")); do
              [ -f "$SIM/shots/playfield.bmp" ] && [ -f "$SIM/shots/dmd.bmp" ] && break
              sleep 0.5
          done
          sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null ) || true
        sleep 1; rm -rf "$APPDIR/data"   # nothing carries over to the next scene
        [ -f "$SIM/shots/playfield.bmp" ] && break
        echo "  $name: no screenshot, again"
    done
    case $screens in *p*) sips -s format jpeg -s formatOptions 82 -Z 1280 "$SIM/shots/playfield.bmp" --out "$OUT/$name.jpg" >/dev/null ;; esac
    case $screens in *b*) sips -s format jpeg -s formatOptions 82 -Z 960 "$SIM/shots/backglass.bmp" --out "$OUT/$name-backglass.jpg" >/dev/null ;; esac
    case $screens in *d*) sips -s format jpeg -s formatOptions 82 -Z 960 "$SIM/shots/dmd.bmp" --out "$OUT/$name-dmd.jpg" >/dev/null ;; esac
    echo "  $name"
}

ONLY="$*"
# The first key waits 5 s: a key pressed while the app is still starting is lost.
shot library   ""                                     6    pbd
shot series    "5:Return"                             7.5  p
shot details   "5:Right 0.5:Right 1:y"                8.5  p
shot reader    "5:Return 1:Return"                    8    pbd
shot zoom      "5:Return 1:Return 2:e 2.5:s"          14   p
shot curl      "5:Return 1:Return 2.5:e"              11   p   COMIC_READER_TURN_HOLD=0.2
shot manuals   "5:e"                                  7.5  p
shot settings  "5:e 0.5:e 1:Down 0.5:Down"            9    p
# Looks Jumbo Comics #82 up in the Grand Comics Database (online; comics.org
# rate-limits its API, so run this one on its own: tools/shots.sh <lib> lookup).
[ -n "$ONLY" ] && shot lookup "5:Return 1:y 1:x"      25   p
echo "screenshots in $OUT"
