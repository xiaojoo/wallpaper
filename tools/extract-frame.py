"""Turn the reference frame's outline into an HLSL vertex list.

The frame has shoulders, bites and long shallow chamfers that no combination of "box with four cut
corners" expresses, so it is extracted as a polygon instead: fill the outline component to get the
silhouette, trace its boundary, then simplify with perpendicular distance until the vertex count is
small enough to compile into a shader and the shape still matches.

Run:  python tools/extract-frame.py
"""
import numpy as np
from PIL import Image
from scipy import ndimage

P = r'C:\Users\XIAOJOO\.qoder\tmp\H--wallpaper\images\d5070e74-f6ab-4104-9e38-62eb29e9a90f\67489ab7-4f10-46cb-a661-4fb2ce33000d.png'

a = np.asarray(Image.open(P).convert('RGB')).astype(np.int16)
H, W = a.shape[:2]
R, G, B = a[..., 0], a[..., 1], a[..., 2]
cyan = (B > 90) & (B - R > 40)
red = (R > 110) & (R - G > 50) & (R - B > 50)

lab, cnt = ndimage.label(cyan, structure=np.ones((3, 3), int))
sizes = ndimage.sum(cyan, lab, range(1, cnt + 1))
order = np.argsort(-sizes)
outer = (lab == int(order[0]) + 1)
solid = np.isin(lab, [int(j) + 1 for j in order[1:] if sizes[j] > 150])

ys, xs = np.nonzero(cyan)
fx0, fx1, fy0, fy1 = xs.min(), xs.max(), ys.min(), ys.max()
FW, FH = fx1 - fx0 + 1, fy1 - fy0 + 1
print("frame bbox %d..%d x %d..%d   %dx%d  aspect %.3f" % (fx0, fx1, fy0, fy1, FW, FH, FW / FH))


def trace(mask):
    """The silhouette as a closed polyline, taken column by column.

    A Moore neighbourhood trace was the first attempt and it is the wrong tool here: the reference's
    outline is not a closed curve - the bites on the left and right edges and the detached tabs break
    it into open arcs, so the walker falls off the end. This shape is single-valued from above and
    from below, so top(x) and bottom(x) describe it exactly and there is nothing to get lost on.
    """
    xs2 = np.nonzero(mask.any(axis=0))[0]
    top, bot = [], []
    for x in xs2:
        col = np.nonzero(mask[:, x])[0]
        top.append((int(x), int(col.min())))
        bot.append((int(x), int(col.max())))
    return np.array(top + bot[::-1], float)


def dp(pts, eps):
    """Douglas-Peucker on an OPEN polyline; keeps both endpoints."""
    if len(pts) < 3:
        return list(range(len(pts)))
    def far(i, j):
        seg = pts[j] - pts[i]
        L = np.hypot(*seg)
        q = pts[i + 1:j] - pts[i]
        if not len(q):
            return -1, 0.0
        # numpy 2 dropped np.cross for 2-vectors, so the scalar form is written out.
        d = np.abs(seg[0] * q[:, 1] - seg[1] * q[:, 0])
        if L > 1e-9:
            d /= L
        else:
            d = np.hypot(q[:, 0], q[:, 1])
        return i + 1 + int(np.argmax(d)), float(d.max())
    keep = {0, len(pts) - 1}
    stack = [(0, len(pts) - 1)]
    while stack:
        i, j = stack.pop()
        k, d = far(i, j)
        if d > eps and 0 < k < j:
            keep.add(k)
            stack += [(i, k), (k, j)]
    return sorted(keep)


def simplify_ring(pts, eps):
    """Simplify a closed ring.

    Feeding the ring straight to Douglas-Peucker is the trap: its first and last point are the same
    pixel, so the very first chord has zero length, every deviation measures against a degenerate
    line, and the whole ring collapses to two points. Splitting at the point farthest from the start
    turns it into two open chains, which is what the algorithm assumes.
    """
    n = len(pts)
    d0 = np.hypot(pts[:, 0] - pts[0, 0], pts[:, 1] - pts[0, 1])
    m = int(np.argmax(d0))
    a = pts[0:m + 1]
    b = np.vstack([pts[m:], pts[:1]])
    ia = dp(a, eps)
    ib = dp(b, eps)
    keep = set(ia)
    keep |= set([k + m for k in ib if k < n - m])
    keep.add(0)
    keep.add(m)
    return pts[sorted(k for k in keep if k < n)]


# The outline is broken where a tab sits in a bite, so close those gaps before filling: without this
# the fill leaks out through the bites and the silhouette loses the left and right edges.
closed = ndimage.binary_closing(cyan, structure=np.ones((9, 9), bool))
filled = ndimage.binary_fill_holes(closed)
filled = ndimage.binary_opening(filled, structure=np.ones((3, 3), bool))
c = trace(filled)
print("silhouette polyline: %d points, area %d px" % (len(c), int(filled.sum())))
for eps in (1.0, 1.5, 2.0, 2.5, 3.0, 4.0):
    s = simplify_ring(c, eps)
    print("  eps %.1f -> %d vertices" % (eps, len(s)))
simp = simplify_ring(c, 2.0)

print("\nouter silhouette vertices, normalised to the frame box (u = x across, v = y down):")
pts = []
for x, y in simp:
    u, v = (x - fx0) / FW, (y - fy0) / FH
    pts.append((u, v))
    print("   float2(%.4f, %.4f),   // px %d,%d" % (u, v, x, y))

print("\nsolid tabs (detached cyan pieces):")
lab2, cnt2 = ndimage.label(solid, structure=np.ones((3, 3), int))
for k in range(1, cnt2 + 1):
    ys2, xs2 = np.nonzero(lab2 == k)
    if len(ys2) < 120:
        continue
    print("   x %d..%d y %d..%d  %dx%d   u %.3f..%.3f v %.3f..%.3f" %
          (xs2.min(), xs2.max(), ys2.min(), ys2.max(),
           xs2.max() - xs2.min() + 1, ys2.max() - ys2.min() + 1,
           (xs2.min() - fx0) / FW, (xs2.max() + 1 - fx0) / FW,
           (ys2.min() - fy0) / FH, (ys2.max() + 1 - fy0) / FH))

ys2, xs2 = np.nonzero(red)
rx0, rx1, ry0, ry1 = xs2.min(), xs2.max(), ys2.min(), ys2.max()
print("\nthe red annotation well: %dx%d  u %.3f..%.3f v %.3f..%.3f  aspect %.2f" %
      (rx1 - rx0 + 1, ry1 - ry0 + 1, (rx0 - fx0) / FW, (rx1 + 1 - fx0) / FW,
       (ry0 - fy0) / FH, (ry1 + 1 - fy0) / FH, (rx1 - rx0 + 1) / (ry1 - ry0 + 1)))
# its corner radius: how far in along the top edge the ring's first ink column sits, per row
sub = red[ry0:ry1 + 1, rx0:rx1 + 1]
for r in range(0, 30):
    xs3 = np.nonzero(sub[r])[0]
    if len(xs3):
        print("   row %2d of the well: first ink at x=%d" % (r, xs3.min()))
        if xs3.min() == 0:
            print("   -> corner radius ~%d px = %.3f of the well's height" % (r, r / sub.shape[0]))
            break
