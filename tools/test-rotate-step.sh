#!/bin/bash
# Drive the new rotate stepping commands through the control pipe and read the desktop back
# out of the snapshot each time, so the proof is what the renderer applied, not what was asked.
set -u
cd /h/wallpaper/bld/bin/RelWithDebInfo || exit 1
showing() {
  ./WallpaperRenderer.exe --ctl status 2>&1 | python -c "
import sys,json
d=json.loads(sys.stdin.read())
print(d['monitors'][0]['wallpaper'], 'index=%s' % d['rotate']['index'], 'on=%s' % d['rotate']['on'])
"
}
echo "start:      $(showing)"
for step in next next prev next; do
  ./WallpaperRenderer.exe --ctl rotate "$step" >/dev/null 2>&1
  sleep 1.5
  echo "after $step: $(showing)"
done
echo "restore datarain"
./WallpaperRenderer.exe --ctl apply datarain all >/dev/null 2>&1
sleep 1.5
echo "end:        $(showing)"
