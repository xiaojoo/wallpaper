"""tools/rank-frames.py - rank already-rendered frames by how much they changed.

peek-frames.py renders a recording to small PNGs; this one reads them back and reports the frames
whose *content* moved the most, in three separate ways, because "the screen flickered" can mean any
of them:

  dMean  - whole-frame brightness moved            (a global flash or a black frame)
  dark   - the fraction of near-black pixels jumped (part of the screen went black: the wallpaper
           surface dropping out is exactly this shape, and it barely moves the global mean)
  diff   - how many pixels differ from the previous frame by a lot (a swap of contents)

usage: blender -b --factory-startup --python tools/rank-frames.py -- <dir> [topN]
"""
import os
import sys

import bpy
import numpy as np

args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
d = args[0] if args else r'H:\wallpaper\build\peek'
top = int(args[1]) if len(args) > 1 else 12
files = sorted(f for f in os.listdir(d) if f.endswith('.png') and not f.startswith('big_'))
if len(files) < 3:
    print(f'FATAL only {len(files)} frames in {d}')
    raise SystemExit(1)

rows = []
dmean, ddark, ndiff = [], [], []
prev = None
for i, f in enumerate(files):
    img = bpy.data.images.load(os.path.join(d, f))
    w, h = img.size[0], img.size[1]
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    rgb = px.reshape(-1, 4)[:, :3]
    lum = rgb.mean(axis=1)
    mean = float(lum.mean())
    dark = float((lum < 0.04).mean())
    rows.append(dict(f=f, mean=mean, dark=dark))
    if prev is not None:
        dmean.append((abs(mean - prev[0]), i))
        ddark.append((abs(dark - prev[1]), i))
        # a 4x4 subsampled grid keeps this cheaper than the render was
        big = float((np.abs(rgb[::4].astype(np.int16) - prev[2]).max(axis=1) > 8).mean())
        ndiff.append((big, i))
    prev = (mean, dark, rgb.astype(np.int16)[::4])
print(f"{len(rows)} frames, {w}x{h}  (阈值: 通道差>8 算变化)")

def report(name, arr, note):
    arr = sorted(arr, reverse=True)[:top]
    print(f'--- {name} 最大的 {len(arr)} 处  ({note})')
    for v, i in arr:
        t = i / 24.0
        print(f'   {rows[i]["f"]:>16}  值={v:9.5f}  第{i}帧  t={t:5.2f}s  '
              f'mean={rows[i]["mean"]:.3f} dark={rows[i]["dark"]:.3f}')

report('整幅亮度变化', dmean, 'dMean：全局闪白/闪黑')
report('近黑像素占比变化', ddark, 'dark：局部变黑，壁纸表面掉出去就是这个形状')
report('相邻帧差异像素', ndiff, 'diff：内容被换掉')
print('RANKDONE')
