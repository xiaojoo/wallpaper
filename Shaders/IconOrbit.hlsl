// Shaders/IconOrbit.hlsl - the desktop's shortcuts on a ring, the engine's third overlay pass.
//
// Not a package. Compiled once by D3D11Renderer and drawn into the same DirectComposition surface
// after the wallpaper and after the two panels, so it reads only b0 (FrameCB) and b2 (StatsCB) and
// deliberately has no `Params` cbuffer - the engine keeps one shared b1 upload slot and a pass with a
// different byte count would reallocate it every frame of every monitor. Same rule as HudCard.hlsl.
//
// Its atlas is the runtime icon grid ShellIcons.cpp built out of the shell's own icons, bound on t1
// because slot 0 is left empty for the glyph atlas the other passes use.
//
// THE GEOMETRY IS NOT COMPUTED HERE. Every icon arrives as a finished rectangle in uOrbitIcon, in
// device pixels, placed by Application.cpp. That is a deliberate change from the first version, which
// derived position from an angle and a phase: making the ring clickable means something else has to
// put a mouse target on each icon, and if the shader and the hit layer each did the trigonometry the
// two would drift - a click landing ten pixels off the picture it points at ruins the feature. One
// side computes, the other follows.
//
// Each uOrbitIcon is (centreX, centreY, halfSize, depth) in device pixels, depth being +1 at the
// front of the ring and -1 at the back. halfSize is the box, which is one whole atlas cell - the
// artwork inside a cell stops short of its edge by ShellIcons::kCellPad, so the box edge and the
// coverage ramp below it always land on transparent gutter, never across the picture. Cutting through
// the picture is what drew the dark outline: that one antialiased pixel composites against the
// wallpaper, not against the icon behind, so wherever two icons overlapped a row of background showed
// between them (measured as (2,3,4) between a maroon icon and a pink one, over a black wallpaper).
// Nothing is culled: an icon at the back of the ring is faded toward the wallpaper by a ramp on depth
// (uOrbit3.z at the back, 1 at the front) rather than hidden, and every slot that is drawn gets a
// click rectangle - this pass can never draw something the user cannot click.

#include "Fullscreen.hlsl"

Texture2D Atlas : register(t1);
SamplerState LinearSampler : register(s0);

#define kMaxIcons 16
#define kCellPx 256.0

float4 PSMain(VSOut i) : SV_TARGET {
    int count = int(uOrbit.x + 0.5);
    if (count <= 0) return float4(0.0, 0.0, 0.0, 0.0);

    float2 px = i.uv * uRes.xy;                          // this pixel, in device pixels
    int cols = max(1, int(uOrbit2.z + 0.5));
    float rows = max(1.0, uOrbit2.w);
    float fadeBack = uOrbit3.z;
    // floor rather than a cast: -1 means "nothing hovered" and int(-1.0 + 0.5) truncates towards zero,
    // which would turn the absence into a permanent highlight on the first icon.
    int hover = int(floor(uOrbit3.w + 0.5));

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float best = -2.0;                                   // the depth of whatever acc currently holds

    for (int k = 0; k < count && k < kMaxIcons; ++k) {
        float4 e = uOrbitIcon[k];
        if (e.z <= 0.0) continue;                            // guards the divide below; no slot is hidden
        float2 d = px - e.xy;
        float half_ = e.z;
        float2 ad = abs(d);
        // One pixel of slack on the cheap reject, because the box edge is covered below rather than
        // cut: the reject still keeps the sampling off 99% of the monitor.
        if (ad.x > half_ + 1.0 || ad.y > half_ + 1.0) continue;
        float depth = e.w;
        if (depth <= best) continue;                       // something nearer already owns this pixel

        uint col = (uint)k % (uint)cols;
        uint row = (uint)floor((float)k / (float)cols);
        // The gradients are in ATLAS UV per device pixel: one cell covers 1/cols of the atlas and is
        // drawn 2*half_ pixels wide. Getting this wrong is not subtle - the first version of this line
        // passed texels-per-pixel where the hardware wants uv-per-pixel, and uv 1.64 per pixel says
        // "the whole atlas fits in one pixel", so every icon came out as the top mip: a flat grey
        // square the shape of the box, geometry and occlusion perfect.
        float2 gu = 1.0 / (float2(cols, rows) * (2.0 * half_));
        float2 atlasPx = float2(cols, rows) * kCellPx;

        // Stay one texel of the mip actually sampled inside the cell, at both ends. The cell grid and
        // the mip grid are aligned (kCellPx is a power of two), so averaging never straddles a
        // boundary - but bilinear reads the texel either side of the sample, and at the cell edge that
        // neighbour is the next icon. The first version inset by `texel`, which is one source pixel
        // expressed in atlas uv, applied to a 0..1 within-cell coordinate: a fifth of a pixel, and one
        // that did not grow with the mip level. At the back of the ring the sampled texel is 2-4 px
        // wide, so the edge samples reached into their neighbour's icon and the transparent parts of
        // the picture let it show through - a rounded outline of somebody else's app, appearing and
        // vanishing as the icon's size crossed each mip boundary while the ring turned.
        float lod = log2(max(max(gu.x * atlasPx.x, gu.y * atlasPx.y), 1.0));
        float2 inset = exp2(lod) / kCellPx;
        // Clamp the SAMPLE coordinate to the box, not just the pixel loop to the box plus one. The
        // reject and the coverage ramp both reach half_+1 so the outer half pixel can fade instead of
        // cutting, and an unclamped `d / half_` lets the texture read past the inset - at the box edge
        // that lands on the neighbouring cell's first texel, which is the transparent black margin
        // between icons. Half of a transparent black texel mixed into the edge is a dark, half-alpha
        // ring exactly one pixel wide, tracing whatever the icon's own silhouette does at the cell
        // edge: measured on the moon icon's top border as (123,37,36) - (23,37,66) - (220,68,107),
        // one row of near-black between the icon behind and the icon in front. That ring is the
        // "border" that reappears the moment two icons overlap.
        float2 dc = clamp(d, -half_, half_);
        float2 cell = (dc / half_ + 1.0) * 0.5 * (1.0 - 2.0 * inset) + inset;
        float2 uvA = float2((col + cell.x) / (float)cols, (row + cell.y) / rows);

        // SampleGrad, not Sample: a gradient-taking Sample inside a loop whose trip count varies makes
        // fxc unroll the whole thing (warning X3570), and sixteen unrolled icon tests is not a shader.
        float4 c = Atlas.SampleGrad(LinearSampler, uvA, float2(gu.x, 0.0), float2(0.0, gu.y));
        if (c.a <= 0.003) continue;

        // The box edge has to be covered, not tested. `abs(d) > half_` is a hard cut with no falloff,
        // so as the depth changes and half_ crosses a half pixel, a whole column of the icon's
        // outermost pixels appeared on one frame and vanished on the next - the border of every icon
        // whose art reaches the edge of its cell (measured: 28 of the 43 cells on this desktop)
        // pulsed while the ring turned, and nothing changed at all once the ring was held still.
        float2 cover = saturate(float2(half_ + 0.5, half_ + 0.5) - ad);
        float cov = min(cover.x, cover.y);
        c.rgb *= cov; c.a *= cov;

        // How far this icon has receded, on depth. Driving ALPHA as well as rgb is the whole effect:
        // the pass blends premultiplied-over (SrcBlend ONE, DestBlend INV_SRC_ALPHA), so one factor on
        // both is a true cross-fade with the wallpaper showing through the icon, while the first
        // version's rgb-only multiply just pushed the back icons toward black and left them opaque.
        float fade = lerp(fadeBack, 1.0, (depth + 1.0) * 0.5);
        c.rgb *= fade; c.a *= fade;
        if (k == hover) c.rgb *= 1.35;
        best = depth;
        acc = c;
    }

    return saturate(acc);
}
