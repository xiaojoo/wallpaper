#!/usr/bin/env python3
"""tools/make-icon.py - draw the app icon candidates and print them at the sizes that matter.

The one that decides an icon is 16 px in the notification area, not 1024 px in a mockup, so every
candidate is rendered twice: big enough to judge the drawing, and at 16/32 px upscaled with no
smoothing so the collapse is visible before anything is wired into the exe.

Colours are the app's own theme (Wallpaper/qml/Main.qml: accent #3B82F6, accentDim #2A5FB8,
surface #151C27, surfaceAlt #1C2532) so the icon and the window are the same brand.

Usage:  python tools/make-icon.py            # writes build/icons/*.png
        python tools/make-icon.py --ico 2     # also writes resources/Wallpaper.ico from candidate 2
"""
import argparse
import os

from PIL import Image, ImageDraw

SS = 8                  # supersample factor: draw big, shrink with LANCZOS, so edges stay honest
BASE = 256              # the design canvas in logical units; real pixels = BASE * SS
ACCENT = (59, 130, 246)
ACCENT_DIM = (42, 95, 184)
SURFACE = (21, 28, 39)
SURFACE_ALT = (28, 37, 50)
DEEP = (16, 21, 30)
LIGHT = (232, 237, 245)
WHITE = (245, 247, 250)


def canvas():
    img = Image.new("RGBA", (BASE * SS, BASE * SS), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)


def shrink(img, size):
    return img.resize((size, size), Image.LANCZOS)


def plate(d, fill, radius, inset=0):
    r = radius * SS
    d.rounded_rectangle([inset * SS, inset * SS, (BASE - inset) * SS - 1, (BASE - inset) * SS - 1],
                        radius=r, fill=fill)


def vgrad(img, d, box, top, bottom):
    """Vertical gradient inside a rounded box, clipped by that box."""
    x0, y0, x1, y1 = [v * SS for v in box]
    h = int(y1 - y0)
    strip = Image.new("RGBA", (int(x1 - x0), h))
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
        ImageDraw.Draw(strip).line([(0, y), (strip.width, y)], fill=c)
    mask = Image.new("L", strip.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, strip.width - 1, strip.height - 1],
                                           radius=int(28 * SS), fill=255)
    img.paste(strip, (int(x0), int(y0)), mask)


# ---------------------------------------------------------------- candidates
def cand_frame():
    """1 - a screen with a picture in it: the literal read, nothing clever."""
    img, d = canvas()
    plate(d, SURFACE, 56)
    vgrad(img, d, (34, 34, 222, 222), (27, 42, 68), ACCENT_DIM)
    # sun
    d.ellipse([140 * SS, 62 * SS, 190 * SS, 112 * SS], fill=WHITE)
    # two ridges, the far one lighter
    ridge = lambda pts: [(x * SS, y * SS) for x, y in pts]
    d.polygon(ridge([(34, 222), (120, 118), (176, 176), (222, 132), (222, 222)]), fill=LIGHT)
    d.polygon(ridge([(34, 222), (96, 156), (150, 222)]), fill=ACCENT)
    return shrink(img, BASE * 2)


def cand_peel():
    """2 - a sheet of wallpaper: the frame plus a lifted corner, so it is "paper on a screen"."""
    img, d = canvas()
    plate(d, SURFACE, 56)
    vgrad(img, d, (30, 30, 226, 226), (27, 42, 68), ACCENT_DIM)
    ridge = lambda pts: [(x * SS, y * SS) for x, y in pts]
    d.polygon(ridge([(30, 226), (112, 126), (168, 182), (226, 120), (226, 226)]), fill=LIGHT)
    d.ellipse([146 * SS, 58 * SS, 194 * SS, 106 * SS], fill=WHITE)
    # the lifted corner: a folded triangle in the accent colour over the deep plate
    fold = Image.new("RGBA", img.size, (0, 0, 0, 0))
    fd = ImageDraw.Draw(fold)
    fd.polygon(ridge([(154, 226), (226, 226), (226, 154)]), fill=ACCENT + (255,))
    fd.polygon(ridge([(154, 226), (226, 154), (196, 196)]), fill=ACCENT_DIM + (255,))
    img = Image.alpha_composite(img, fold)
    return shrink(img, BASE * 2)


def cand_w():
    """3 - a monogram: a folded "W" for Wallpaper, the boldest thing at 16 px."""
    img, d = canvas()
    plate(d, ACCENT, 56)
    # Down-up-down-up from the top edge: a W, not an M. The vertex heights are what make it read.
    w = [(44, 74), (88, 188), (128, 116), (168, 188), (212, 74)]
    d.line([(a * SS, b * SS) for a, b in w], fill=DEEP, width=int(30 * SS), joint="curve")
    return shrink(img, BASE * 2)


def contact(cands, out_dir):
    """One sheet: each candidate at 256, 64, 32 and 16 px, the small ones upscaled 4x with no
    smoothing so a design that dies small is visible while it is still cheap to throw away."""
    tile = 300
    sheet = Image.new("RGB", (tile * len(cands), 340), (12, 15, 20))
    for i, im in enumerate(cands):
        x = i * tile
        sheet.paste(im.convert("RGB").resize((256, 256), Image.LANCZOS), (x + 12, 12))
        for j, s in enumerate((64, 32, 16)):
            small = im.convert("RGB").resize((s, s), Image.LANCZOS)
            big = small.resize((s * 4, s * 4), Image.NEAREST)
            sheet.paste(big, (x + 12 + j * 96, 284))
    sheet.save(os.path.join(out_dir, "candidates.png"))


def write_ico(cand, path):
    sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)
    # One base image at the largest size: PIL's ICO writer resizes the base down to each entry in
    # `sizes` and silently drops any entry larger than the base, so handing it a 16 px image used to
    # produce a 728-byte file holding one frame.
    base = cand.convert("RGBA").resize((256, 256), Image.LANCZOS)
    base.save(path, format="ICO", sizes=[(s, s) for s in sizes])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ico", type=int, default=0, help="candidate number to write as resources/Wallpaper.ico")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "build", "icons")
    os.makedirs(out, exist_ok=True)
    cands = [cand_frame(), cand_peel(), cand_w()]
    for n, im in enumerate(cands, 1):
        im.convert("RGBA").save(os.path.join(out, "cand%d.png" % n))
    contact(cands, out)
    print("wrote %s/candidates.png and cand1..%d.png" % (out, len(cands)))
    if a.ico:
        ico = os.path.join(root, "resources", "Wallpaper.ico")
        os.makedirs(os.path.dirname(ico), exist_ok=True)
        write_ico(cands[a.ico - 1], ico)
        print("wrote %s from candidate %d" % (ico, a.ico))


main()
