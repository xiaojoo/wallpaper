"""Measure where the ring actually puts its icons, from the geometry the engine itself logs.

`orbit rects` is the engine's own once-a-second dump of the click rectangles it built, and those come
out of the same arithmetic as the shader's icon boxes (Application.cpp says why: two independent
trigonometries drift apart). So this is not a screenshot of the ring - it is the ring's own numbers.

It writes settings through the pipe, so it records what was there and puts it back. It arms the drag
layer only to read that window's rectangle, which gives an independent centre and the bounding box;
arming writes nothing, only a drag commit does.
"""
import ctypes
import ctypes.wintypes
import glob
import json
import os
import re
import subprocess
import sys
import time

CTL = r"H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe"
LOGDIR = r"H:\wallpaper\bld\bin\RelWithDebInfo\logs"
DRAG_TITLE = "SmartWallpaper shortcut ring - drag to move"
RING_STEP = 0.42          # kOrbitRingStep: each further ring is this much wider than the one inside
FUNNEL_NECK = 0.30        # kFunnelNeck: the reference measures 212 px of mouth against 64 px of neck

user32 = ctypes.windll.user32
# Without this every rectangle below is virtualized for a DPI-unaware process and off by the scale
# factor of the monitor.
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))


def status():
    r = subprocess.run([CTL, "--ctl", "status"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return json.loads(r.stdout)


def ctl(*args):
    subprocess.run([CTL, "--ctl", *args], capture_output=True, text=True,
                   encoding="utf-8", errors="replace")


def newest_rects(after):
    """The last `orbit rects` line written after `after`: {slot: (cx, cy, half)} in screen pixels."""
    path = max(glob.glob(os.path.join(LOGDIR, "renderer-*.log")), key=os.path.getmtime)
    best = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "orbit rects" not in line:
                continue
            stamp = time.mktime(time.strptime(line[:19], "%Y-%m-%d %H:%M:%S")) + int(line[20:23]) / 1000.0
            if stamp < after:
                continue
            best = line
    if best is None:
        return None
    out = {}
    for m in re.finditer(r"(\d+):(-?\d+),(-?\d+)\+(-?\d+)", best):
        out[int(m.group(1))] = tuple(int(m.group(i)) for i in (2, 3, 4))
    return out


def drag_rect():
    hwnd = user32.FindWindowW(None, DRAG_TITLE)
    if not hwnd:
        return None
    r = ctypes.wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def work_h():
    """The primary monitor's work area, the unit the ring's fractions are taken of. Monitor 0 is the
    primary one here (status says px=0,0,3840x2160), so SPI_GETWORKAREA is the same rectangle."""
    rc = ctypes.wintypes.RECT()
    if not user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(rc), 0):   # SPI_GETWORKAREA
        return 0, "unreadable"
    return rc.bottom - rc.top, "%d,%d,%d,%d" % (rc.left, rc.top, rc.right, rc.bottom)


s0 = status()
was = dict(orbit=s0["orbit"], per_ring=s0["orbit_per_ring"], rings=s0["orbit_rings"],
           shape=s0["orbit_shape"])
wh, wr = work_h()
print("found: orbit=%(orbit)s per_ring=%(per_ring)s rings=%(rings)s shape=%(shape)s" % was)
print("work area %s -> h=%d   orbit_scale=%s%% icon_scale=%s%% shown=%s of pickable=%s"
      % (wr, wh, s0["orbit_scale"], s0["orbit_icon_scale"], s0["orbit_shown"], s0["orbit_pickable"]))
print("formula says rx0=%.1f ry0=%.1f pad=%.1f (0.30/0.16/0.085 of work h, x the two percentages)"
      % (0.30 * wh * s0["orbit_scale"] / 100, 0.16 * wh * s0["orbit_scale"] / 100,
         0.5 * 0.085 * wh * s0["orbit_icon_scale"] / 100))

if not s0["orbit"]:
    ctl("orbit", "on")
    time.sleep(1.5)

cases = [
    ("ellipse r1 x10", 10, 1, "ellipse"),
    ("ellipse r2 x10", 10, 2, "ellipse"),
    ("star    r1 x10", 10, 1, "star"),
    ("funnel  r1 x12", 12, 1, "funnel"),
    ("funnel  r1 x20", 20, 1, "funnel"),
]

measured = {}
for name, per_ring, rings, shape in cases:
    ctl("orbitrings", str(rings))
    time.sleep(1.2)
    ctl("orbitshape", shape)
    time.sleep(1.2)
    ctl("orbitcount", str(per_ring))
    want = min(per_ring * rings, s0["orbit_pickable"], 64)
    deadline = time.time() + 12
    s = status()
    while time.time() < deadline:
        s = status()
        if (s["orbit_per_ring"] == per_ring and s["orbit_rings"] == rings
                and s["orbit_shape"] == shape and s["orbit_shown"] == want):
            break
        time.sleep(0.4)
    settle = time.time()
    ctl("orbitdrag", "on")                 # also holds the phase still, so the dump is one layout
    time.sleep(0.8)
    rect = drag_rect()
    time.sleep(1.8)
    pts = newest_rects(settle)
    ctl("orbitdrag", "off")
    print("\n== %-14s per_ring=%d rings=%d shape=%s shown=%d (expected %d)%s%s"
          % (name, s["orbit_per_ring"], s["orbit_rings"], s["orbit_shape"], s["orbit_shown"], want,
             "" if rect else "  NO DRAG WINDOW", "" if pts else "  NO RECTS LINE"))
    if not rect or not pts:
        continue
    cx, cy = (rect[0] + rect[2]) / 2.0, (rect[1] + rect[3]) / 2.0
    measured[name] = dict(cx=cx, cy=cy, hw=(rect[2] - rect[0]) / 2.0, hh=(rect[3] - rect[1]) / 2.0,
                          pts=pts, per=s["orbit_per_ring"], rings=s["orbit_rings"])

# rx0/ry0 and the icon padding, solved from two bounding boxes rather than assumed: the 1-ring box is
# rx0+pad and the 2-ring box is 1.42*rx0+pad, and the two together say what each half is.
a, b = measured.get("ellipse r1 x10"), measured.get("ellipse r2 x10")
if not a or not b:
    print("\nneed the 1-ring and 2-ring ellipse cases to normalise")
    sys.exit(1)
rx0 = (b["hw"] - a["hw"]) / RING_STEP
ry0 = (b["hh"] - a["hh"]) / RING_STEP
pad = a["hw"] - rx0
print("\nbbox solve: rx0=%.1f ry0=%.1f pad=%.1f   (bbox %.0f/%.0f -> %.0f/%.0f for 1->2 rings)"
      % (rx0, ry0, pad, a["hw"], a["hh"], b["hw"], b["hh"]))
print("outer ring of a 4-ring set reaches rx=%.0f px from the centre" % (rx0 * (1 + 3 * RING_STEP)))

print("\n%-14s %-5s %-7s %s" % ("case", "ring", "radius", "per-slot radius (1.000 = the ellipse's own edge)"))
for name, m in measured.items():
    groups = {}
    for k in sorted(m["pts"]):
        x, y, _ = m["pts"][k]
        dx, dy = (x - m["cx"]) / rx0, (y - m["cy"]) / ry0
        groups.setdefault(k // m["per"], []).append((dx * dx + dy * dy) ** 0.5)
    for g in sorted(groups):
        v = groups[g]
        print("%-14s %-5d %-7.3f %s" % (name, g, sum(v) / len(v), " ".join("%.3f" % t for t in v)))

# The funnel is the one shape whose point is vertical separation, so it gets its own reading: are the
# layers evenly stepped down, does the figure narrow as it goes, and does all of it still fit on the
# screen. None of this re-implements the formula - it is the measured ys and xs, compared with each
# other and with one icon's own height.
print("\nfunnel, per slot in ring order (top layer first):")
for name, m in measured.items():
    if "funnel" not in name:
        continue
    per, pts = m["per"], m["pts"]
    ys = [(k, pts[k][1] - m["cy"]) for k in sorted(pts)]
    gaps = [round((ys[i + 1][1] - ys[i][1]), 1) for i in range(len(ys) - 1)]
    lo, hi = min(gaps), max(gaps)
    head = [abs(pts[k][0] - m["cx"]) / rx0 for k in sorted(pts)[:3]]
    tail = [abs(pts[k][0] - m["cx"]) / rx0 for k in sorted(pts)[-3:]]
    print("  %-14s y: %s" % (name, " ".join("%+d" % y for _, y in ys)))
    print("  %-14s step between layers %.1f..%.1f px, one icon is %.0f px => %.2f..%.2f icon heights"
          % ("", lo, hi, pad * 2, lo / (pad * 2), hi / (pad * 2)))
    print("  %-14s mouth |dx|/rx0 = %.2f (first 3 slots), neck = %.2f (last 3), taper %.1f:1"
          % ("", max(head), max(tail), max(head) / max(max(tail), 0.01)))
    # The taper on its own, without knowing the phase: slot j and slot j+n/2 are half a turn apart, so
    # their cos(angle) has the same magnitude and cancels out of |dx_j| / |dx_j+n/2|. What is left is
    # the ratio of their radii, which is the cone's narrowing and nothing else.
    half = len(ys) // 2
    pairs = []
    for j in range(min(half, 4)):
        k, l = ys[j][0], ys[j + half][0]
        dxj, dxl = abs(pts[k][0] - m["cx"]), abs(pts[l][0] - m["cx"])
        if dxl < 8:                       # a slot sitting on the vertical axis: |dx| is rounding there
            continue
        slope = 1.0 - FUNNEL_NECK
        t = j / float(len(ys))
        want = (1.0 - slope * t) / (1.0 - slope * (t + 0.5))
        pairs.append("j%d/%d %.2f(%.2f)" % (j, j + half, dxj / dxl, want))
    print("  %-14s radius ratio of opposite slots, measured(predicted): %s" % ("", " ".join(pairs)))
    print("  %-14s needs %.0f px of height (bbox half %.0f), work area is %d px"
          % ("", 2 * m["hh"], m["hh"], wh))

print("\nrestoring")
ctl("orbitshape", was["shape"])
time.sleep(1.0)
ctl("orbitrings", str(was["rings"]))
time.sleep(1.0)
ctl("orbitcount", str(was["per_ring"]))
time.sleep(1.0)
if not was["orbit"]:
    ctl("orbit", "off")
time.sleep(1.5)
s = status()
print("after restore: rings=%s shape=%s per_ring=%s orbit=%s  (was %s/%s/%s/%s)"
      % (s["orbit_rings"], s["orbit_shape"], s["orbit_per_ring"], s["orbit"],
         was["rings"], was["shape"], was["per_ring"], was["orbit"]))
