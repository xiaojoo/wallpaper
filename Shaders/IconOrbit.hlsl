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
// front of the ring and -1 at the back. A slot the centre object hides is still in the array - with
// halfSize 0 - because the atlas cell is the slot number, so packing the array would slide every
// remaining icon onto somebody else's picture. Zero size draws nothing and is not clickable either,
// which is the property that matters here: this pass can never draw something the user cannot click.

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
    float dimBack = uOrbit3.z;
    // floor rather than a cast: -1 means "nothing hovered" and int(-1.0 + 0.5) truncates towards zero,
    // which would turn the absence into a permanent highlight on the first icon.
    int hover = int(floor(uOrbit3.w + 0.5));
    // One atlas texel in uv. Inset the sample by half of it so bilinear at a box edge cannot pull in
    // the neighbouring icon - the atlas is a grid, not a strip.
    float2 texel = 1.0 / (float2(cols, rows) * kCellPx);

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float best = -2.0;                                   // the depth of whatever acc currently holds

    for (int k = 0; k < count && k < kMaxIcons; ++k) {
        float4 e = uOrbitIcon[k];
        if (e.z <= 0.0) continue;                            // a hidden slot, see the header
        float2 d = px - e.xy;
        float half_ = e.z;
        if (abs(d.x) > half_ || abs(d.y) > half_) continue;   // cheap reject before any sampling
        float depth = e.w;
        if (depth <= best) continue;                       // something nearer already owns this pixel

        uint col = (uint)k % (uint)cols;
        uint row = (uint)floor((float)k / (float)cols);
        float2 cell = (d / half_ + 1.0) * 0.5 * (1.0 - 2.0 * texel) + texel;
        float2 uvA = float2((col + cell.x) / (float)cols, (row + cell.y) / rows);

        // SampleGrad, not Sample: a gradient-taking Sample inside a loop whose trip count varies makes
        // fxc unroll the whole thing (warning X3570), and sixteen unrolled icon tests is not a shader.
        //
        // The gradients are in ATLAS UV per device pixel: one cell covers 1/cols of the atlas and is
        // drawn 2*half_ pixels wide. Getting this wrong is not subtle - the first version of this line
        // passed texels-per-pixel (256*256/(2*half*256) = 1.64) where the hardware wants uv-per-pixel
        // (0.0008), and uv 1.64 per pixel says "the whole atlas fits in one pixel", so every icon came
        // out as the top mip: a flat grey square the shape of the box, geometry and occlusion perfect.
        float2 gu = 1.0 / (float2(cols, rows) * (2.0 * half_));
        float4 c = Atlas.SampleGrad(LinearSampler, uvA, float2(gu.x, 0.0), float2(0.0, gu.y));
        if (c.a <= 0.003) continue;

        c.rgb *= lerp(dimBack, 1.0, (depth + 1.0) * 0.5);
        if (k == hover) c.rgb *= 1.35;
        best = depth;
        acc = c;
    }

    return saturate(acc);
}
