#include "Fullscreen.hlsl"

Texture2D gGrain : register(t0);
SamplerState WrapSampler : register(s0);

// Names must match wallpaper.json's params: the engine binds the cbuffer by reflected name.
cbuffer Params : register(b1) {
    float scroll_speed;   // cells per second the traces travel downward
    float trace_density;  // column count per screen height
    float trace_width;    // line thickness in desktop pixels
    float node_glow;      // the round pads at the bends
    float spark_speed;    // how fast a bright packet runs along a trace
    float hue_shift;      // 0 cyan .. 1 deep blue
    float bg_depth;       // strength of the vertical gradient and the nebula
    float bokeh;          // soft out-of-focus dots behind the board
    float vignette;
    int octaves;
};

// One cell owns one lane. The lane holds its x for most of the cell and steps to the next lane in
// the first 18% of it, which is what turns a column of dots into right-angle bends.
float Lane(float col, float seg) {
    return (Hash21(float2(col, seg) * 1.37) - 0.5) * 0.66;
}

float PathLane(float col, float2 p) {
    float seg = floor(p.y);
    float f = frac(p.y);
    return lerp(Lane(col, seg), Lane(col, seg + 1.0), smoothstep(0.0, 0.26, f));
}

float Glow(float d, float r) {
    return exp(-d * d / max(r * r, 1e-5));
}

float BoxMask(float2 d, float2 h) {
    float2 o = abs(d) - h;
    return 1.0 - smoothstep(0.0, 0.02, max(o.x, o.y));
}

// The board: three columns are visited per pixel so a bend that leaves its own column still draws.
float3 Board(float2 p, float t, float wcells, float seed, float3 tint) {
    float3 acc = float3(0.0, 0.0, 0.0);
    int c0 = (int)floor(p.x);

    [unroll]
    for (int i = -1; i <= 1; ++i) {
        float col = (float)c0 + (float)i;
        float2 q = float2(col, p.y);
        float lane = PathLane(col + seed, q);
        float cx = col + 0.5 + lane;
        float dx = p.x - cx;

        // sigma is trace_width converted to cells; the line is a core plus a wide halo so it blooms.
        float strand = Glow(dx, wcells) + Glow(dx, wcells * 4.2) * 0.05;

        float seg = floor(q.y);
        float f = frac(q.y);
        float2 npad = float2(col + 0.5 + Lane(col + seed, seg), seg);
        float nd = length(p - npad);
        float ring = Glow(abs(nd - 0.115), 0.022) * 1.6;
        float pin = Glow(nd, 0.03) * 0.9;

        float2 r = Hash22(float2(col + seed, seg) + 11.0);
        float pad = step(0.55, r.x);
        float dash = step(0.62, Hash21(float2(col + seed, seg) + 5.0));
        float2 dq = p - float2(cx + (r.y - 0.5) * 0.34, seg + 0.62);
        float tick = BoxMask(dq, float2(0.055, 0.014)) * dash;

        // One bright head per column, wrapping over a run of cells, riding the lane it belongs to.
        float head = frac(t * spark_speed * 0.07 + Hash21(float2(col + seed, 3.7)));
        float run = frac(q.y * 0.083 - head);
        float spark = Glow(min(run, 1.0 - run) * 6.0, 0.16) * strand;

        acc += (strand * 0.42 + (ring + pin) * node_glow * pad + tick * 0.5) * tint;
        acc += spark * tint * 2.2;
    }
    return acc;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);
    // uPerf.z is the surface's fraction of the monitor height, so a thumbnail gets the same board
    // as the desktop instead of one drawn at six times the relative thickness.
    float px = uPerf.z / uRes.y;
    float t = uTime.x * scroll_speed;

    float3 tint = lerp(float3(0.20, 0.74, 1.00), float3(0.10, 0.42, 1.00), saturate(hue_shift));

    // Deep navy, brighter toward the upper middle where the reference carries its glow.
    float3 c = lerp(float3(0.002, 0.007, 0.021), float3(0.007, 0.030, 0.062),
                    (1.0 - uv.y) * 0.75 + 0.25) * (0.6 + bg_depth * 0.8);
    float neb = Fbm(float2(auv.x * 1.3, auv.y * 1.3 - t * 0.02), max(2, octaves - 2));
    c += tint * 0.05 * bg_depth * smoothstep(0.35, 1.0, neb);
    c += tint * 0.05 * bg_depth * Glow(length(auv - float2(aspect * 0.5, 0.34)), 0.42);

    // Cell size is 1/trace_density of the screen height, in aspect-corrected units.
    float scale = max(6.0, trace_density);
    float2 p = float2(auv.x * scale, auv.y * scale - t);
    float wcells = max(0.004, trace_width * px * scale);

    // Two depths of board: the far one is thinner and dimmer, which is what reads as distance.
    c += Board(p * float2(1.0, 1.0), t, wcells, 0.0, tint) * 1.0;
    c += Board(p * 0.62 + float2(4.3, 9.1), t * 0.62, wcells * 1.4, 17.0, tint * 0.42) * 0.35;

    // Bokeh sits behind the traces and drifts slower than they scroll.
    float2 bp = float2(auv.x, auv.y) * 7.0 + float2(0.0, -t * 0.25);
    float2 bc = floor(bp);
    float2 bf = frac(bp) - 0.5;
    float2 br = Hash22(bc + 2.0);
    float bOn = step(0.80, br.x);
    c += lerp(tint, float3(0.35, 0.75, 1.0), br.y * 0.4) * bokeh * bOn *
           Glow(length(bf - (br - 0.5) * 0.5), 0.05 + br.y * 0.04) * 0.6;

    float grain = gGrain.Sample(WrapSampler, float2(auv.x, uv.y) * 0.4 + float2(t * 0.004, -t * 0.01)).r;
    c *= 0.94 + grain * 0.12;

    float v = 1.0 - vignette * length((uv - 0.5) * float2(1.05, 1.0));
    c *= v;
    c = ToneMap(c * 1.15);
    return float4(ToSrgb(c), 1.0);
}
