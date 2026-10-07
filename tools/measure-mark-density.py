"""Density ruler for wallpaper previews.

Marks are pixels that stand out from the *local* background (a large-radius blur), so a slow
nebula gradient does not count as content. Reports, per image:
  coverage  - fraction of pixels above the ink threshold
  marks     - connected components >= min_px
  spacing   - mean nearest-neighbour distance between mark centroids, in DESKTOP pixels
  runs_row  - how many separate ink runs one horizontal scanline crosses (mean over k rows)
  run_gap   - median centre-to-centre distance between those runs, in DESKTOP pixels

marks/spacing are only meaningful for content that is *separated* into blobs. On line-based
content (embers' traces) a rising threshold splits one line into many components, so the count is
non-monotonic in the threshold and the number means nothing - read runs_row / run_gap there.
A 10pt mono terminal on 3840x2160 is an 8x16 px glyph cell: runs_row ~ 480, run_gap ~ 8 px.
"""
import sys
import json
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree

DESKTOP_W = 3840.0
SCAN_ROWS = 60


def scanline(ink, scale):
    """Runs one horizontal line crosses, averaged over evenly spaced rows."""
    h, w = ink.shape
    rows = np.linspace(int(h * 0.05), int(h * 0.95), SCAN_ROWS).astype(int)
    counts, gaps = [], []
    for r in rows:
        line = ink[r]
        edges = np.flatnonzero(np.diff(line.astype(np.int8)) == 1)
        if edges.size == 0:
            continue
        counts.append(edges.size)
        if edges.size > 1:
            gaps.append(np.median(np.diff(edges)) * scale)
    if not counts:
        return 0.0, float("nan")
    return float(np.mean(counts)), float(np.median(gaps)) if gaps else float("nan")


def measure(path, delta=12.0, bg_sigma=None, min_px=2, desktop_w=DESKTOP_W):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    lum = im @ np.array([0.2126, 0.7152, 0.0722])
    scale = desktop_w / im.shape[1]
    # The background estimate has to be stated in DESKTOP pixels: it is there to swallow the nebula
    # gradient, whose real width does not care how many image pixels it lands on. So the radius in
    # image pixels shrinks as the image is a bigger fraction of the desktop - 25 px on the /3 hero
    # preview, 75 px on a 1:1 desktop crop. Too small a radius counts the gradient itself as content.
    if bg_sigma is None:
        bg_sigma = 75.0 / max(scale, 1e-6)
    bg = ndimage.gaussian_filter(lum, bg_sigma)
    ink = (lum - bg) > delta

    runs, gap = scanline(ink, scale)

    lab, n = ndimage.label(ink)
    if n == 0:
        return dict(coverage=0.0, marks=0, spacing=float("nan"), per_glyph=float("nan"),
                    runs_row=runs, run_gap=gap)
    sizes = np.bincount(lab.ravel())[1:]
    keep = np.flatnonzero(sizes >= min_px) + 1
    if keep.size == 0:
        return dict(coverage=0.0, marks=0, spacing=float("nan"), per_glyph=float("nan"),
                    runs_row=runs, run_gap=gap)
    cents = np.array(ndimage.center_of_mass(ink, lab, keep))   # (rows, cols)

    # nearest-neighbour distance between mark centroids, in desktop pixels
    tree = cKDTree(cents * scale)
    d, _ = tree.query(cents * scale, k=2)
    spacing = float(np.median(d[:, 1]))

    h, w = lum.shape
    px_area = (h * w) / max(keep.size, 1)
    return dict(coverage=float(ink.mean()) * 100.0, marks=int(keep.size),
                spacing=spacing, per_glyph=float(np.sqrt(px_area)) * scale,
                runs_row=runs, run_gap=gap)


if __name__ == "__main__":
    args = sys.argv[1:]
    dw = DESKTOP_W
    if args and args[0] == "--dw":
        dw = float(args[1])
        args = args[2:]
    paths = args
    out = []
    for p in paths:
        row = {"file": p.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]}
        for d in (8.0, 12.0, 20.0):
            m = measure(p.replace("\\", "/"), delta=d, desktop_w=dw)
            row[f"c{int(d)}"] = round(m["coverage"], 2)
            row[f"m{int(d)}"] = m["marks"]
            row[f"s{int(d)}"] = round(m["spacing"], 1) if m["marks"] else None
            row[f"g{int(d)}"] = round(m["per_glyph"], 1) if m["marks"] else None
            row[f"r{int(d)}"] = round(m["runs_row"], 1)
            row[f"q{int(d)}"] = None if np.isnan(m["run_gap"]) else round(m["run_gap"], 1)
        out.append(row)
    print(json.dumps(out, indent=1))
