#!/bin/bash
# tools/verify-video.sh - drive the video wallpaper path end to end and print the numbers.
#
#   tools/verify-video.sh H:/wallpaper/build/fixtures/fixture_1080.mp4 [monitor]
#
# It imports the file, applies it, and reads the renderer's own counters back over the control
# pipe twice, three seconds apart, so "decode fps" is a measured delta rather than a claim. Then it
# checks the transport: pause must stop the counter, seek must move the position.
# Run tools/make-fixture.sh first to (re)create the known-content clips.
set -u
cd /h/wallpaper
FILE="${1:?usage: verify-video.sh <path to mp4|wmv|...> [monitor]}"
MON="${2:-all}"
E=bld/bin/RelWithDebInfo/WallpaperRenderer.exe
ctl() { "$E" --ctl "$@" 2>&1; }
stat() { ctl status | python tools/video-status.py; }

echo "=== renderer state before"
stat || true

# Start a development instance in its own windows if nothing is answering the pipe. This never takes
# over the desktop, so it cannot leave someone's wallpaper pointing at a test clip.
if ! ctl status >/dev/null 2>&1; then
  echo "=== no renderer on the pipe; starting --window --no-power"
  ( cd bld/bin/RelWithDebInfo && ./WallpaperRenderer.exe --window --no-power --no-tray --log info >/dev/null 2>&1 & )
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    ctl status >/dev/null 2>&1 && break
  done
fi

echo "=== import"
before=$(ctl status | python -c 'import json,sys; d=sys.stdin.read(); i=d.find("{"); print(len(json.loads(d[i:]).get("catalog",[])))')
t0=$(date +%s%N)
ctl addvideo "$FILE"
id=""
for _ in $(seq 30); do
  sleep 1
  id=$(ctl status | python -c '
import json,sys
d=sys.stdin.read(); i=d.find("{"); s=json.loads(d[i:])
if len(s.get("catalog",[])) > '"$before"':
    v=[c for c in s["catalog"] if c.get("type")=="video"]
    print(v[-1]["id"] if v else "")
')
  [ -n "$id" ] && break
done
ms=$(( ($(date +%s%N) - t0) / 1000000 ))
echo "imported as '${id:-NONE}' in ${ms} ms (that wait is on the render thread)"
[ -z "$id" ] && { echo "import failed, renderer log:"; grep -a "\[video\]\|addvideo" bld/bin/RelWithDebInfo/logs/renderer-*.log | tail -6; exit 1; }

echo "=== apply $id to $MON"
ctl apply "$id" "$MON" | head -2
sleep 4
echo "=== sample A"
stat
a=$(ctl status | python -c '
import json,sys
d=sys.stdin.read(); i=d.find("{"); s=json.loads(d[i:])
m=s["monitors"][0]; print(m.get("video_frames_delivered",0), m.get("video_pos",0), m.get("process",{}) and s["process"]["cpu_percent"])')
sleep 3
echo "=== sample B (3 s later)"
stat
b=$(ctl status | python -c '
import json,sys
d=sys.stdin.read(); i=d.find("{"); s=json.loads(d[i:])
m=s["monitors"][0]; print(m.get("video_frames_delivered",0), m.get("video_pos",0), s["process"]["cpu_percent"])')
python - "$a" "$b" <<'PY'
fa, pa, _ = [float(x) for x in input().split()]
fb, pb, cpu = [float(x) for x in input().split()]
print(f"=== decode: {fb - fa:.0f} frames in ~3 s = {(fb - fa) / 3.0:.2f} fps delivered, "
      f"position moved {pb - pa:.2f} s, process cpu {cpu:.2f} %")
PY

echo "=== pause"
ctl video pause | head -3
sleep 2
stat
echo "=== seek 5.0"
ctl video seek 5 | head -3
sleep 1
stat
echo "=== resume"
ctl video resume | head -2
sleep 2
stat
echo "=== nvidia decode utilisation (dec% column is the video engine)"
nvidia-smi dmon -s u -c 5 2>&1 | tail -7
