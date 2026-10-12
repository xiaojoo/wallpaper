#!/bin/bash
# 无头断言：把预览第二遍强开起来，量它自己跑不跑得满，以及桌面那一遍有没有被拖累。
# 这是 2026-10-12 那轮"流提到 3840x2160 值不值"的探针删掉之后留下的那把尺子——探针量的是
# 每帧几毫秒，这条断言守的是那轮量出来的结论：投递要 >= 25 fps，桌面那一遍不许变慢。
#
# 预览是排队命令，回答只有 {"ok":true,"queued":...}，所以判据一律从 status 现读；引擎的节拍
# 看门狗 3 s 收掉第二遍，因此每一拍都要重新敲一次 preview。跑完不留状态：不再敲，看门狗自己收。
#
# usage: tools/check-preview-pace.sh [wallpaper-id] [seconds]
set -u
cd /h/wallpaper/bld/bin/RelWithDebInfo || exit 1
ID=${1:-matrixclock}
SAMPLES=${2:-8}

# 一行三个数：投递 fps、累计 dropped、桌面那一遍的 avg_frame_ms
read_status() {
  ./WallpaperRenderer.exe --ctl status 2>&1 | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
except Exception:
    print('0 0 0'); raise SystemExit
p, m = d.get('preview', {}), d['monitors'][0]
print('%s %s %s' % (p.get('fps') or 0, p.get('dropped') or 0, float(m.get('avg_frame_ms') or 0)))
"
}

best=0
drop0=$(read_status | cut -d' ' -f2)
slot0=$(read_status | cut -d' ' -f3)
for _ in $(seq "$SAMPLES"); do
  ./WallpaperRenderer.exe --ctl preview "$ID" >/dev/null 2>&1
  sleep 1
  line=$(read_status)
  fps=$(echo "$line" | cut -d' ' -f1)
  drop=$(echo "$line" | cut -d' ' -f2)
  slot=$(echo "$line" | cut -d' ' -f3)
  best=$(python -c "print(max($best, $fps))")
done
echo "preview '$ID': delivered fps best=$best, dropped +$((drop - drop0)), desktop avg_frame_ms $slot0 -> $slot"
python -c "
import sys
ok = $best >= 25 and ($slot - $slot0) <= 1.0
print('FAIL: the pass cannot keep pace, or the desktop paid for it' if not ok
      else 'OK: >= 25 fps delivered and the desktop pass did not slow down')
sys.exit(0 if ok else 1)
"
