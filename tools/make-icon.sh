#!/usr/bin/env bash
# Renders assets/metadata/icon.html to assets/metadata/comic-reader.png (1024 x 1536)
# with headless Chrome (it fetches the fonts from Google Fonts).
set -eu
cd "$(dirname "$0")/../assets/metadata"
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new --disable-gpu --hide-scrollbars \
  --force-device-scale-factor=1 --window-size=1024,1536 --virtual-time-budget=8000 \
  --screenshot="$PWD/comic-reader.png" "file://$PWD/icon.html" 2>/dev/null
sips -g pixelWidth -g pixelHeight comic-reader.png | tail -2
