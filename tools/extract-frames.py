"""Generate Shaders/HudFramesData.hlsl by measuring every reference frame on its sheet.

Why this exists: Shaders/HudFrames.hlsl used to draw ONE silhouette and a flag that toggled three
bars, so the settings window's border options were really two. The frames on the sheet are different
outlines, and the attempts at drawing them "from the shape of it" were sent back, so they are
measured instead.

What is measured, per frame:
  * the ring - the largest connected cyan component - as a closed polygon, taken from its four
    extreme profiles (top(x), bottom(x), left(y), right(y)), each Douglas-Peuckered. The profiles are
    measured from the ring ALONE: closing the outline first, as tools/extract-frame.py does, bridges
    every detached bar into it and moves the lines.
  * that outline snapped onto the grid the artwork is drawn on: every edge forced to 0, 45, 90 or 135
    degrees and every corner rebuilt as the intersection of its two fitted edges. The JPEG leaves
    edges 5 to 20 degrees off; drawing those back verbatim is the "不够规则".
  * how thick the drawn line is at each edge, probed inward off the sheet. The plain edges are snapped
    to the frame's own modal weight - the raw probe varies 2, 3, 2, 3.5 px along a line drawn at one
    width, and painting that back is the "边框线条不够均匀". The edges genuinely drawn heavier keep
    their own weight, to half a pixel.
  * every other component as its own polygon, filled if it covers more than 55% of its box and
    stroked if it is an outline: the card reference's top-left bar is hollow, 598 line pixels in a
    2106 pixel box.

  * the content box, where readable stuff goes: the card's is the red rectangle annotated on its own
    reference, and the sheet frames take the largest rectangle inside the ring that no decoration
    intrudes on.

A stroke is emitted as a PAIR of polygons - the outline, and an inner outline set in from it edge by
edge - and the shader draws what lies between them. One inner outline rather than a uniform band plus
heavy blobs painted over it, because two regions that overlap always leave a seam: their boundaries
are measured from the same JPEG two different ways, sit a fraction of a pixel apart, cross, and the
union scallops - the "叠加的部分不平整". Here nothing overlaps, so there is nothing to scallop, and a
change of width is a 45-degree cut in the inner outline, which is the taper the drawing shows.

Run:  python tools/extract-frames.py                # measure and verify
      python tools/extract-frames.py --png out.png  # model over sheet, per frame
      python tools/extract-frames.py --write        # also write Shaders/HudFramesData.hlsl
"""
import io
import sys
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.ndimage import distance_transform_edt
from matplotlib.path import Path

SHEET = r'E:\壁纸\4dae905d8022a26c9d3891d57623ab3c.jpg'
CARD = (r'C:\Users\XIAOJOO\.qoder\tmp\H--wallpaper\images'
        r'\d5070e74-f6ab-4104-9e38-62eb29e9a90f\67489ab7-4f10-46cb-a661-4fb2ce33000d.png')
OUT = r'H:\wallpaper\Shaders\HudFramesData.hlsl'
# The one number the C++ side needs from the same measurement - see the writer at the bottom.
META = r'H:\wallpaper\Engine\Packages\HudFramesMeta.hpp'

# The sheet's 2x3 grid, minus its bottom-left cell: that frame is the card's own reference, measured
# twice from two sources, and its ring comes out the same to 0.002 of the frame width - about one
# pixel - so it is dropped rather than shipped as a seventh style that copies the first. The bands
# must not overlap: a cell that reaches into the row below adopts that row's ring.
CELLS = [(0, 600, 0, 300), (600, 1200, 0, 300), (0, 600, 300, 560),
         (600, 1200, 300, 560), (600, 1200, 560, 857)]
NAMES = ['card', 'sheet1', 'sheet2', 'sheet3', 'sheet4', 'sheet6']

FILL, BAND_OUT, BAND_IN = 0, 1, 2          # polygon modes, mirrored in Shaders/HudFrames.hlsl

TAPER = {}
FITLOG = []    # (vertices in, runs, worst point-to-line deviation, worst vertex move, accepted)


def dp(pts, eps):
    p = np.asarray(pts, float)
    if len(p) < 3:
        return p
    ab = p[-1] - p[0]
    d = np.abs((p[:, 0] - p[0, 0]) * ab[1] - (p[:, 1] - p[0, 1]) * ab[0]) / max(np.linalg.norm(ab), 1e-9)
    i = int(np.argmax(d))
    if d[i] <= eps:
        return np.array([p[0], p[-1]])
    return np.vstack([dp(p[:i + 1], eps)[:-1], dp(p[i:], eps)])


def boundary_poly(mask, eps=1.4):
    """A component's outline as a closed polyline, from its four extreme profiles.

    The two vertical profiles are clipped to the y range the horizontal ones already cover. Without
    that the bottom chain starts at the component's rightmost column - high up, for a chamfered
    corner - and walks back over ground the right chain just covered, which puts a chord straight
    across the frame.
    """
    ys, xs = np.nonzero(mask)
    x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
    col = {x: np.nonzero(mask[:, x])[0] for x in range(x0, x1 + 1) if mask[:, x].any()}
    row = {y: np.nonzero(mask[y, :])[0] for y in range(y0, y1 + 1) if mask[y, :].any()}
    tL, bL = col[x0].min(), col[x0].max()
    tR, bR = col[x1].min(), col[x1].max()
    chains = [
        [(x, v.min()) for x, v in col.items()],
        [(row[y].max(), y) for y in range(tR, bR + 1) if y in row],
        [(x, v.max()) for x, v in sorted(col.items(), reverse=True)],
        [(row[y].min(), y) for y in range(bL, tL - 1, -1) if y in row],
    ]
    pts = []
    for ch in chains:
        pts += [(int(a), int(b)) for a, b in dp(ch, eps)]
    out = [pts[0]]
    for p in pts[1:]:
        if p != out[-1]:
            out.append(p)
    while len(out) > 1 and out[0] == out[-1]:
        out = out[:-1]
    return np.array(out, float)


def inward_normals(poly):
    """Unit inward normal per edge. The polygon is the drawn line's OUTER boundary, so the material
    lies inside it.

    The sign is easy to get backwards and the failure is silent - the probe walks outward, finds
    nothing, and every edge comes back one pixel wide. In y-down screen coordinates the shoelace sum
    is positive for a clockwise ring, and the interior of a clockwise ring lies to the RIGHT of each
    edge, which is (-dy, dx) - so positive area means sgn = -1.
    """
    n = len(poly)
    area = sum(poly[i][0] * poly[(i + 1) % n][1] - poly[(i + 1) % n][0] * poly[i][1] for i in range(n))
    sgn = -1.0 if area > 0 else 1.0
    out = []
    for i in range(n):
        d = poly[(i + 1) % n] - poly[i]
        L = np.hypot(d[0], d[1])
        if L < 1e-6:
            # A vertex repeated from the one before leaves an edge with no direction, so it has no
            # side the interior could be on. Standing in with the previous edge's normal is what keeps
            # a cut that lands on a corner from turning the offset built off it into a spike.
            out.append(out[-1] if out else (0.0, 0.0, 0.0))
            continue
        out.append((sgn * d[1] / L, -sgn * d[0] / L, float(L)))
    return out


def bilinear(g, x, y):
    ix, iy = int(np.floor(x)), int(np.floor(y))
    H, W = g.shape
    if not (0 <= ix < W - 1 and 0 <= iy < H - 1):
        return 0.0
    fx, fy = x - ix, y - iy
    return (g[iy, ix] * (1 - fx) * (1 - fy) + g[iy, ix + 1] * fx * (1 - fy)
            + g[iy + 1, ix] * (1 - fx) * fy + g[iy + 1, ix + 1] * fx * fy)


def probe_width(p, nx, ny, mf, edtm, reach=22, step=0.5):
    """How many pixels of line stand between a point on the outer boundary and the inside.

    Walked inward until the line stops covering the sample, and bounded by twice the deepest the walk
    got - the distance transform's ridge. Counting alone ran away at concave corners and where a solid
    band fuses into the outline, returning the reach as the width; the ridge bound without the walk cut
    sheet3's genuine 18 px band to 8.5 px. tools/tmp/calibrate-probe.py fits this to widths counted by
    hand on the reference: 2, 2 and 7 px for its top, left and heavy right edge.

    Two alternatives were measured and lost. An axis-aligned run count with a cosine correction,
    because the walk reads a 45-degree edge ~2.4x too thick - but the card's top-left chamfer really is
    5 px (a horizontal scan crosses it in 7 pixels at every row from 60 to 90), so that reading is not
    the bug it looks like, and the run count fit the drawing worse on both sides at once: ink left
    unpainted 18.4% -> 23.5%, paint laid where there is no ink 0.18% -> 8.60%.
    """
    x0, y0 = p[0] + 0.5 * nx, p[1] + 0.5 * ny
    ridge = 0.0
    c = 0.0
    while c < reach:
        ridge = max(ridge, bilinear(edtm, x0 + c * nx, y0 + c * ny))
        if bilinear(mf, x0 + c * nx, y0 + c * ny) < 0.5:
            break
        c += step
    return min(c + 0.5, 2.0 * ridge + 1.0, 20.0)



def edge_probe(poly, mask):
    """Probe every edge at pixel spacing: (point, width, edge index, position along the edge)."""
    nrm = inward_normals(poly)
    mf, edtm = mask.astype(float), distance_transform_edt(mask)
    recs = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        nx, ny, L = nrm[i]
        steps = max(int(np.ceil(L)), 1)
        for k in range(steps + 1):
            p = a + (b - a) * (k / steps)
            recs.append((p, max(probe_width(p, nx, ny, mf, edtm), 1.0), i, k / steps))
    return recs


def modal_width(recs):
    """The frame's own line weight: the lower quartile of what the probe came back with.

    The raw probe on a line the designer drew at one width comes back 2, 2.5, 3, 2, 3.5 px because it
    is reading a JPEG. Drawing that back verbatim is the "边框线条不够均匀" complaint - the variation
    belongs to the measurement, not to the artwork.

    The quartile rather than the mode, because a small hollow piece has proportionally many corner
    samples and they read deep: a 44 x 30 tick drawn with a 2 px line gave the mode 12.5 px, and a
    band that wide collapses its own inner outline into a knot. The heavy edges are not lost by
    taking the low end - they are drawn as their own solid blobs below.
    """
    ws = np.array([w for _, w, _, _ in recs], float)
    return max(round(float(np.percentile(ws, 25)) * 2.0) / 2.0, 1.0)












def boundary_poly_sm(mask, scale=2, eps=1.4):
    """Trace a component's outline from a 4x bicubic enlargement of it, then map back.

    Tracing at 1:1 is what makes every derived edge ragged: the JPEG's own staircase is then real
    geometry, and a 10 px tab comes out with a step in its side and an 18 px taper ends in stairs.
    Upsampling first turns that staircase into a ramp the walk can follow, so straight edges come
    back straight - which is the "平整" the snapping heuristics were trying and failing to buy.
    """
    ys, xs = np.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    sub = mask[y0:y1 + 1, x0:x1 + 1]
    im = Image.fromarray((sub * 255).astype(np.uint8)).resize(
        (sub.shape[1] * scale, sub.shape[0] * scale), Image.BICUBIC)
    big = np.asarray(im) > 127
    p = boundary_poly(big, eps=eps * scale)
    return p / scale + np.array([x0, y0], float)


def collinear_merge(poly, flat=1.2):
    """Drop vertices that lie on the line across their neighbours."""
    pts = [tuple(p) for p in poly]
    while len(pts) > 3:
        keep = []
        n = len(pts)
        for i in range(n):
            a, b, c = np.array(pts[(i - 1) % n]), np.array(pts[i]), np.array(pts[(i + 1) % n])
            d = c - a
            L = np.hypot(*d)
            dev = abs(d[0] * (a[1] - b[1]) - d[1] * (a[0] - b[0])) / max(L, 1e-9)
            if dev > flat:
                keep.append(pts[i])
        if len(keep) == len(pts) or len(keep) < 3:
            break
        pts = keep
    return np.array(pts, float)


def fit_grid(poly, dev_tol=1.2):
    """Snap a measured outline onto the grid the artwork is built from: four directions, 0/45/90/135.

    The previous version only snapped an edge that was already within 3 degrees of the grid, so every
    edge the JPEG left at 8, 15 or 20 degrees was kept verbatim - and tools/tmp/edge-grid.py counted
    9 to 31 of them per frame. That is the "不够规则": a tab's side that should be one straight slant
    comes back as a shallow zigzag.

    So the fit is constrained instead of nudged. Consecutive edges that share a grid direction are one
    run; the run's line is the least-squares line through all its points, forced parallel to that
    direction; every vertex is then the intersection of its two runs' lines. A run whose own points
    stand more than dev_tol off its fitted line is genuinely curved and is not flattened, and a vertex
    whose intersection lands more than max_move away is left where it was, because that means two long
    runs were each tilted by a degree and their intersection has flown off - which is the trade the
    whole-polygon guard used to make by throwing the entire result away.
    """
    P = collinear_merge(poly)
    n = len(P)
    if n < 3:
        return np.array(P, float)
    # A small piece gets a looser straightness test than the ring. Its 45-degree ends are only a few
    # pixels long, so the JPEG's own staircase stands 2 to 3 pixels off their line, and holding them to
    # the ring's 1.2 px threw the fit away and shipped the staircase - which on a 20 px tick is 15% of
    # its length and is exactly the ragged edge being pointed at.
    diag = float(np.hypot(*(P.max(axis=0) - P.min(axis=0))))
    dev_tol = dev_tol if diag > 60.0 else max(dev_tol, 3.0)
    lab, u = [], []
    for i in range(n):
        d = P[(i + 1) % n] - P[i]
        L = np.hypot(*d)
        if L < 1e-9:
            lab.append(-1)
            u.append(None)
            continue
        k = int(round(np.degrees(np.arctan2(d[1], d[0])) % 180.0 / 45.0)) % 4
        r = np.radians(45.0 * k)
        lab.append(k)
        u.append(np.array([np.cos(r), np.sin(r)]))
    # Rotate so edge 0 begins a new direction: otherwise the run straddling the closing vertex is
    # split in two, each fitted to the same direction but a slightly different line, and the seam
    # shows as a step at an arbitrary point of the outline.
    rot = next((i for i in range(n) if lab[i] != lab[(i - 1) % n]), 0)
    P = np.roll(P, -rot, axis=0)
    lab, u = lab[rot:] + lab[:rot], u[rot:] + u[:rot]
    runs, edge_run = [], [None] * n
    i = 0
    while i < n:
        j = i
        while j + 1 < n and lab[j + 1] == lab[i] and lab[i] >= 0:
            j += 1
        runs.append((i, j))
        for t in range(i, j + 1):
            edge_run[t] = len(runs) - 1
        i = j + 1
    if len(runs) < 2:
        return np.array(P, float)
    lines, ok, devs = [], [], []
    for (a, b) in runs:
        idx = list(range(a, b + 1)) + [(b + 1) % n]
        pts = P[idx]
        d = u[a]
        if d is None:
            lines.append(None)
            ok.append(False)
            devs.append(99.0)
            continue
        c = pts.mean(axis=0)
        dev = np.abs((pts - c) @ np.array([-d[1], d[0]]))
        lines.append((c, d))
        devs.append(float(dev.max()))
        ok.append(bool(dev.max() <= dev_tol))
    # All runs straight, or none of it. Fitting some runs and leaving others at their measured
    # vertices shears the shape: a corner then sits on one fitted line and one noisy point, and the
    # small pieces came out shifted a couple of pixels off their ink. The test is whether a run's own
    # points stand on its grid line - NOT how far its vertices moved, which is the wrong worry: two
    # runs that meet at 45 degrees cannot send their intersection far.
    if not all(ok):
        FITLOG.append((len(P), len(runs), max(devs), 0.0, False))
        return np.array(poly, float)
    out = np.empty_like(P)
    for i in range(n):
        g0, g1 = edge_run[(i - 1) % n], edge_run[i]
        L0, L1 = lines[g0], lines[g1]
        if L0 is None or L1 is None:
            out[i] = P[i]
            continue
        (p0, d0), (p1, d1) = L0, L1
        cr = d0[0] * d1[1] - d0[1] * d1[0]
        if abs(cr) < 1e-9:
            out[i] = P[i]
            continue
        w = p1 - p0
        t = (w[0] * d1[1] - w[1] * d1[0]) / cr
        out[i] = p0 + d0 * t
    fit = out
    moved = float(np.hypot(*(fit - P).T).max())
    # Two fitted lines that meet at the same point as their neighbour's intersection leave a vertex
    # with no edge under it, and an edge with no length has no direction, so it has no inward normal
    # either: the inner outline built from such a polygon spikes. Nothing downstream can recover the
    # intent, so they come out here.
    # Straightening that moves a corner by more than three pixels is not straightening a wobble, it is
    # moving the drawing - except on a small mark, where the whole thing is only a few pixels across
    # and the corners are the noisy part. There the allowance scales with the mark: a 25 px tick whose
    # hole's top edge bows a pixel needs its corners moved 4 px to come out straight, and holding it
    # to 3 threw the fit away and shipped the bow. Rejected whole rather than vertex by vertex: a
    # per-vertex fallback mixes fitted and measured corners and shears the piece off its ink.
    if moved > max(3.0, 0.30 * diag):
        FITLOG.append((len(P), len(runs), max(devs), moved, False))
        return np.array(poly, float)
    out = collinear_merge(fit)
    if len(out) < 3 or poly_area(out) < 0.5 * poly_area(P):
        FITLOG.append((len(P), len(runs), max(devs), moved, False))
        return np.array(poly, float)
    FITLOG.append((len(P), len(runs), max(devs), moved, True))
    return out




def poly_area(p):
    return 0.5 * abs(float(np.dot(p[:, 0], np.roll(p[:, 1], -1)) -
                          np.dot(p[:, 1], np.roll(p[:, 0], -1))))


def convex_hull(pts):
    """Andrew's monotone chain."""
    pts = sorted({(round(float(x), 3), round(float(y), 3)) for x, y in pts})
    if len(pts) < 3:
        return np.array(pts, float)

    def half(ps):
        out = []
        for x, y in ps:
            while len(out) >= 2 and (out[-1][0] - out[-2][0]) * (y - out[-2][1]) - \
                    (out[-1][1] - out[-2][1]) * (x - out[-2][0]) <= 0:
                out.pop()
            out.append((x, y))
        return out
    # Each half ends on the extreme point the other one starts from, so both duplicates come off their
    # own ends. Dropping the last two of the concatenation instead eats a real corner of the shape.
    return np.array(half(pts)[:-1] + half(pts[::-1])[:-1], float)


def simplify_closed(poly, eps):
    """Douglas-Peucker around a closed chain, from its two extreme points.

    Run straight over a closed polyline, dp's first chord is the one joining its endpoints - which for
    a hull is the last edge, about a pixel long. Every deviation is then measured against a direction
    set by that one pixel, the split it picks is arbitrary, and a six-corner parallelogram came back
    as a triangle. Cutting the ring at its leftmost and rightmost points gives both halves a chord as
    long as the shape.
    """
    p = np.asarray(poly, float)
    n = len(p)
    if n < 4:
        return p
    i0, i1 = int(np.argmin(p[:, 0])), int(np.argmax(p[:, 0]))
    if i0 == i1:
        return p

    def fwd(start, end):
        idx, i = [], start
        while True:
            idx.append(i)
            if i == end:
                break
            i = (i + 1) % n
        return p[idx]

    return np.vstack([dp(fwd(i0, i1), eps)[:-1], dp(fwd(i1, i0), eps)[:-1]])


def piece_outline(mask, scale=2, eps=1.4):
    """A mark's outline: its convex hull when it is convex, the four-profile walk when it is not.

    The walk cannot trace a strongly slanted hole. A parallelogram tick leaning left has its rightmost
    column at the top-right corner alone, so the right chain collapses to a point and the result is a
    triangle - which is what two of sheet6's three bottom ticks rendered as. Everything decorative on
    these frames except the ring is convex, and for a convex set the hull is not an approximation of
    the outline, it is the outline, so it goes first; the area test is only there to hand the ring and
    any hollow bar back to the walk, whose hull would cover the whole card.
    """
    ys, xs = np.nonzero(mask)
    h = convex_hull(np.column_stack([xs, ys]))
    if len(h) < 3 or poly_area(h) > 1.10 * max(mask.sum(), 1):
        return fit_grid(boundary_poly_sm(mask, scale=scale, eps=eps))
    # The hull of a pixel set is staircased - a 45-degree side comes back as alternating 1 px steps,
    # every one of them a vertex with its own direction for the fit to key off - so it is simplified
    # back to the shape's real corners first.
    return fit_grid(simplify_closed(h, eps))


def enclosed_holes(mask):
    """The regions a component encloses - read off the ink, not computed from its outline.

    Offsetting an outline inward is what this used to do, and it breaks on the small ones: a slanted
    tick converges to a 1 px tip, the four-profile outline records that as a two-vertex stub, and the
    offset of such a polygon crosses itself - which is the field of triangles the user ringed. The
    hole is the shape the drawing actually left empty, so it needs no geometry to be right.
    """
    holes = ndimage.binary_fill_holes(mask) & ~mask
    lab, n = ndimage.label(holes, structure=np.ones((3, 3), int))
    return [lab == j + 1 for j in range(n) if (lab == j + 1).sum() >= 20]


def offset_poly(poly, d, maxmiter=3.0):
    """The outline moved by `d` along its inward normals (negative d moves it outward).

    Used to build a small hollow piece's OUTER outline from its hole, rather than tracing both. When
    they were traced separately the two crossed, and since the stroke is the difference the crossing
    ate whole sides: two of sheet6's three bottom ticks came back as a top bar and a diagonal.
    """
    P = np.array(poly, float)
    n = len(P)
    nrm = inward_normals(P)
    out = np.empty_like(P)
    for i in range(n):
        n0 = np.array(nrm[(i - 1) % n][:2])
        n1 = np.array(nrm[i][:2])
        b = n0 + n1
        bn = np.hypot(*b)
        if bn < 1e-6:
            out[i] = P[i] + n1 * d
            continue
        u = b / bn
        out[i] = P[i] + u * d * min(1.0 / max(float(u @ n1), 1.0 / maxmiter), maxmiter)
    return out




def clip_inside(inner, outer):
    """The inner outline, cut back to what lies inside the outer one.

    Hand-rolled rather than by polygon-clipping library because of what this is actually used for: the
    inner is a *hole*, so any piece of it left outside the outline would stop the band from subtracting
    there and paint cyan where the drawing has nothing. Dropping those pieces is therefore exact, and
    the only thing lost is subtraction coverage in a region the band could not use anyway.
    """
    from matplotlib.path import Path as _P
    o = _P(np.asarray(outer, float))
    I = np.asarray(inner, float)
    n = len(I)
    keep = [i for i in range(n) if o.contains_point(tuple(I[i]))]
    if not keep:
        return [I]
    if len(keep) == n:
        return [I]
    # Rebuild the boundary from the inside vertices, walking each edge in turn and entering the
    # outline at the first point along it, so the result still closes around the inside region.
    out = []
    for i in keep:
        j = (i + 1) % n
        if o.contains_point(tuple(I[j])):
            out.append(I[i])
            continue
        d = I[j] - I[i]
        e = None
        for k in range(1, 20):
            q = I[i] + d * (k / 20.0)
            if not o.contains_point(tuple(q)):
                e = I[i] + d * ((k - 1) / 20.0)
                break
        out.append(I[i] if e is None else (I[i] + e) * 0.5)
    return [np.array(out, float)] if len(out) >= 3 else [I]


def self_crosses(poly):
    """Whether a closed polyline crosses itself. Adjacent segments are allowed to meet."""
    P = np.asarray(poly, float)
    n = len(P)

    def hit(a1, b1, a2, b2):
        d1, d2 = b1 - a1, b2 - a2
        cr = d1[0] * d2[1] - d1[1] * d2[0]
        if abs(cr) < 1e-12:
            return False
        w = a2 - a1
        t = (w[0] * d2[1] - w[1] * d2[0]) / cr
        u = (w[0] * d1[1] - w[1] * d1[0]) / cr
        return 0.0 < t < 1.0 and 0.0 < u < 1.0

    for i in range(n):
        for j in range(i + 2, n):
            if i == 0 and j == n - 1:
                continue
            if hit(P[i], P[(i + 1) % n], P[j], P[(j + 1) % n]):
                return (i, j)
    return None




def emit(polys, poly, mask, ox, oy, FW, FH, uniform=True, tag=''):
    """Turn one measured outline into the polygons the shader draws. Returns the line weight used."""
    recs = edge_probe(poly, mask)
    w0 = modal_width(recs)
    H, W = mask.shape
    yy, xx = np.mgrid[0:H, 0:W]
    pts = np.column_stack([xx.ravel(), yy.ravel()])
    holes = enclosed_holes(mask)

    def add(p, mode):
        polys.append(((p[:, 0] - ox) / FW, (p[:, 1] - oy) / FH, mode))

    if not holes:                                   # a solid mark: a tab, a block, a tapered bar
        add(piece_outline(mask), FILL)
        return w0

    if not uniform:
        # A small mark keeps the width the drawing gave it, and its outer outline is that hole pushed
        # out by the width - never a second trace. Offsetting the mark's own outline instead converges
        # to a knot where it is thin and pointed: a slanted tick comes to a 1 px tip, the four-profile
        # walk records that as a two-vertex stub, and the offset of such a polygon crosses itself.
        for h in holes:
            hp = piece_outline(h)
            add(offset_poly(hp, -w0), BAND_OUT)
            add(hp, BAND_IN)
        return w0
    # One weight all the way round. The ring used to be measured with a per-edge weight and an inset,
    # because that is what the reference does: its heavy right edge is drawn at x 517..523 while its
    # thin right edge is at x 532..534, so that band sits 11 px inside the outline, and its top-left
    # chamfer is 5 px against a 2 px line. Row by row the model reproduced all of it - and he read the
    # result as two borders lying side by side and asked for one line. So: no heavy runs, no inset, no
    # taper cuts. The detached marks keep their own shapes.
    add(poly, BAND_OUT)
    inner = offset_poly(poly, w0)
    # The inner outline is only meaningful inside the outline it was offset from.
    parts = clip_inside(inner, poly)
    # A ring at one weight still crosses itself wherever the outline has a one-pixel edge - two tiny
    # loops the even-odd fill cancels - so a crossing is a warning to look at, not a reason to reject.
    x = self_crosses(inner)
    TAPER[tag] = 'crosses at %s' % (x,) if x else 'clean'
    for q in parts:
        add(q, BAND_IN)
    return w0


def largest_rect(mask):
    """The biggest all-True axis-aligned rectangle, by the standard histogram sweep."""
    H, W = mask.shape
    heights = np.zeros(W, int)
    best = (0, 0, 0, 0)
    for y in range(H):
        heights = np.where(mask[y], heights + 1, 0)
        stack = []
        for x in range(W + 1):
            h = 0 if x == W else heights[x]
            sx0 = x
            while stack and stack[-1][1] >= h:
                sx, sh = stack.pop()
                sx0 = sx
                if sh * (x - sx) > best[0] * best[1]:
                    best = (x - sx, sh, sx, y - sh + 1)
            stack.append((sx0, h))
    w, h, x, y = best
    return x, y, w, h


def rasterise(polys, ox, oy, FW, FH, shape):
    """Fill every polygon the way the shader will, so the verification scores the real model."""
    H, W = shape
    yy, xx = np.mgrid[0:H, 0:W]
    pts = np.column_stack([xx.ravel(), yy.ravel()])
    drawn = np.zeros((H, W), bool)

    def cover(vu, vv):
        return Path(np.column_stack([vu * FW + ox, vv * FH + oy])).contains_points(pts).reshape(H, W)

    i = 0
    while i < len(polys):
        vu, vv, mode = polys[i]
        ins = cover(vu, vv)
        if mode == BAND_OUT:
            inner = np.zeros((H, W), bool)
            j = i + 1
            while j < len(polys) and polys[j][2] == BAND_IN:
                inner |= cover(polys[j][0], polys[j][1])
                j += 1
            drawn |= ins & ~inner
            i = j
            continue
        drawn |= ins
        i += 1
    return drawn


def measure(name, cyan, red, cell=None):
    """One frame: its polygons, its content box, and how well they match the sheet."""
    lab, cnt = ndimage.label(cyan, structure=np.ones((3, 3), int))
    sizes = ndimage.sum(cyan, lab, range(1, cnt + 1))
    mem = []
    for j in range(cnt):
        if sizes[j] <= 60:
            continue
        cid = j + 1
        m = lab == cid
        yy, xx = np.nonzero(m)
        mx, my = (xx.min() + xx.max()) / 2, (yy.min() + yy.max()) / 2
        if cell and not (cell[0] <= mx < cell[1] and cell[2] <= my < cell[3]):
            continue
        mem.append((cid, m, int(xx.min()), int(xx.max()), int(yy.min()), int(yy.max())))
    if not mem:
        return None
    ring = max(mem, key=lambda t: (t[3] - t[2] + 1) * (t[5] - t[4] + 1))
    ox = min(t[2] for t in mem)
    oy = min(t[4] for t in mem)
    FW = max(t[3] for t in mem) - ox + 1
    FH = max(t[5] for t in mem) - oy + 1
    allc = np.isin(lab, [t[0] for t in mem])

    polys = []
    rp = fit_grid(boundary_poly_sm(ring[1]))
    w0 = emit(polys, rp, ring[1], ox, oy, FW, FH, tag=name)
    for t in mem:
        if t is ring:
            continue
        w, h = t[3] - t[2] + 1, t[5] - t[4] + 1
        pp = piece_outline(t[1])
        if t[1].sum() / (w * h) > 0.55:
            polys.append(((pp[:, 0] - ox) / FW, (pp[:, 1] - oy) / FH, FILL))
        else:
            emit(polys, pp, t[1], ox, oy, FW, FH, uniform=False)

    drawn = rasterise(polys, ox, oy, FW, FH, cyan.shape)
    real = allc
    # The number that asks about the heavy parts specifically: of the ink that is thicker than the
    # line, how much does the model actually paint, and where is the rest.
    ink = distance_transform_edt(allc)
    thick = allc & (2 * ink > w0 + 1.0)
    tcover = 100.0 * (drawn & thick).sum() / max(thick.sum(), 1)
    toff = int((thick & ~drawn).sum())       # heavy pixels the model leaves unpainted
    missed = 100.0 * (real & ~drawn).sum() / max(real.sum(), 1)
    d = distance_transform_edt(~real)
    extra = 100.0 * (drawn & (d > 1.5)).sum() / max(drawn.sum(), 1)
    # The ruler for "uneven": the drawn band's width across the frame, and how much it wanders.
    core = drawn & (distance_transform_edt(~real) <= 1.5)

    if red is not None and red.any():
        rys, rxs = np.nonzero(red)
        box = ((rxs.min() - ox) / FW, (rys.min() - oy) / FH,
               (rxs.max() - ox) / FW, (rys.max() - oy) / FH)
    else:
        lab2, _ = ndimage.label(~allc, structure=np.ones((3, 3), int))
        ys, xs = np.nonzero(ring[1])
        cy, cx = (ys.min() + ys.max()) // 2, (xs.min() + xs.max()) // 2
        win = lab2[cy - 3:cy + 4, cx - 3:cx + 4]
        nz = win[win > 0]
        if not len(nz):
            box = (0.06, 0.14, 0.94, 0.90)
        else:
            interior = lab2 == int(np.bincount(nz.ravel()).argmax())
            x, y, w, h = largest_rect(interior & ~drawn)
            if w < 8 or h < 8:
                box = (0.06, 0.14, 0.94, 0.90)
            else:
                m = max(w0, 2.0) + 2.0
                box = ((x - ox + m) / FW, (y - oy + m) / FH,
                       (x - ox + w - m) / FW, (y - oy + h - m) / FH)
    return dict(name=name, FW=FW, FH=FH, polys=polys, ringverts=len(rp), missed=missed,
                extra=extra, box=box, w0=w0, drawn=drawn, real=real, core=core, thick=thick,
                tcover=tcover, toff=toff, crop=(ox, oy, FW, FH))


def load(path, with_red=False):
    a = np.asarray(Image.open(path).convert('RGB')).astype(int)
    R, G, B = a[..., 0], a[..., 1], a[..., 2]
    cyan = (B > 90) & (B - R > 40)
    red = ((R > 110) & (R - G > 50) & (R - B > 50)) if with_red else None
    return cyan, red


sc, sr = load(CARD, with_red=True)
sh, _ = load(SHEET)
frames = [measure(NAMES[0], sc, sr)]
frames += [measure(nm, sh, None, cell) for cell, nm in zip(CELLS, NAMES[1:])]
frames = [f for f in frames if f]

print("style   frame box   ring poly  line px   line missed  model off line  thick part painted"
      "  thick px left   inner")
for f in frames:
    print("%-8s %4dx%-4d  %3d  %2d    %4.1f      %6.2f%%       %6.2f%%          %6.2f%%      %5d" %
          (f['name'], f['FW'], f['FH'], f['ringverts'], len(f['polys']), f['w0'],
           f['missed'], f['extra'], f['tcover'], f['toff']))

acc = [r for r in FITLOG if r[4]]
print("corner taper: %s" % (", ".join("%s=%s" % (k, v) for k, v in TAPER.items())
                            or "every frame takes the bisector"))
print("\ngrid fit: %d of %d outlines accepted, worst point-to-line %.2f px, worst vertex moved "
      "%.1f px, rejected for %s" %
      (len(acc), len(FITLOG), max([r[2] for r in acc], default=0.0),
       max([r[3] for r in acc], default=0.0),
       ", ".join("%.1f" % r[2] for r in FITLOG if not r[4])))

if '--png' in sys.argv:
    out = sys.argv[sys.argv.index('--png') + 1]
    w = max(f['crop'][2] for f in frames)
    h = max(f['crop'][3] for f in frames)
    rows = (len(frames) + 1) // 2
    sheet = Image.new('RGB', (w * 2 + 30, h * rows + 10 * (rows + 1)), (12, 12, 12))
    for i, f in enumerate(frames):
        cx, cy, cw, ch = f['crop']
        rgb = np.zeros((ch, cw, 3), np.uint8)
        sub = lambda m: m[cy:cy + ch, cx:cx + cw]       # noqa: E731
        rgb[sub(f['real'])] = (255, 255, 255)
        rgb[sub(f['drawn'])] = (255, 40, 40)
        sheet.paste(Image.fromarray(rgb), ((i % 2) * (w + 10), (i // 2) * (h + 10)))
    sheet.save(out)
    print("white = the sheet's line, red = the model as drawn ->", out)

if '--write' in sys.argv:
    flat = [p for f in frames for p in f['polys']]
    tot_v = sum(len(p[0]) for p in flat)
    o = io.open(OUT, 'w', encoding='utf-8', newline='\n')
    o.write("// Shaders/HudFramesData.hlsl - GENERATED by tools/extract-frames.py. Do not edit.\n"
            "//\n"
            "// Every number is measured off the reference sheets. A stroke is a PAIR of polygons - the\n"
            "// outline, and that outline set inward by the frame's one line weight - and the shader\n"
            "// draws what lies between them. One weight all the way round: corners mitre instead of\n"
            "// butting, and nothing overlaps anything. See BACKLOG 3 before adding a heavy run back.\n"
            "//\n"
            "// Coordinates are in frame-box units - (0,0) top-left to (1,1) bottom-right - and widths\n"
            "// in frame HEIGHT units, since the box is not square.\n\n")
    o.write("#ifndef SW_HUDFRAMESDATA_HLSL\n#define SW_HUDFRAMESDATA_HLSL\n\n")
    o.write("#define kFrameStyles %d\n#define kFramePolys %d\n#define kFrameVerts %d\n\n"
            % (len(frames), len(flat), tot_v))
    o.write("// polygon modes\n#define kPolyFill %d\n#define kPolyBandOuter %d\n#define kPolyBandInner %d\n\n"
            % (FILL, BAND_OUT, BAND_IN))
    o.write("// (first polygon, polygon count, aspect = width/height, line weight)\n")
    o.write("static const float4 FSTYLE[kFrameStyles] = {\n")
    ps = 0
    for f in frames:
        o.write("    float4(%d, %d, %.6f, %.6f),   // %s  %dx%d px, line %.1f px\n"
                % (ps, len(f['polys']), f['FW'] / f['FH'], f['w0'] / f['FH'],
                   f['name'], f['FW'], f['FH'], f['w0']))
        ps += len(f['polys'])
    o.write("};\n\n")
    o.write("// polygon bounding box, for the early-out: (minU, minV, maxU, maxV)\n")
    o.write("static const float4 FPOLY[kFramePolys] = {\n")
    for vu, vv, mode in flat:
        o.write("    float4(%.6f, %.6f, %.6f, %.6f),\n" % (vu.min(), vv.min(), vu.max(), vv.max()))
    o.write("};\n\n")
    o.write("// polygon: (first vertex, vertex count, mode)\n")
    o.write("static const float4 FPMETA[kFramePolys] = {\n")
    vs = 0
    for vu, vv, mode in flat:
        o.write("    float4(%d, %d, %d, 0.0),\n" % (vs, len(vu), mode))
        vs += len(vu)
    o.write("};\n\n")
    o.write("// vertices, in frame-box units\nstatic const float2 FVERT[kFrameVerts] = {\n")
    for vu, vv, mode in flat:
        for i in range(len(vu)):
            o.write("    float2(%.6f, %.6f),\n" % (vu[i], vv[i]))
    o.write("};\n\n")
    o.write("// content box: (u0, v0, u1, v1). The card's is the red rectangle annotated on its own\n")
    o.write("// reference; the sheet frames take the largest rectangle inside the ring that no\n")
    o.write("// decoration intrudes on.\n")
    o.write("static const float4 FBOX[kFrameStyles] = {\n")
    for f in frames:
        o.write("    float4(%.6f, %.6f, %.6f, %.6f),   // %s\n" %
                (f['box'][0], f['box'][1], f['box'][2], f['box'][3], f['name']))
    o.write("};\n\n#endif // SW_HUDFRAMESDATA_HLSL\n")
    o.close()
    # The engine needs the same aspect the shader uses, because the card's rectangle is resolved in
    # C++ - the drag window has to land on the card, and only the engine knows this monitor's size and
    # taskbar strip. Two readers of one measurement, so both come from here.
    with io.open(META, 'w', encoding='utf-8', newline='\n') as h:
        h.write("// Engine/Packages/HudFramesMeta.hpp - GENERATED by tools/extract-frames.py. Do not edit.\n"
                "//\n"
                "// How wide each border is, as width divided by height. The shapes live in\n"
                "// Shaders/HudFramesData.hlsl; this is the one number the C++ side also has to know,\n"
                "// because the card's rectangle is resolved by the engine (it is the only thing that\n"
                "// knows this monitor) and the drag window and the shader must agree on it.\n"
                "#pragma once\n\n")
        h.write("#define kHudFrameCount %d\n\n" % len(frames))
        h.write("static constexpr float kHudFrameAspect[kHudFrameCount] = {\n")
        for f in frames:
            h.write("    %.6ff,   // %s  %dx%d px\n" % (f['FW'] / f['FH'], f['name'], f['FW'], f['FH']))
        h.write("};\n")
    print("wrote", OUT, "and", META, "polys=%d verts=%d" % (len(flat), tot_v))
