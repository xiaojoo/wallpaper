#!/usr/bin/env python3
"""tools/probe-ring-seam.py - does the ring leave a hole of background where two icons overlap?

usage: python tools/probe-ring-seam.py <ring-on.png> <ring-off.png>

The ring draws one icon per pixel and antialiases the outermost pixel of its box. Antialiasing means
blending with what is underneath - and underneath is the wallpaper, because the pass composites the
icons by picking one, not by stacking them. So wherever an icon's own artwork reached the box edge,
that one pixel showed the desktop through it: a hairline of background between two overlapping icons.
The atlas now keeps a transparent gutter inside every cell so the box edge falls in the gutter.

A hole is counted per box: an edge pixel that matches the ring-off frame at that pixel while the pixel
six steps inward is clearly not the ring-off frame. The second half is what keeps a box edge over
empty desktop - where showing the wallpaper through is correct - out of the count.
"""
import glob, os, re, sys
import numpy as np
from PIL import Image

BIN = r"H:\wallpaper\bld\bin\RelWithDebInfo"
CROP = (1500, 440)


def rects(want):
    """The geometry the capture was taken with. The ring is frozen during a pair, so the log line from
    that run is the same layout - but a later run has already moved on, so the log has to be named."""
    logs = sorted(glob.glob(os.path.join(BIN, "logs", "renderer-*.log")), key=os.path.getmtime,
                  reverse=True)
    if want:
        logs = [f for f in logs if want in os.path.basename(f)] or sys.exit(f"no log matching {want}")
    for f in logs:
        lines = [l for l in open(f, encoding="utf-8", errors="replace") if "orbit rects" in l]
        if lines:
            return os.path.basename(f), [(int(a), int(b), int(c), int(d)) for a, b, c, d in
                                         re.findall(r"(\d+):(\d+),(\d+)\+(\d+)",
                                                    lines[-1].split("slot:", 1)[1])]
    sys.exit("no 'orbit rects' line in the chosen log")


def main():
    want = None
    if "--log" in sys.argv:
        want = sys.argv[sys.argv.index("--log") + 1]
        sys.argv = [a for a in sys.argv if a not in ("--log", want)]
    logname, rs = rects(want)
    on = np.asarray(Image.open(sys.argv[1]).convert("RGB"), dtype=np.float64)
    off = np.asarray(Image.open(sys.argv[2]).convert("RGB"), dtype=np.float64)
    print(f"{os.path.basename(sys.argv[1])} vs {os.path.basename(sys.argv[2])}  "
          f"{len(rs)} boxes from {logname}")
    tot = edge = 0
    for slot, cx, cy, half in rs:
        x, y = cx - CROP[0], cy - CROP[1]
        r = half + 8
        if x - r < 0 or y - r < 0 or x + r > on.shape[1] or y + r > on.shape[0]:
            print(f"  slot {slot:>2} outside the crop")
            continue
        A = on[y - r:y + r + 1, x - r:x + r + 1]
        B = off[y - r:y + r + 1, x - r:x + r + 1]
        yy, xx = np.mgrid[-r:r + 1, -r:r + 1]
        ad = np.maximum(np.abs(xx), np.abs(yy))
        horiz = np.abs(xx) >= np.abs(yy)
        same = (np.abs(A - B).max(axis=2) <= 8.0)             # the wallpaper, untouched
        drawn = (np.abs(A - B).max(axis=2) > 40.0)            # an icon, painted here
        # A hole is a band of untouched wallpaper with paint on BOTH sides of it along the box normal:
        # the icon this box belongs to on the inside, another icon (or another part of the ring) on the
        # outside. Without that second half the count is just "how much of the box edge is empty",
        # which the transparent gutter now deliberately answers with yes.
        ix = np.where(horiz, xx - np.sign(xx) * 2, xx)
        iy = np.where(horiz, yy, yy - np.sign(yy) * 2)
        ox = np.where(horiz, xx + np.sign(xx) * 2, xx)
        oy = np.where(horiz, yy, yy + np.sign(yy) * 2)
        m = ad == half
        edge += int(m.sum())
        holes = int((same[m] & drawn[iy[m] + r, ix[m] + r] & drawn[oy[m] + r, ox[m] + r]).sum())
        tot += holes
        print(f"  slot {slot:>2} half={half:>3}  edge px {int(m.sum()):>4}  holes {holes:>4}"
              f"  ({100.0 * holes / max(1, m.sum()):4.1f}%)")
    print(f"hole pixels: {tot} of {edge} box-edge pixels "
          f"({100.0 * tot / max(1, edge):.2f}%)")


main()
