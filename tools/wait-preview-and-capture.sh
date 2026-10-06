#!/bin/bash
# Captures the settings window's big picture once, at the moment the renderer's preview feed is
# actually drawing the wallpaper we want - the carousel steps every 4 s and there is no way to stop
# it from outside, so waiting for the right slide beats restarting the window.
# usage: wait-preview-and-capture.sh <local_01> <out.png>
set -u
WANT=${1:-local_01}
OUT=${2:-/h/wallpaper/build/shots/pc.png}
BIN=/h/wallpaper/bld/bin/RelWithDebInfo
for i in $(seq 1 60); do
  id=$(cd "$BIN" && ./WallpaperRenderer.exe --ctl status 2>/dev/null \
       | python -c 'import sys,json;print(json.load(sys.stdin).get("preview",{}).get("id",""))' 2>/dev/null)
  if [ "$id" = "$WANT" ]; then
    powershell -NoProfile -ExecutionPolicy Bypass \
      -File 'H:/wallpaper/tools/capture-window.ps1' -Out "$(cygpath -w "$OUT")" -Title 'SmartWallpaper' | tail -1
    echo "slide=$WANT after ${i} polls"
    exit 0
  fi
  sleep 0.5
done
echo "TIMEOUT: preview never showed $WANT (last=$id)"
exit 1
