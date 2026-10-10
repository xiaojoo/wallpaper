// Shaders/HudCard.hlsl - the engine's own status card, drawn over whichever wallpaper is active.
//
// This is not a package. It is compiled once by D3D11Renderer and drawn as a second pass into the
// same DirectComposition surface, after the wallpaper, so it appears on every wallpaper and on none
// of them by choice - the settings window's 状态卡 switch decides, on the next frame.
//
// It reads only b0 (FrameCB) and b2 (StatsCB). It deliberately has no `Params` cbuffer: the engine
// keeps one shared b1 upload slot for all passes and resizes it whenever a pass asks for a different
// byte count, so a second shader with its own parameter block would reallocate a constant buffer and
// log a line every frame of every monitor.
//
// The frame around it is not hardcoded: HudFrames.hlsl draws whichever of the six measured frames
// the setting asks for, and the card's own reference is a wide bar, 503 x 194 px, whose content box is
// 431 x 111. The content box is where everything readable goes, and the four readouts sit side by side
// in it - a field nearly four times wider than it is tall has no room for stacked rows, which is why
// the reference leaves it as one wide band.
//
// Everything on it comes from uStats, which is this computer as the OS reports it, sampled once a
// second by Engine/System/MachineMetrics. Nothing here is decorative: a field whose source did not
// answer is -1 and its cell prints nothing at all, because a gauge drawn from an unmeasured field as
// zero claims the machine is idle - a different and much worse lie than a blank panel.

#include "Fullscreen.hlsl"
#include "Glyphs.hlsl"
#include "HudKit.hlsl"
#include "HudFrames.hlsl"

// The four row marks. Drawn procedurally rather than as glyphs: the atlas carries the ASCII range
// only, and a chip, a stick, a drive and a card are not letters. k = 0 chip, 1 memory stick,
// 2 drive, 3 expansion card.
float CardIcon(float2 uv, float2 o, float w, int k) {
    float2 q = (uv - o) / w;                       // 0..1 across the icon box
    if (min(q.x, q.y) < 0.0 || max(q.x, q.y) > 1.0) return 0.0;
    float2 c = q - 0.5;
    if (k == 0) {
        float body = (1.0 - smoothstep(0.24, 0.27, abs(c.x))) * (1.0 - smoothstep(0.24, 0.27, abs(c.y)));
        float hole = (1.0 - smoothstep(0.10, 0.13, abs(c.x))) * (1.0 - smoothstep(0.10, 0.13, abs(c.y)));
        float pin = (step(0.26, abs(c.y)) * step(abs(c.y), 0.42) * step(abs(c.x), 0.30))
                  + (step(0.26, abs(c.x)) * step(abs(c.x), 0.42) * step(abs(c.y), 0.30));
        return saturate(body - hole + pin * 0.9);
    }
    if (k == 1) {
        // A module: a slab with a row of pins under it.
        float body = (1.0 - smoothstep(0.36, 0.39, abs(c.x))) * (1.0 - smoothstep(0.18, 0.21, abs(c.y + 0.05)));
        float pins = step(0.10, c.y + 0.05) * step(c.y + 0.05, 0.26) * step(abs(c.x), 0.28)
                   * step(0.30, abs(frac(q.x * 6.0) - 0.5));
        return saturate(body + pins);
    }
    if (k == 2) {
        float body = (1.0 - smoothstep(0.34, 0.37, abs(c.x))) * (1.0 - smoothstep(0.24, 0.27, abs(c.y)));
        float slot = (1.0 - smoothstep(0.20, 0.23, abs(c.x))) * (1.0 - smoothstep(0.03, 0.06, abs(c.y - 0.08)));
        float dot = 1.0 - smoothstep(0.05, 0.08, length(c - float2(0.22, 0.12)));
        return saturate(body - slot + dot * 0.9);
    }
    // An expansion card drawn as an outline with a fan ring and three vents: filled solid, the same
    // shape reads as a camera.
    float shell = (1.0 - smoothstep(0.42, 0.45, abs(c.x))) * (1.0 - smoothstep(0.22, 0.25, abs(c.y)))
                - (1.0 - smoothstep(0.35, 0.38, abs(c.x))) * (1.0 - smoothstep(0.15, 0.18, abs(c.y)));
    float fan = 1.0 - smoothstep(0.05, 0.08, abs(length(c + float2(0.20, 0.0)) - 0.09));
    float vents = step(abs(c.y), 0.10) * step(0.0, c.x) * step(c.x, 0.32)
                * step(0.35, abs(frac((q.x - 0.5) * 5.0) - 0.5));
    return saturate(shell + fan + vents);
}

float4 PSMain(VSOut i) : SV_TARGET {
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(i.uv.x * aspect, i.uv.y);
    float px = uPerf.z / uRes.y;                          // one device pixel, in auv

    // The card's rectangle comes from the engine in uHud - its height is a fraction of screen height,
    // so a 4K and a 1080p desktop get the same shape, and its width follows the selected frame's own
    // proportions rather than being assumed. Where it sits is the engine's business too: the corner
    // margins, the taskbar strip it has to clear and anywhere the user has dragged it all resolve to
    // the same four numbers, so the drag window and this pass cannot disagree.
    int style = int(uHud2.x + 0.5);
    float2 ctop = uHud.xy;
    float2 csz = uHud.zw;

    // Frame-box units: (0,0) at the card's top-left, (1,1) at its bottom-right; `lw` is one device
    // pixel stated in those units, which is what lets a frame keep its reference's relative line
    // weight at every scale without ever falling below a pixel.
    float2 fq = (auv - ctop) / csz;
    float fEdge, fGlow, fSolid, fInside;
    HudFrame(style, fq, px / csz.y, fEdge, fSolid, fGlow, fInside);

    float3 col = float3(0.0, 0.0, 0.0);
    // The stroke's own colour, measured where it is wide enough not to be antialiased: the middle of
    // the reference's 18 px top band is (6,254,254) and of its solid bottom bar (2,255,255). The
    // median over a whole outline - (80,181,179) - is not the line's colour, it is the line mixed
    // with its own edges, and painting that back is what made the border read dull.
    col += float3(0.02, 1.00, 0.99) * max(fEdge, fSolid);
    col += float3(0.05, 0.35, 0.35) * fGlow;
    // The panel's field is black and translucent: the reference's interior is one black area from
    // edge to edge, and 45% of it over the wallpaper keeps a bar this wide from blanking a third
    // of the desktop.
    float alpha = saturate(fInside * 0.45 + max(max(fEdge, fSolid), fGlow));
    // Everything drawn inside the well is emissive and opaque in its own right, so it has to raise
    // the alpha with it - otherwise the glyphs inherit the panel's 45% and read as a watermark.
    float ink = 0.0;

    // The well's rectangle back in auv: the only place content is allowed. For the card's own
    // reference that is the red rectangle annotated on the sheet; the six sheet frames have no
    // annotation and report the interior their ring encloses.
    float4 cb = FrameContentBox(style);
    float2 wc = ctop + csz * cb.xy;
    float2 ws = csz * float2(cb.z - cb.x, cb.w - cb.y);
    float2 wp = (auv - wc) / ws;                           // well-local, 0..1
    bool inWell = wp.x > 0.0 && wp.x < 1.0 && wp.y > 0.0 && wp.y < 1.0;

    float3 txt = float3(0.34, 0.50, 0.60);
    float3 num = float3(0.50, 0.53, 0.57);

    // uMeta.x carries the bitmask as a float, because a constant buffer has no integer channel here.
    // The < 0 guard is not decoration: an uninitialised StatsCB is all -1 by design, and uint(-1.0)
    // is 0xFFFFFFFF, so a source that never sampled would answer "every group is valid".
    uint valid = uMeta.x < 0.0 ? 0u : uint(uMeta.x);
    bool cpuOk = (valid & kStatCpu) != 0;
    bool memOk = (valid & kStatMemory) != 0;
    bool gpuOk = (valid & kStatGpu) != 0;
    bool diskOk = (valid & kStatDisk) != 0 && uDisk2.y > 0.0;

    if (inWell) {
        float2 hc = float2(ws.y * 0.195 * kCellAspect, ws.y * 0.195);
        float h = Lbl(auv, wc + ws * float2(0.012, 0.015), hc, 13,
                      cS, cY, cS, cT, cE, cM, cSP, cS, cT, cA, cT, cU, cS);
        col += txt * h; ink = max(ink, h);

        // Four cells across the well. Their vertical rhythm is stated as fractions of the cell so the
        // block lands where the reference's marked box does: the heading takes the top of it, the
        // cells start a quarter of the way down and the bars finish at 0.87, leaving the same margin
        // at the bottom as the box has at the top.
        float2 cellSz = float2(ws.x * 0.25, ws.y * 0.70);
        float2 cellTop = wc + ws * float2(0.0, 0.26);
        float2 labCell = float2(cellSz.y * 0.150 * kCellAspect, cellSz.y * 0.150);
        float2 valCell = float2(cellSz.y * 0.260 * kCellAspect, cellSz.y * 0.260);

        for (int k = 0; k < 4; ++k) {
            float2 cell0 = cellTop + float2(cellSz.x * k, 0.0);
            float v = k == 0 ? (cpuOk ? uCpu.x : -1.0)
                        : k == 1 ? (memOk ? uMemory.x : -1.0)
                        : k == 2 ? (diskOk ? 100.0 * (1.0 - uDisk2.x / uDisk2.y) : -1.0)
                        : (gpuOk ? uGpu.x : -1.0);
            float ok = v >= 0.0 ? 1.0 : 0.0;

            // Line 1: the mark and the name. (`cell0`, not `c0` - the glyph header #defines c0 as
            // the digit zero, and HLSL adds a scalar to a float2 component-wise, so the collision
            // compiles clean and puts every cell 48 units off the screen.)
            float isz = cellSz.y * 0.150;
            float ic = CardIcon(auv, cell0 + float2(cellSz.x * 0.04, cellSz.y * 0.010), isz, k);
            col += float3(0.31, 0.52, 0.67) * ic; ink = max(ink, ic);

            float2 lo = cell0 + float2(cellSz.x * 0.04 + isz + labCell.x * 0.12, cellSz.y * 0.020);
            float lb = k == 0 ? Lbl(auv, lo, labCell, 3, cC, cP, cU)
                       : k == 1 ? Lbl(auv, lo, labCell, 3, cR, cA, cM)
                       : k == 2 ? Lbl(auv, lo, labCell, 4, cD, cI, cS, cK)
                       :          Lbl(auv, lo, labCell, 3, cG, cP, cU);
            col += txt * lb; ink = max(ink, lb);

            // Line 2: the number, ending at the cell's own right edge. Disk is derived from the
            // volume's free/total, and the fourth cell is GPU busy percent rather than the
            // reference's Battery - this is a desktop, and the probe answered "battery present =
            // False", so a bar with no source would be decoration.
            // Left-aligned on the bar's own left edge. It used to be right-aligned at 0.94, which
            // put a short value like "3 %" 229 px right of the bar it belongs to (measured on the
            // 4K capture at hud_scale 180), and the number read as belonging to no cell at all.
            float ny = cell0.y + cellSz.y * 0.30;
            float nx = cell0.x + cellSz.x * 0.04;
            // The digit count the value actually needs. HudNumber's field is fixed at intDigits wide
            // and pads leading zeros with blanks, which right-aligns the value inside it: asked for 3
            // cells, "5 %" started two cells (57 px on the 4K capture) right of the bar it belongs to.
            // Left-aligning means asking for exactly as many cells as there are digits, and letting the
            // percent sign follow that many cells.
            int nd = v >= 99.5 ? 3 : (v >= 9.5 ? 2 : 1);
            float dn = HudNumber(auv, float2(nx, ny), valCell, ok > 0.0 ? v : 0.0, nd, 0, cSP) * ok;
            float dp = Lbl(auv, float2(nx + (float)nd * valCell.x, ny), valCell, 1, cPCT) * ok;
            col += num * max(dn, dp); ink = max(ink, max(dn, dp));

            // Line 3: the bar. Track, fill, and a short bright cap where they meet.
            float2 b0 = float2(cell0.x + cellSz.x * 0.04, cell0.y + cellSz.y * 0.80);
            float2 bs = float2(cellSz.x * 0.90, cellSz.y * 0.075);
            float2 bq = (auv - b0) / bs;
            float inBar = step(0.0, bq.x) * step(bq.x, 1.0) * step(0.0, bq.y) * step(bq.y, 1.0);
            float frac = saturate(v / 100.0) * ok;
            col += float3(0.13, 0.17, 0.22) * inBar;
            col += float3(0.19, 0.64, 0.77) * inBar * step(bq.x, frac);
            col += float3(0.55, 0.85, 0.95) * inBar
                 * (1.0 - smoothstep(0.0, 0.035, abs(bq.x - frac))) * 0.9 * ok;
            // The track is part of the panel's own opacity, so it has to claim it too.
            ink = max(ink, inBar);
        }
    }

    return float4(col, saturate(alpha + ink));
}
