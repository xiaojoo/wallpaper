#!/usr/bin/env python3
"""tools/probe-orbit-fade.py - how transparent each ring icon actually is, measured off the screen.

Run tools/probe-orbit-fade.ps1 first: it grabs the same crop with the ring on and then off, several
times, with the ring frozen. Each pair's off frame is the background its on frame was composited over,
so the difference is the icon's own contribution; the spread of the answer across pairs is the error
bar, and it is not small - the wallpaper under the ring is a clock whose digits change on the same
timescale as the toggle.

The model is the blend state the pass actually uses, premultiplied over:
    observed = fade * icon_pma + (1 - fade * icon_a) * background
so with the ring-off frame as the background, one number falls out per channel:
    fade = sum(observed - background) / sum(icon_pma - icon_a * background)
summed over the icon's own box, which weights each artwork pixel by exactly the amount it covers.
"""
import glob, os, re, sys
import numpy as np
from PIL import Image

BIN = r"H:\wallpaper\bld\bin\RelWithDebInfo"
TMP = r"H:\wallpaper\tools\tmp"
CROP = (1500, 440)
CELL = 256
PAD = 8                     # ShellIcons::kCellPad
COLS, ROWS = 8, 6


def rects():
    logs = sorted(glob.glob(os.path.join(BIN, "logs", "renderer-*.log")), key=os.path.getmtime)
    if not logs:
        sys.exit("no renderer log")
    line = None
    with open(logs[-1], "r", encoding="utf-8", errors="replace") as f:
        for l in f:
            if "orbit rects" in l:
                line = l
    if line is None:
        sys.exit(f"no 'orbit rects' line in {logs[-1]} - is the ring on and clickable?")
    out = []
    for slot, cx, cy, half in re.findall(r"(\d+):(\d+),(\d+)\+(\d+)", line.split("slot:", 1)[1]):
        out.append((int(slot), int(cx), int(cy), int(half)))
    return os.path.basename(logs[-1]), out


def load(name, root=TMP):
    # 0..1 everywhere: the atlas side of the equation arrives as bytes and has to be divided the same
    # way, or the denominator mixes a 0..1 icon with a 0..255 background and every fade comes out a
    # large negative number, which is what this probe's first run reported.
    return np.asarray(Image.open(os.path.join(root, name)).convert("RGBA"), dtype=np.float64) / 255.0


def cell_of(atlas, slot, size):
    """The artwork the ring actually paints for this slot, area-averaged down to the box it is drawn
    into. The cell keeps a transparent gutter (ShellIcons::kCellPad) around the artwork and the engine
    sizes the click rectangle to the artwork, so the crop has to start at the gutter, not at the cell
    edge - measuring the whole cell against the artwork rect biases every fade low by that 6%."""
    col, row = slot % COLS, slot // COLS
    p = PAD
    sub = atlas[row * CELL + p:(row + 1) * CELL - p, col * CELL + p:(col + 1) * CELL - p]
    im = Image.fromarray(np.rint(sub * 255.0).astype(np.uint8), "RGBA")
    im = im.resize((size, size), Image.BOX)          # same footprint the hardware's mip takes
    return np.asarray(im, dtype=np.float64) / 255.0


def main():
    logname, rs = rects()
    atlas = load("orbit-atlas.png", os.path.join(BIN, "cache"))
    pairs = []
    for name in sorted(os.listdir(TMP)):
        m = re.fullmatch(r"fade_on(\d+)\.png", name)
        if m and os.path.exists(os.path.join(TMP, f"fade_off{m.group(1)}.png")):
            pairs.append(m.group(1))
    if not pairs:
        sys.exit("no fade_on<i>.png / fade_off<i>.png in tools/tmp - run tools/probe-orbit-fade.ps1")
    first = load(f"fade_on{pairs[0]}.png")
    print(f"log {logname}  {len(rs)} rects  crop {CROP} {first.shape[1]}x{first.shape[0]}  "
          f"{len(pairs)} pairs")
    ys = [r[2] for r in rs]
    ylo, yhi = min(ys), max(ys)
    print(f"{'slot':>4} {'box':>5} {'depth t':>8} {'want':>6} {'fade':>7} {'spread':>7} "
          f"{'per pair':>22}")
    rows = []
    for slot, cx, cy, half in rs:
        x0, y0 = cx - CROP[0] - half, cy - CROP[1] - half
        if x0 < 0 or y0 < 0 or x0 + 2 * half > first.shape[1] or y0 + 2 * half > first.shape[0]:
            print(f"{slot:>4} outside the crop")
            continue
        sl = (slice(y0, y0 + 2 * half), slice(x0, x0 + 2 * half))
        icon = cell_of(atlas, slot, 2 * half)
        ip, ia = icon[..., :3], icon[..., 3:]
        fs = []
        for p in pairs:
            on, off = load(f"fade_on{p}.png"), load(f"fade_off{p}.png")
            bg = off[sl][..., :3]
            denom = (ip - ia * bg).sum(axis=(0, 1))
            num = (on[sl][..., :3] - bg).sum(axis=(0, 1))
            fs.append(float(np.mean(num / denom)))
        t = (cy - ylo) / (yhi - ylo) if yhi > ylo else 1.0
        f, sp = float(np.mean(fs)), float(np.max(fs) - np.min(fs))
        rows.append((t, f, sp, slot))
        print(f"{slot:>4} {2 * half:>5} {t:8.3f} {0.25 + 0.75 * t:6.2f} {f:7.3f} {sp:7.3f} "
              f"{' '.join(f'{v:6.2f}' for v in fs):>22}")
    # Only the boxes that repeat AND land in the range opacity can actually be in: a negative or a
    # >1.3 reading is not a faint icon, it is the denominator going small, which happens on icons
    # whose artwork is mostly transparent (an outline glyph) or whose background moved during the
    # toggle. Those boxes are named in the table above and left out of the claim.
    keep = [r for r in rows if r[2] < 0.15 and 0.0 <= r[1] <= 1.25]
    print(f"\n{len(keep)} of {len(rows)} boxes repeat within 0.15 across pairs")
    if len(keep) >= 3:
        tt = np.array([r[0] for r in keep])
        ff = np.array([r[1] for r in keep])
        a, b = np.polyfit(tt, ff, 1)
        order = np.argsort(tt)
        mono = all(ff[order][i] <= ff[order][i + 1] + 0.06 for i in range(len(ff) - 1))
        resid = np.abs(ff - (b + a * tt)).max()
        print(f"fit fade = {b:.3f} + {a:.3f} * t   (want 0.25 + 0.75 * t)  max residual {resid:.3f}"
              f"  monotone in depth: {mono}")
        for lab, msk in (("back ", tt < 0.15), ("side ", (tt > 0.4) & (tt < 0.6)),
                         ("front", tt > 0.9)):
            if msk.any():
                print(f"  {lab}: {ff[msk].mean():.3f}  (want {0.25 + 0.75 * tt[msk].mean():.3f})")


main()
