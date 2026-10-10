#include "Fullscreen.hlsl"
#include "Glyphs.hlsl"
#include "HudKit.hlsl"

Texture2D Figure : register(t0);

// Joint positions, as a fraction of the figure's own box, read off the extracted drawing rather than
// guessed: the crop is the front view at 445x1042 px (aspect 0.427), and these are where its armour
// actually puts each joint. x is across the figure, y down it.
static const float2 JOINT[11] = {
    float2(0.500, 0.070),   // head
    float2(0.220, 0.170),   // shoulder, left
    float2(0.780, 0.170),   // shoulder, right
    float2(0.100, 0.370),   // elbow, left
    float2(0.900, 0.360),   // elbow, right
    float2(0.090, 0.490),   // hand, left
    float2(0.900, 0.480),   // hand, right
    float2(0.350, 0.530),   // hip, left
    float2(0.650, 0.520),   // hip, right
    float2(0.270, 0.760),   // knee, left
    float2(0.720, 0.720),   // knee, right
};
#define REACTOR float2(0.570, 0.220)

// Machine Drift - part 1 of the resource HUD: the readouts, no skeleton yet.
//
// Everything on screen comes from uStats (register b2), which is this computer as the OS reports it,
// sampled once a second by Engine/System/MachineMetrics. Nothing here is decorative: a field whose
// source did not answer is -1, and its row prints NO DATA instead of a number, because a gauge drawn
// at zero claims the machine is idle.

cbuffer Params : register(b1) {
    float bar_top;        // the header bands' top edge, 0 = top of the monitor
    float bar_h;          // their height, and the unit every other header size is set in
    float bar_w;          // one band's width, in auv
    float bar_margin;     // how far each band sits in from its own side of the screen
    float bars_y;         // baseline the per-core bars stand on, 0 = top
    float bars_height;    // a core at 100%
    float figure_h;       // the suit's height as a fraction of screen height
    float figure_cx;      // its centre, in auv (1 = one screen height)
    float figure_cy;
    float figure_ink;     // line brightness
    float scan_speed;     // down-travels per second, before the network term
    float ring_ink;
    float brightness;
};

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);
    float px = uPerf.z / uRes.y;                          // one device pixel, in auv

    float3 c = float3(0.006, 0.011, 0.017);
    float3 ink = float3(0.42, 0.80, 0.92);      // cyan: structure
    float3 dim = float3(0.17, 0.34, 0.41);
    float3 hot = float3(0.98, 0.66, 0.24);      // amber: the two things that run hot

    float t0 = uTime.x;

    // ---- the suit -----------------------------------------------------------------------------
    // The line art is the blueprint's own front view, cut out by tools/extract-figure.py: white lines
    // above luminance 185 (the blueprint grid sits at 118..140, so one threshold separates the drawing
    // from both its field and its grid), dead-straight drafting runs removed, stored as an alpha mask
    // at 2x nearest. The figure is 445x1042 in the source, so at this size its lines are enlarged
    // about 1.9x - they are the drawing's own pixels, not redrawn vectors, which is the price of
    // matching it stroke for stroke instead of re-deriving a suit from its proportions.
    float fh = max(0.1, figure_h);
    float2 fsize = float2(fh * 0.427, fh);
    float2 fo = float2(figure_cx - fsize.x * 0.5, figure_cy - fsize.y * 0.5);
    float2 fu = (auv - fo) / fsize;
    bool onFig = fu.x > 0.0 && fu.x < 1.0 && fu.y > 0.0 && fu.y < 1.0;

    // Two readings drive the overlays: how hot the card is, and how busy the machine is. A group that
    // never answered leaves its field at -1, and saturate() takes that to 0 - an unmeasured sensor
    // dims the element rather than lighting it.
    float heat = saturate((uGpu2.x - 30.0) / 50.0);
    float netRate = max(uNet.x, 0.0) / 8388608.0;      // one extra full scan per 8 MB/s received

    if (onFig) {
        float4 fig = Figure.Sample(LinearSampler, fu);
        // BGRA, not RGBA: Texture.cpp converts every image to DXGI_FORMAT_B8G8R8A8, so
        // .z is red and .w is alpha. Reading the line mask from .r read the blue channel,
        // which the atlas generator fills with a constant - that is what made the suit a
        // flat teal rectangle instead of a drawing.
        float stroke = fig.z;                 // the drawing's own lines, written to red
        float body = fig.w;                   // the filled interior, carried in alpha
        // No vertical flip. WIC hands the PNG over top-down, so v = 0 is already the top of the
        // drawing; flipping it put the boots where the helmet belongs.
        // The scan travels down the torso and its rate carries the network throughput, so the suit
        // moves faster when the machine is moving data. Clipped to the body so it is not a bar
        // spanning the empty dark either side of the armour.
        float scanY = frac(t0 * (scan_speed + netRate));
        float scan = (1.0 - smoothstep(0.0, 0.030, abs(fu.y - scanY))) * body;
        c += ink * (stroke * figure_ink + scan * 0.5);

        // 32 ribs across the chest, one per logical core: the scheduler's own view, laid onto the
        // torso where a HUD would put an equaliser, and masked to the body for the same reason.
        float ribH = 0.20 / 32.0;
        int ri = int(floor((fu.y - 0.28) / ribH));
        if (ri >= 0 && ri < 32 && ri < int(max(uMeta.z, 1.0))) {
            float cl = saturate(uCore[ri] / 100.0);
            float band = 1.0 - smoothstep(0.28, 0.46, abs(frac((fu.y - 0.28) / ribH) - 0.5));
            float across = step(0.28, fu.x) * step(fu.x, 0.72);
            c += lerp(dim, hot, cl) * band * across * body * (0.08 + 1.05 * cl) * ring_ink;
        }

        // The reactor: a ring with a modest core, brightening with the GPU temperature. The
        // falloff has to start at the radius, not inside it, or it arrives as a filled sun.
        float2 rv = (fu - REACTOR) * float2(fsize.x / max(fsize.y, 0.001), 1.0);
        float rr = length(rv) / 0.030;
        float ring = smoothstep(0.72, 0.92, rr) * (1.0 - smoothstep(1.0, 1.22, rr));
        c += hot * (ring * 1.5 + (1.0 - smoothstep(0.0, 0.75, rr)) * 0.30)
           * (0.35 + 1.3 * heat) * ring_ink;
    }

    // Joint rings, each a dotted circle turning at the rate of one core. Placed in the figure's own
    // space, so they stay on the armour when it is resized.
    if (onFig) {
        for (int j = 0; j < 11; ++j) {
            float2 d = (fu - JOINT[j]) * float2(fsize.x / max(fsize.y, 0.001), 1.0);
            float r = length(d);
            float rad = 0.030;
            float ring = smoothstep(rad * 0.40, rad * 0.75, r) * (1.0 - smoothstep(rad * 0.72, rad, r));
            float ang = atan2(d.y, d.x);
            float spin = saturate(uCore[(j * 3) % 32] / 100.0);
            float dots = step(0.5, frac(ang * 1.5915 + t0 * (0.25 + 1.6 * spin)));
            c += lerp(ink, hot, spin) * ring * dots * (0.25 + 0.85 * spin) * ring_ink;
        }
    }

    // ---- the header bands ----------------------------------------------------------------------
    // The reference's own top edge: a thin band at each corner. The left one carries the reactor and
    // the name, the right one the tagline on two lines. Each is cut back the whole height of itself at
    // the end facing the centre, so the pair reads as one open frame instead of two boxes - and the two
    // cuts sit at opposite ends, which is the half turn the drawing uses rather than a mirror.
    float barBw = max(px * 1.2, bar_h * 0.055);
    float3 barLine = float3(0.19, 0.23, 0.28);
    float3 barTxt  = float3(0.42, 0.55, 0.64);
    float3 barHot  = float3(0.30, 0.55, 0.66);
    float2 barA = float2(bar_margin, bar_top);
    float2 barB = float2(aspect - bar_margin - bar_w, bar_top);
    float2 barH = float2(bar_w * 0.5, bar_h * 0.5);
    float4 cutA = float4(bar_h * 0.17, 0.0, bar_h * 1.58, bar_h * 0.17);
    float4 cutB = float4(bar_h * 1.58, bar_h * 0.17, bar_h * 0.17, 0.0);
    float eA = 1.0 - smoothstep(barBw * 0.6, barBw * 1.6,
                                abs(CutBox(auv - (barA + barH), barH, cutA)));
    float eB = 1.0 - smoothstep(barBw * 0.6, barBw * 1.6,
                                abs(CutBox(auv - (barB + barH), barH, cutB)));
    c += barLine * max(eA, eB);

    // The bright segment riding the top edge at each band's inner end.
    float topEdge = 1.0 - smoothstep(barBw * 0.7, barBw * 1.9, abs(auv.y - bar_top));
    float accA = step(barA.x + bar_w * 0.58, auv.x) * step(auv.x, barA.x + bar_w * 0.975) * topEdge;
    // The right band's top edge does not start at its own left end: the taper eats the first 23% of
    // it, and an accent laid over that part floats in open space beside the diagonal.
    float accB = step(barB.x + bar_w * 0.28, auv.x) * step(auv.x, barB.x + bar_w * 0.66) * topEdge;
    c += barHot * max(accA, accB);

    // The reactor: a ring with a delta inside it, point down.
    float2 bctr = barA + float2(bar_h * 0.78, bar_h * 0.5);
    float  rad  = bar_h * 0.45;
    float2 bd   = auv - bctr;
    float  blen = length(bd);
    float  ring = (1.0 - smoothstep(rad * 0.72, rad * 0.80, blen))
                * smoothstep(rad * 0.54, rad * 0.62, blen);
    float  dOut = DeltaInk(bd, rad * 0.60);
    float  dIn  = DeltaInk(bd, rad * 0.36);
    float  delta = (1.0 - smoothstep(barBw * 0.5, barBw * 1.5, dOut))
                 - (1.0 - smoothstep(barBw * 0.5, barBw * 1.5, dIn));
    c += barHot * (ring + saturate(delta)) * 1.4;
    c += float3(0.62, 0.86, 0.96) * (1.0 - smoothstep(rad * 0.06, rad * 0.14, blen)) * 1.2;

    // Both faces are set in the atlas's one monospaced grid, so the two lines are sized to fit the
    // band rather than to match the reference's cap heights: its condensed face gets a taller name
    // into the same run of pixels. Proportional advances are what closes that - see BACKLOG.
    float2 ttlCell = float2(bar_h * 0.62 * kCellAspect, bar_h * 0.62);
    float2 subCell = float2(bar_h * 0.36 * kCellAspect, bar_h * 0.36);
    c += barTxt * Lbl(auv, barA + float2(bar_h * 1.62, bar_h * 0.19), ttlCell, 12,
                      cJ, cDOT, cA, cDOT, cR, cDOT, cV, cDOT, cI, cDOT, cS, cDOT);
    float subX = barB.x + bar_w - bar_h * 0.30 - 17.79 * subCell.x;
    c += barTxt * Lbl(auv, float2(subX, barB.y + bar_h * 0.13), subCell, 18,
                      cJ, cU, cS, cT, cSP, cA, cSP, cR, cA, cT, cH, cE, cR, cSP, cV, cE, cR, cY);
    c += barTxt * Lbl(auv, float2(subX, barB.y + bar_h * 0.51), subCell, 18,
                      cI, cN, cT, cE, cL, cL, cI, cG, cE, cN, cT, cSP, cS, cY, cS, cT, cE, cM);

    // The per-core bars: one column per logical core, height = that core's load. This is the
    // scheduler's own view of the machine, 32 values, redrawn every second.
    float coreN = max(uMeta.z, 1.0);
    float barsW = 0.94, bx = 0.03;
    float colW = barsW / coreN;
    float fieldX = auv.x / aspect - bx;
    int ci = int(floor(fieldX / colW));
    float base = bars_y;
    if (ci >= 0 && ci < int(coreN) && ci < 32) {
        float load = uCore[ci];
        float h = max(max(load, 0.0) / 100.0 * bars_height, 0.0018);
        float rel = frac(fieldX / colW);
        float inX = step(abs(rel - 0.5), 0.39);
        float inY = step(0.0, base - auv.y) * step(base - auv.y, h);
        c += lerp(dim, hot, saturate(load / 100.0)) * inX * inY * 0.9;
    }
    float onField = step(bx, auv.x / aspect) * step(auv.x / aspect, bx + barsW);
    c += dim * (1.0 - smoothstep(0.0, 0.0018, abs(auv.y - base))) * onField * 0.9;

    return float4(saturate(c * brightness), 1.0);
}
