#!/usr/bin/env python3
"""tools/make-icon.py - build resources/Wallpaper.ico, and print it at the sizes that matter.

The one that decides an icon is 16 px in the notification area, not 1024 px in a mockup, so every
candidate is rendered twice: big enough to judge the drawing, and at 16/32 px upscaled with no
smoothing so the collapse is visible before anything is wired into the exe.

Two sources feed the .ico:
  * hand-drawn candidates (below) - colours are the app's own theme (Wallpaper/qml/Main.qml:
    accent #3B82F6, accentDim #2A5FB8, surface #151C27, surfaceAlt #1C2532) so the icon and the
    window are the same brand;
  * `--art` - a raster someone drew for us: the plate is cut out of its white field and becomes the
    alpha, then resources/Wallpaper.png (the 1024 master) and the .ico are both written from it.
    The .ico is generated from that master, never edited in place.

Usage:  python tools/make-icon.py                    # writes build/icons/*.png (candidates)
        python tools/make-icon.py --ico 2            # also writes the .ico from candidate 2
        python tools/make-icon.py --art path/to.png  # cut the plate out of art, refresh master+ico
        python tools/make-icon.py --ico-from         # rebuild the .ico from resources/Wallpaper.png
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


def plate_from_art(path, size=1024):
    """Cut the icon plate out of art somebody else drew: a rounded tile on a flat white field, as a
    rule with a generator watermark parked in a corner of that field.

    The plate is the largest connected piece of non-white, so a corner watermark - which is its own
    piece - is dropped rather than forcing a crop into the tile. The plate's own outline becomes the
    alpha, which keeps the squircle that was actually drawn (its corner runs out at 19% of the
    width; a radius guessed at 22% would have sheared the rim). Supplied art is never quite square:
    this sheet measured 1259 x 1291, and the 2.5% gets squashed rather than padded, because padding
    shrinks the tile inside the frame every consumer sizes to.
    """
    import numpy as np                # only this path needs numpy/scipy; the candidates are PIL
    from scipy import ndimage
    src = Image.open(path).convert("RGB")
    a = np.asarray(src)
    lab, _ = ndimage.label(a.min(axis=2) >= 248)          # the white field, as connected regions
    border = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))) - {0}
    obj, n = ndimage.label(~np.isin(lab, list(border)))    # everything the field does not reach
    if n == 0:
        raise SystemExit("no plate found in %s - the whole image is background" % path)
    areas = ndimage.sum_labels(np.ones_like(obj), obj, index=range(1, n + 1))
    plate_mask = obj == (int(np.argmax(areas)) + 1)
    plate = ndimage.binary_fill_holes(plate_mask)
    ys, xs = np.nonzero(plate)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    alpha = Image.fromarray((plate[box[1]:box[3], box[0]:box[2]] * 255).astype(np.uint8))
    out = Image.new("RGBA", (size, size))
    out.paste(src.crop(box).resize((size, size), Image.LANCZOS), (0, 0),
              alpha.resize((size, size), Image.LANCZOS))
    # The largest thing left behind, so a detached element that should have been part of the mark
    # cannot go missing quietly: the watermark here is 3.8k px, a fifth of the plate would not be.
    rest = np.delete(areas, int(np.argmax(areas)))
    print("plate %dx%d from %s: corner runs out at %d%% of the height, %d other piece(s), biggest "
          "left behind %d px (plate is %d px)"
          % (box[2] - box[0], box[3] - box[1], os.path.basename(path),
             100 * (int(np.argmax(plate_mask[:, box[0]])) - box[1]) // (box[3] - box[1]),
             rest.size, int(rest.max()) if rest.size else 0, int(plate_mask.sum())))
    return out


def contact(cands, out_dir, name="candidates.png"):
    """One sheet: each candidate at 256, 64, 32 and 16 px, the small ones upscaled 4x with no
    smoothing so a design that dies small is visible while it is still cheap to throw away.
    Composited over the taskbar's own dark, not pasted flat: an icon's transparent corners have to
    read as transparent for the 16 px judgement to be the real one."""
    tile = 300
    sheet = Image.new("RGB", (tile * len(cands), 340), (12, 15, 20))
    for i, im in enumerate(cands):
        cell = Image.new("RGBA", (256, 256), SURFACE + (255,))
        cell.alpha_composite(im.convert("RGBA").resize((256, 256), Image.LANCZOS))
        sheet.paste(cell.convert("RGB"), (i * tile + 12, 12))
        for j, s in enumerate((64, 32, 16)):
            small = im.convert("RGBA").resize((s, s), Image.LANCZOS)
            big = small.resize((s * 4, s * 4), Image.NEAREST)
            cell = Image.new("RGBA", (big.size[0] + 8, big.size[1] + 8), SURFACE + (255,))
            cell.alpha_composite(big, (4, 4))
            sheet.paste(cell.convert("RGB"), (i * tile + 12 + j * 96, 284))
    sheet.save(os.path.join(out_dir, name))


def write_ico(cand, path):
    sizes = (16, 20, 24, 32, 40, 48, 64, 128, 256)
    # PIL's ICO writer resizes the base down to each entry in `sizes` and silently drops any entry
    # larger than the base, so handing it a 16 px image used to produce a 728-byte file holding one
    # frame. A base bigger than the largest frame is kept whole: one resample per frame, not a
    # chain of them.
    base = cand.convert("RGBA")
    if base.size[0] < max(sizes):
        base = base.resize((256, 256), Image.LANCZOS)
    base.save(path, format="ICO", sizes=[(s, s) for s in sizes])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ico", type=int, default=0, help="candidate number to write as resources/Wallpaper.ico")
    ap.add_argument("--art", default="", help="cut the plate out of this raster, refresh master png + ico")
    ap.add_argument("--ico-from", action="store_true", help="rewrite the ico from resources/Wallpaper.png")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "build", "icons")
    os.makedirs(out, exist_ok=True)
    master = os.path.join(root, "resources", "Wallpaper.png")
    ico = os.path.join(root, "resources", "Wallpaper.ico")
    if a.art:
        plate = plate_from_art(a.art)
        plate.save(master)
        write_ico(plate, ico)
        contact([plate], out, "from-art.png")
        print("wrote %s, %s and %s/from-art.png" % (master, ico, out))
        return
    if a.ico_from:
        write_ico(Image.open(master), ico)
        print("wrote %s from %s" % (ico, master))
        return
    cands = [cand_frame(), cand_peel(), cand_w()]
    for n, im in enumerate(cands, 1):
        im.convert("RGBA").save(os.path.join(out, "cand%d.png" % n))
    contact(cands, out)
    print("wrote %s/candidates.png and cand1..%d.png" % (out, len(cands)))
    if a.ico:
        write_ico(cands[a.ico - 1], ico)
        print("wrote %s from candidate %d" % (ico, a.ico))


main()
