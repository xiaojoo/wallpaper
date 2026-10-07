#include "Fullscreen.hlsl"

// Neon Fall: comet streaks with bright heads and fading tails, and out-of-focus bokeh. Three
// depths, each with its own speed, thickness and blur - the near layer is deliberately unreadable,
// which is what sells the depth. The cursor only moves the near layers.
//
// There used to be a layer of square "pads" here. At desktop scale they came out as hard-edged
// rectangles up to 190 px on a side, hollow or filled, and read as a rendering artefact either way,
// so the layer is gone rather than tuned.
cbuffer Params : register(b1) {
    float fall_speed;     // screen heights per second at the middle depth
    float streaks;        // columns per screen height
    float tail_len;       // how long the comet tail is
    float head_glow;
    float bokeh;          // out-of-focus discs
    float haze;           // the blue wash behind everything
    float parallax;
    int octaves;
};

float Glow(float d, float r) {
    return exp(-d * d / max(r * r, 1e-5));
}

float Soft(float d, float half_w, float feather) {
    return 1.0 - smoothstep(half_w, half_w + max(feather, 1e-4), d);
}

float3 StreakLayer(float2 auv, float t, float scale, float wpx, float seed, float blur,
                   float gain, float3 tint, float2 shift, float prob) {
    float2 p = (auv + shift) * float2(scale, scale);
    float col = floor(p.x);
    float2 cr = Hash22(float2(col, seed));
    float cx = p.x - col - 0.5 - (cr.x - 0.5) * 0.4;
    float d = abs(cx);
    float core = Soft(d, wpx * 0.5, blur + wpx * 0.5);

    float rows = p.y * (0.5 + cr.y * 0.55);
    float v = frac(rows - t * scale * (0.8 + cr.y * 0.6) + cr.x);
    float live = step(1.0 - prob, Hash11(col * 3.7 + seed));
    float tail = pow(saturate(v), max(0.6, tail_len * (0.5 + cr.y))) * live;
    float head = exp(-pow((v - 1.0) * 12.0, 2.0)) * live;

    // The tail carries most of a comet now that there are a sixth as many of them; at 0.5 the few
    // surviving streaks read as thin scratches with a dot on top.
    return tint * core * (tail * 0.85 + head * head_glow * 1.6) * gain;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);
    float px = uPerf.z / uRes.y;
    float2 mouse = (uMouse.zw - 0.5) * parallax;
    float t = uTime.x * fall_speed;
    float3 tint = float3(0.30, 0.72, 1.00);

    float3 c = lerp(float3(0.0015, 0.005, 0.013), float3(0.004, 0.017, 0.036), 1.0 - uv.y);
    float neb = Fbm(float2(auv.x * 1.2, auv.y * 1.2 - t * 0.04), max(2, octaves - 1));
    c += float3(0.05, 0.14, 0.26) * neb * haze * 0.55;
    c += tint * 0.05 * Glow(length(auv - float2(aspect * 0.42, 0.45)), 0.55) * haze;

    float sc = max(3.0, streaks);
    float w = max(0.004, 2.6 * px * sc);
    // The far layer was 2.3x the middle one's column count and 70 % of every layer's columns were
    // live, which together covered the screen in vertical lines at a pitch you could read as type.
    c += StreakLayer(auv, t * 1.6, sc * 1.45, w * 0.7, 5.0, 0.0, 0.45, tint, mouse * 0.2, 0.45);
    c += StreakLayer(auv, t, sc, w, 19.0, 0.002, 1.0, tint, mouse * 0.55, 0.34);
    c += StreakLayer(auv, t * 0.55, sc * 0.5, w * 2.4, 33.0, 0.012, 1.25,
                     lerp(tint, float3(0.8, 0.95, 1.0), 0.3), mouse * 1.1, 0.22);

    // Bokeh: sparse, large, heavily blurred, drifting slower than everything else.
    // The disc's radius and its offset from the cell centre are both kept under half a cell
    // together: a Gaussian that is still bright at the cell edge steps against the neighbouring
    // cell's different hash there, which drew a visible square around every bokeh disc.
    float2 bp = (auv + mouse * 1.4) * 5.0;
    bp.y -= t * 0.22 * 5.0;
    float2 bc = floor(bp);
    float2 bf = frac(bp) - 0.5;
    float2 bh = Hash22(bc + 4.0);
    float bOn = step(0.90, bh.x);
    c += lerp(tint, float3(0.85, 0.95, 1.0), bh.y * 0.5) * bokeh * bOn *
           Glow(length(bf - (bh - 0.5) * 0.4), 0.075 + bh.y * 0.105) * 0.7;

    c *= 1.0 - 0.5 * length((uv - 0.5) * float2(1.05, 1.0));
    c = ToneMap(c * 1.2);
    return float4(ToSrgb(c), 1.0);
}
