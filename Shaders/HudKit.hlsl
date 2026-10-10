// Shaders/HudKit.hlsl - the two shapes every heads-up panel in this engine is built from.
//
// Both were written inside Wallpapers/machinedrift and moved here when the status card became an
// engine-level overlay: the card and the wallpaper's own header bands have to agree on what a corner
// looks like, and two copies of the same distance function drift apart the first time one is tuned.

#ifndef SW_HUDKIT_HLSL
#define SW_HUDKIT_HLSL

// Distance to a box with each of its four corners cut at 45 degrees; `c` = (top-left, top-right,
// bottom-right, bottom-left) and each entry is that corner's cut, measured along both axes.
//
// The maximum of the five half-plane distances, which is exact along every edge and short by at most
// a factor of sqrt(2) in the two wedges beside each vertex - under a pixel of a two pixel border.
//
// Four independent cuts rather than one, because the reference's header bands are not symmetric: each
// is square at its outer corners and cut back the whole height of itself at the end facing the centre,
// and the two are cut at *opposite* ends. That is a half turn, not a mirror.
float CutBox(float2 p, float2 b, float4 c) {
    float2 a = abs(p);
    float k = 0.70710678;
    float tl = (-p.x - p.y - (b.x + b.y - c.x)) * k;
    float tr = ( p.x - p.y - (b.x + b.y - c.y)) * k;
    float br = ( p.x + p.y - (b.x + b.y - c.z)) * k;
    float bl = (-p.x + p.y - (b.x + b.y - c.w)) * k;
    return max(max(max(a.x - b.x, a.y - b.y), max(tl, tr)), max(br, bl));
}

// Distance to a plain rounded rectangle, negative inside. `b` is the half size and `r` the corner
// radius, both in the same units as `p`. The content well inside the reference's frame is rounded
// rather than cut, so this one has to exist next to CutBox rather than being replaced by it.
float RoundedBox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Signed distance to an equilateral triangle with circumradius r, one vertex pointing down the screen
// (y grows downwards). Three half-planes: the top edge sits at r/2 above the centre, and the two
// slanted edges are one plane reflected in x, which is why abs() covers both.
float DeltaInk(float2 p, float r) {
    return max(-p.y - r * 0.5, 0.8660254 * abs(p.x) + p.y * 0.5 - r * 0.5);
}

#endif // SW_HUDKIT_HLSL
