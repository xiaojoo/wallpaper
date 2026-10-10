// Shaders/HudFrames.hlsl - draw one of the measured frames.
//
// The shapes are not written here. They are measured off the reference sheets by
// tools/extract-frames.py and land in HudFramesData.hlsl. What that file emits for a stroke is a
// PAIR of polygons: the outline, and that outline set inward by the frame's one line weight. The band
// between them is the line.
//
// One continuous region at one weight is what settles "边框线条不够均匀": corners mitre by
// construction, so there is no cap to butt or round, and no step where a heavy edge meets a light one.
// The reference does vary its weight - the card's heavy right edge is drawn 11 px inside its own
// outline, beside the thin line rather than on it - and reproducing that was measured row by row to
// work, but it reads as two borders lying on top of each other, which is not wanted. So the heavy
// runs, the insets and the taper cuts are gone; see BACKLOG 3 before putting any of them back.
//
// Coordinates: the caller passes `q` in frame-box units - (0,0) top-left to (1,1) bottom-right - and
// `lw` = one device pixel in those units. Those units are NOT isotropic, since a frame is about 2.6
// times wider than it is tall, so every distance here is taken after multiplying x by the style's
// aspect. One unit then means the same thing on both axes - one frame height - and a width stated in
// them is a real screen length at any card scale. Getting that conversion wrong is what once made
// the whole card render as a solid slab.

#ifndef SW_HUDFRAMES_HLSL
#define SW_HUDFRAMES_HLSL

#include "HudKit.hlsl"
#include "HudFramesData.hlsl"

float FrameAspect(int style) { return FSTYLE[clamp(style, 0, kFrameStyles - 1)].z; }

// Where readable content is allowed to go, in frame-box units: x0, y0, x1, y1.
float4 FrameContentBox(int style) { return FBOX[clamp(style, 0, kFrameStyles - 1)]; }

// Signed distance to one polygon: negative inside, and the unsigned distance to its outline.
float3 PolySigned(float2 pq, int start, int n, float ar) {
    float d = 1e9;
    int crossings = 0;
    for (int i = 0; i < n; ++i) {
        float2 a = float2(FVERT[start + i].x * ar, FVERT[start + i].y);
        float2 b = float2(FVERT[start + (i + 1) % n].x * ar, FVERT[start + (i + 1) % n].y);
        float2 e = b - a;
        float L = length(e);
        if (L < 1e-6) continue;                       // no direction, so no side to measure
        float2 u = e / L;
        float2 w0 = pq - a;
        float2 c = a + u * clamp(dot(w0, u), 0.0, L);
        d = min(d, length(pq - c));
        if ((a.y > pq.y) != (b.y > pq.y))
            crossings += (pq.x < (b.x - a.x) * (pq.y - a.y) / (b.y - a.y) + a.x) ? 1 : 0;
    }
    bool isIn = (crossings & 1) == 1;
    return float3(isIn ? -d : d, isIn ? 1.0 : 0.0, d);
}

// The frame.
//   edge   - the outline's band, at the weight each of its edges is drawn at
//   solid  - the filled pieces: tabs and detached bars
//   glow   - the soft bloom the reference carries along its line
//   inside - 1 anywhere the panel's black goes
void HudFrame(int style, float2 q, float lw,
              out float edge, out float solid, out float glow, out float inside) {
    style = clamp(style, 0, kFrameStyles - 1);
    float4 st = FSTYLE[style];
    float ar = st.z;
    int p0 = int(st.x + 0.5);
    int np = int(st.y + 0.5);
    float2 pq = float2(q.x * ar, q.y);
    float aa = max(lw * 0.5, 1e-5);                   // half a device pixel of fade, on both edges

    edge = 0.0;
    solid = 0.0;
    inside = 0.0;
    float dring = 1e9;
    // A band is one outer polygon and however many inner ones follow it: the line is the outer region
    // minus the UNION of the inners. Pairing them one-to-one instead paints a stroke's second hole.
    float a_out = 0.0, a_in = 0.0;
    bool inBand = false;
    for (int i = 0; i < np; ++i) {
        float4 mt = FPMETA[p0 + i];
        int mode = int(mt.z + 0.5);
        float4 bb = FPOLY[p0 + i];
        // A filled piece this pixel is nowhere near can be skipped. The two halves of a band cannot:
        // skipping the inner one would leave the outer's whole interior painted.
        if (mode == kPolyFill &&
            (q.x < bb.x - lw || q.x > bb.z + lw || q.y < bb.y - lw || q.y > bb.w + lw)) continue;
        float3 r = PolySigned(pq, int(mt.x + 0.5), int(mt.y + 0.5), ar);
        if (i == 0) dring = r.z;
        if (mode == kPolyBandOuter) {
            if (inBand) edge = max(edge, saturate(a_out - a_in));
            a_out = 1.0 - smoothstep(-aa, aa, r.x);
            a_in = 0.0;
            inBand = true;
            inside = max(inside, a_out);
            continue;
        }
        if (mode == kPolyBandInner) {
            if (!inBand) continue;
            a_in = max(a_in, 1.0 - smoothstep(-aa, aa, r.x));
            continue;
        }
        solid = max(solid, r.y);
        inside = max(inside, r.y);
    }
    if (inBand) edge = max(edge, saturate(a_out - a_in));
    // Measured off the reference (tools/tmp/glow-profile.py): one pixel past its line the bloom is
    // 0.14 of the line's own peak, two pixels 0.07, three 0.02 - gone by four. It is a bloom, so it
    // lives outside the ink: added on top of it, the same term put a bright stripe along the inside
    // of every heavy band, four pixels in from its edge.
    float hair = max(st.w, lw);
    glow = (1.0 - smoothstep(hair * 0.35, hair * 1.15, dring)) * 0.38
         * (1.0 - saturate(max(edge, solid)));
}

#endif // SW_HUDFRAMES_HLSL
