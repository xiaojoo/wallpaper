// Shaders/HudChart.hlsl - the engine's chart panel, drawn over whichever wallpaper is active.
//
// A second overlay pass next to HudCard.hlsl, in the same surface, with the same rules: it reads only
// b0 (FrameCB) and b2 (StatsCB), declares no Params, and gets its rectangle from the engine so the
// drag window and this pass cannot disagree about where the panel is.
//
// The four marks are CPU busy %, GPU busy %, memory used % and the drive's temperature in Celsius -
// chosen because all four move and because the first three share one axis while the fourth happens to
// land in the same 0..100 range as a temperature. That coincidence is only convenient, not meaningful,
// which is why every mark carries its own unit in its label: a bar at half height is 50 % for three of
// them and 50 °C for the last, and nothing about the drawing tells you which.
//
// The temperature has no performance counter and no WMI class an ordinary process can read; it comes
// from the NVMe itself (Engine/System/DiskTemp). A SATA or USB drive answers nothing, and then this
// mark is simply absent rather than a bar sitting at zero claiming a cold disk.
//
// What is drawn comes from the three references he sent, measured rather than copied
// (tools/tmp/measure-charts.py, measure-charts2.py):
//   bars    gap/width = 1.18 measured over five bars; each bar fades from its own bright top
//           (14,228,243) down to (6,46,79) at the baseline. Its *heights* are not a function of its
//           labels - 27% measures 36 px and 18% measures 34 px, and the 7% bar's top is brighter than
//           the 27% one's - so the scale here is linear 0..100 instead of a curve reverse-engineered
//           out of marketing art.
//   donut   ring thickness / outer radius = 16/40.9 = 0.39, and the arc sweep really is proportional
//           to the value: the purple segment measures 70 deg = 19.4% against a label of 18%, which is
//           inside the +-1 px floor of a 117 px tall source.
//   line    stroke 2 px on an 85 px tall image = 0.024 of the height, with dots wider than the stroke.
//
// "只要实际有数据的那部分": a metric whose source did not answer is -1 and draws nothing - no stub
// bar, no zero-length arc, no line segment. The history is -1 in the slots that have not been sampled
// yet, so four seconds after the engine starts the line is four points long and the rest is empty.

#include "Fullscreen.hlsl"
#include "Glyphs.hlsl"
#include "HudFrames.hlsl"

#define kSamples 16

// The three marks, all of them a temperature in Celsius. This is the only place the order lives: the
// bars, the legend, the line's series, the tints and the engine's history ring all index by these, so
// changing it here moves everything. PushChartSeries has to agree with it.
//
// There is no CPU-die or DIMM temperature because this machine answers neither - see the probe in
// tools/tmp/probe-cpu-ram-temp.ps1 - and the motherboard's ACPI zone is labelled MB rather than
// borrowed a CPU's name, because 27 C is not what an i9-14900KF idles at.
#define kMarkGpu 0
#define kMarkSsd 1
#define kMarkMb  2
#define kMarks   3

float Series(int m, int i) {
    return StatSeries(m, i);
}

// Distance to a segment, in auv. Used once per link of each polyline.
float SegDist(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-9));
    return length(p - (a + ab * t));
}

// "CPU 12%" / "SSD 53C" as one call: three letters, a space, a fixed-point number, and a unit.
// Returns coverage, not colour, so the caller decides how bright the label is. `px` is one device
// pixel in auv, needed because the degree mark is drawn rather than looked up - see below.
float MetricLabel(float2 auv, float2 o, float2 cell, uint n0, uint n1, uint n2,
                  float value, int fracDigits, uint unit, bool degree, float px) {
    float a = Lbl(auv, o, cell, 3, n0, n1, n2);
    float x = o.x + 3.45 * cell.x;                      // three letters and a narrow gap
    float b = HudNumber(auv, float2(x, o.y), cell, value, 2, fracDigits, cSP);
    x += (2 + (fracDigits > 0 ? 1 : 0) + (float)fracDigits) * cell.x;
    float d = 0.0;
    if (degree) {
        // The atlas cannot draw a degree sign: it is 16 x 6 cells for ASCII 32..127 and cDEG is 176,
        // so the index lands past the last row and the linear-wrap sampler reads back some other
        // glyph - which came out looking like a tilde, so "42 degrees" read as "42~". The ring is
        // drawn instead, sitting on the cap band where a degree sign belongs.
        float rad = cell.y * 0.115;
        float2 c = float2(x + cell.x * 0.32, o.y + cell.y * (kInkTop + 0.16));
        d = 1.0 - smoothstep(rad * 0.42, rad * 0.42 + px * 1.5, abs(length(auv - c) - rad));
        x += cell.x * 0.64;
    }
    // `unitInk`, not `c2`: Glyphs.hlsl #defines c0..c9 as the digit glyphs, so an identifier that
    // looks like one of them is replaced by an integer before the compiler ever sees the declaration.
    float unitInk = Lbl(auv, float2(x, o.y), cell, 1, unit);
    return max(max(max(a, b), unitInk), d);
}

// How wide MetricLabel's result is, so the caller can centre it.
float MetricLabelWidth(float2 cell, bool degree) {
    return cell.x * (6.45 + (degree ? 0.64 : 0.0));
}

float4 PSMain(VSOut i) : SV_TARGET {
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(i.uv.x * aspect, i.uv.y);
    float px = uPerf.z / uRes.y;                          // one device pixel, in auv

    int type = int(uChart2.x + 0.5);
    if (type < 1 || type > 3) return float4(0.0, 0.0, 0.0, 0.0);

    int style = int(uChart2.y + 0.5);
    float2 ctop = uChart.xy;
    float2 csz = uChart.zw;
    float2 fq = (auv - ctop) / csz;
    float fEdge, fGlow, fSolid, fInside;
    HudFrame(style, fq, px / csz.y, fEdge, fSolid, fGlow, fInside);

    float3 col = float3(0.0, 0.0, 0.0);
    col += float3(0.02, 1.00, 0.99) * max(fEdge, fSolid);
    col += float3(0.05, 0.35, 0.35) * fGlow;
    float alpha = saturate(fInside * 0.45 + max(max(fEdge, fSolid), fGlow));
    float ink = 0.0;

    float4 cb = FrameContentBox(style);
    float2 wc = ctop + csz * cb.xy;
    float2 ws = csz * float2(cb.z - cb.x, cb.w - cb.y);
    float2 wp = (auv - wc) / ws;
    bool inWell = wp.x > 0.0 && wp.x < 1.0 && wp.y > 0.0 && wp.y < 1.0;

    uint valid = uMeta.x < 0.0 ? 0u : uint(uMeta.x);
    // Every mark goes through 0..1 so one scale draws all four; `unit` says what the number means.
    float val[kMarks];
    val[kMarkGpu] = (valid & kStatGpu) != 0u ? uGpu2.x : -1.0;
    val[kMarkSsd] = uDisk3.x;
    val[kMarkMb] = uTemp.z;
    uint name0[kMarks] = { cG, cS, cM };
    uint name1[kMarks] = { cP, cS, cB };
    uint name2[kMarks] = { cU, cD, cB };
    uint unit[kMarks] = { cC, cC, cC };        // every mark carries the degree ring, then this C
    int fracD[kMarks] = { 0, 0, 0 };
    float3 tint[kMarks];
    tint[0] = float3(0.16, 0.84, 0.95);
    tint[1] = float3(0.12, 0.62, 0.74);
    tint[2] = float3(0.08, 0.38, 0.50);
    float3 lab = float3(0.34, 0.50, 0.60);

    if (inWell) {
        if (type == 1) {
            // Four bars on a shared baseline, and the labels are the point of the picture: without
            // them a bar at half height is 50 % or 50 C depending on which one you are looking at.
            // The labels live inside the well, on a strip taken off the bottom of it: putting them
            // under the baseline instead meant under the well's own edge, where the inWell test clips
            // them away and the panel silently draws four bars and no names.
            // The binding constraint on how big these can get is one bar's slot, not the panel's
            // height: at 4 marks the slot is 57 px on this panel at scale 60, and a label is 6.45
            // cells wide, so the cell could not pass ~13 px until the gap inside the label narrowed.
            float cellH = min(ws.y * 0.16, ws.x * 0.040);
            float2 cell = float2(cellH * kCellAspect, cellH);
            float plotH = max(ws.y - cellH * 1.7, ws.y * 0.30);
            float bw = ws.x / 5.36;                       // 3 bars and 2 gaps of 1.18 bar widths
            float pitch = bw * 2.18;
            float x0 = wc.x + (ws.x - (pitch * 2.0 + bw)) * 0.5;
            float base = wc.y + plotH;
            for (int k = 0; k < kMarks; ++k) {
                if (val[k] < 0.0) continue;
                // The axis is 0..100 degrees for every mark now, so the scale is one thing at last.
                float h = max(plotH * saturate(val[k] / 100.0), px);
                float2 b0 = float2(x0 + pitch * k, base - h);
                float2 p = auv - b0;                       // 0..bw across, 0..h down from the top
                float r = min(bw * 0.18, h * 0.5);
                // A box whose top two corners are rounded: outside the cap band the corner term is
                // switched off, so the bar stays square where it meets the baseline, as the reference's
                // do. Clamping the corner centre's x collapses the two circles into one across the flat
                // middle of the cap.
                float2 cc = float2(clamp(p.x, r, bw - r), r);
                float corner = length(p - cc) - r;
                float sdBox = max(max(-p.x, p.x - bw), max(-p.y, p.y - h));
                float d = max(sdBox, step(p.y, r) * corner);
                float m = 1.0 - smoothstep(-px, px, d);
                float3 dark = float3(0.024, 0.180, 0.310);
                col += lerp(tint[k], dark, pow(saturate(p.y / h), 0.8)) * m;
                ink = max(ink, m);
                // The name centred under the bar, the value centred above it.
                bool deg = true;
                float lw = MetricLabelWidth(cell, deg);
                float lb = MetricLabel(auv, float2(b0.x + (bw - lw) * 0.5, base + cellH * 0.55),
                                       cell, name0[k], name1[k], name2[k], val[k], fracD[k],
                                       unit[k], deg, px);
                col += lab * lb;
                ink = max(ink, lb);
            }
            // The baseline the bars stand on, one device pixel like the reference's.
            float bl = 1.0 - smoothstep(px * 0.5, px * 1.5, abs(auv.y - base + px * 0.5));
            col += float3(0.16, 0.84, 0.95) * bl * 0.55;
            ink = max(ink, bl * 0.55);
        } else {
            // The donut and the line have no per-mark position that says what it is, so both get the
            // same legend along the bottom of the well instead.
            float cellH = min(ws.y * 0.13, ws.x * 0.030);
            float2 cell = float2(cellH * kCellAspect, cellH);
            float legend = 0.0;
            for (int k = 0; k < kMarks; ++k) {
                if (val[k] < 0.0) continue;
                float2 o = float2(wc.x + ws.x * (0.03 + 0.32 * k), wc.y + ws.y - cellH * 1.3);
                legend = max(legend, MetricLabel(auv, o, cell, name0[k], name1[k], name2[k],
                                                 val[k], fracD[k], unit[k], true, px));
            }
            col += lab * legend;
            ink = max(ink, legend);
            // The plot area stops above the legend rather than overlapping it.
            float2 ph = float2(ws.x, ws.y - cellH * 1.7);

            if (type == 2) {
                // Four concentric arcs. Measured single-ring thickness is 0.39 of the outer radius;
                // with four rings sharing that outer radius each band is a quarter of the annulus.
                float2 ctr = float2(wc.x + ws.x * 0.5, wc.y + ph.y * 0.5);
                float rOut = min(ph.y * 0.46, ph.x * 0.5);
                float rIn = rOut * 0.61;
            float band = (rOut - rIn) / float(kMarks);
                float2 d = auv - ctr;
                float r = length(d);
                float ang = atan2(d.x, -d.y);                    // 0 at the top, clockwise
                ang = frac(ang / 6.2831853 + 1.0) * 6.2831853;
                for (int k2 = 0; k2 < kMarks; ++k2) {
                    if (val[k2] < 0.0) continue;
                    float rMid = rOut - band * (float(k2) + 0.5);
                    float t = smoothstep(band * 0.5, band * 0.5 - px, abs(r - rMid));
                    float sweep = saturate(val[k2] / 100.0) * 6.2831853;
                    float a = step(ang, sweep) * (1.0 - smoothstep(sweep - px, sweep + px, ang));
                    float m = t * a;
                    col += tint[k2] * m;
                    ink = max(ink, m);
                }
            } else {
                // Four histories. Only the links whose two ends were both sampled are drawn, so a
                // panel that has been up for four seconds shows four points and no invented flat line.
                float stroke = ph.y * 0.024;
                for (int k3 = 0; k3 < kMarks; ++k3) {
                    for (int s = 0; s < kSamples - 1; ++s) {
                        float v0 = Series(k3, s), v1 = Series(k3, s + 1);
                        if (v0 < 0.0 || v1 < 0.0) continue;
                        float x0 = wc.x + ph.x * (float(s) / float(kSamples - 1));
                        float x1 = wc.x + ph.x * (float(s + 1) / float(kSamples - 1));
                        float y0 = wc.y + ph.y * (1.0 - saturate(v0 / 100.0));
                        float y1 = wc.y + ph.y * (1.0 - saturate(v1 / 100.0));
                        float m = 1.0 - smoothstep(stroke * 0.5, stroke * 0.5 + px,
                                                   SegDist(auv, float2(x0, y0), float2(x1, y1)));
                        col += tint[k3] * m;
                        ink = max(ink, m);
                    }
                    float v = Series(k3, kSamples - 1);
                    if (v < 0.0) continue;
                    // The newest sample keeps a dot, wider than the stroke, as the reference's do.
                    float dx = wc.x + ph.x;
                    float dy = wc.y + ph.y * (1.0 - saturate(v / 100.0));
                    float m = 1.0 - smoothstep(stroke * 1.6, stroke * 1.6 + px,
                                               length(auv - float2(dx, dy)));
                    col += tint[k3] * m;
                    ink = max(ink, m);
                }
            }
        }
    }

    return float4(col, saturate(alpha + ink));
}
