"""Measure the vertical pitch of a light menu's text rows in a 1:1 screen capture.

Rows are found as bands of dark ink inside a given x range; the pitch is the distance between
consecutive band centres, which is what "the gap between items" actually is on screen.
"""
import sys
from PIL import Image

path, x0, x1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
y0, y1 = (int(sys.argv[4]), int(sys.argv[5])) if len(sys.argv) > 5 else (0, 10 ** 6)

im = Image.open(path).convert("L")
px = im.load()
rows = []
for y in range(max(0, y0), min(im.height, y1)):
    dark = sum(1 for x in range(x0, x1) if px[x, y] < 120)
    rows.append(dark)

bands, start = [], None
for i, d in enumerate(rows):
    if d >= 3 and start is None:
        start = i
    elif d < 3 and start is not None:
        bands.append((start, i - 1))
        start = i
if start is not None:
    bands.append((start, len(rows) - 1))

centres = [y0 + (a + b) / 2 for a, b in bands if b - a >= 4]
print("bands=%d" % len(centres))
for i, c in enumerate(centres):
    prev = "" if i == 0 else " pitch=%.1f" % (c - centres[i - 1])
    print("  row %d centre=%.1f  ink %d..%d%s" % (i + 1, c, y0 + bands[i][0], y0 + bands[i][1], prev))
