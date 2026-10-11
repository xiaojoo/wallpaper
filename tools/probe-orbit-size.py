#!/usr/bin/env python3
"""tools/probe-orbit-size.py - the ring's two size knobs, read off the geometry the engine logged.

The engine writes one line a second while the ring is clickable: `orbit rects hover=N slot:cx,cy+half`
for every slot it drew. From that list the ellipse's two radii are just the spans, and the icons' size
is the half width - so this measures what the desktop is doing, not what the setting claims.

Run with the ring frozen (--ctl orbitspeed 0), or the numbers move between the two reads.
"""
import glob, os, re, sys

BIN = r"H:\wallpaper\bld\bin\RelWithDebInfo"


def last_rects():
    logs = sorted(glob.glob(os.path.join(BIN, "logs", "renderer-*.log")), key=os.path.getmtime)
    if not logs:
        sys.exit("no renderer log")
    lines = [l for l in open(logs[-1], encoding="utf-8", errors="replace") if "orbit rects" in l]
    if not lines:
        sys.exit(f"no 'orbit rects' line in {logs[-1]}")
    return re.findall(r"(\d+):(\d+),(\d+)\+(\d+)", lines[-1].split("slot:", 1)[1])


def main():
    rs = [(int(a), int(b), int(c), int(d)) for a, b, c, d in last_rects()]
    xs = [r[1] for r in rs]
    ys = [r[2] for r in rs]
    halves = [r[3] for r in rs]
    # "front" is the one number the setting is: iconPx is what an icon measures at the front of the
    # ring, and every other slot is that scaled down by its depth, so the mean is not a size anyone
    # asked for.
    print(f"{len(rs)} slots  rx={(max(xs) - min(xs)) / 2:.0f}  ry={(max(ys) - min(ys)) / 2:.0f}  "
          f"icon front={max(halves) * 2}  (half {min(halves)}..{max(halves)})  "
          f"centre {sum(xs) / len(xs):.0f},{sum(ys) / len(ys):.0f}")


main()
