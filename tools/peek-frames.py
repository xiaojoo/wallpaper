"""tools/peek-frames.py - run inside headless Blender to pull frames out of a screen recording.

Media Foundation on this machine refuses every retarget (RGB32 and NV12 alike come back
E_INVALIDARG), so the product's decoder path cannot be used to look at a video file. Blender ships
its own FFmpeg, which reads H.264 in MP4 without asking MF anything.

Two passes, both in one run:
  1. render every frame at low resolution to PNG (fast, and enough to compute brightness);
  2. load them back with numpy and print mean brightness per frame, flagging jumps.

The frames with the biggest jumps are re-saved larger so a human (or an agent that can look at
images) can inspect the flicker instead of only its number.

usage:
  blender -b --factory-startup --python tools/peek-frames.py -- <mp4> [outDir] [maxBig]
"""
import os
import sys

import bpy
import numpy as np

args = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
# No default source: this used to fall back to a hard-coded Desktop path, which put a personal
# username into a public repo. Ask for it instead.
if not args:
    print("usage: blender -b --python tools/peek-frames.py -- <video.mp4> [out-dir] [max-big]")
    raise SystemExit(2)
src = args[0]
out = args[1] if len(args) > 1 else r'H:\wallpaper\build\peek'
max_big = int(args[2]) if len(args) > 2 else 6
os.makedirs(out, exist_ok=True)

clip = bpy.data.movieclips.load(filepath=src)
w, h = clip.size[0], clip.size[1]
nframes = clip.frame_duration
print(f'clip {src}')
fps = getattr(clip, 'fps', 0) or getattr(clip, 'frame_rate', 0) or 0
print(f'  {w}x{h}  {nframes} frames  start={clip.frame_start}  fps={fps}')

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.render.image_settings.file_format = 'PNG'
scene.render.image_settings.color_mode = 'RGB'
# A screen recording only has to be measured, not admired: 25% keeps a 4K clip cheap.
scene.render.resolution_x, scene.render.resolution_y = w, h
scene.render.resolution_percentage = 25
scene.render.use_sequencer = True
scene.frame_set(clip.frame_start)
scene.frame_start = clip.frame_start
scene.frame_end = clip.frame_start + nframes - 1

editor = scene.sequence_editor or scene.sequence_editor_create()
for seq in list(editor.strips):
    editor.strips.remove(seq)
strip = editor.strips.new_movie('peek', src, 1, clip.frame_start)
strip.frame_start = clip.frame_start

scene.render.filepath = out + os.sep
bpy.ops.render.render(animation=True)

files = sorted(f for f in os.listdir(out) if f.endswith('.png'))
print(f'  rendered {len(files)} frames')

stats = []
for f in files:
    img = bpy.data.images.load(os.path.join(out, f))
    px = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    lum = px.reshape(-1, 4)[:, :3].mean(axis=1)
    stats.append((f, float(lum.mean()), float(lum.max()), float(lum.min())))
    bpy.data.images.remove(img)

print('  file                 mean    min    max   dMean')
jumps = []
prev = None
for f, mean, mx, mn in stats:
    d = 0.0 if prev is None else mean - prev
    mark = ''
    if prev is not None and abs(d) > 0.02:
        mark = f'  <== 亮度跳变 {d:+.3f}'
        jumps.append((abs(d), f, mean))
    print(f'  {f:>22} {mean:6.3f} {mn:6.3f} {mx:6.3f} {d:+7.3f}{mark}')
    prev = mean

# Re-save the worst offenders at full size so the flicker can be looked at, not just counted.
jumps.sort(reverse=True)
scene.render.resolution_percentage = 100
for _, f, mean in jumps[:max_big]:
    idx = int(os.path.splitext(f)[0].split('_')[-1])
    scene.frame_set(clip.frame_start + idx)
    scene.render.filepath = out + os.sep + f'big_{idx:05d}.png'
    bpy.ops.render.render(write_still=True)
    print(f'  saved big_{idx:05d}.png (mean={mean:.3f})')
print(f'DONE jumps={len(jumps)} big={min(max_big, len(jumps))}')
