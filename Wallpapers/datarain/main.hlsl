#include "Fullscreen.hlsl"

// Data Rain: sparse columns of long light streaks at three depths. The 3D reading comes from the
// depths disagreeing - near columns are wider, brighter, slower to appear and out of focus, far ones
// are hairlines - plus a cursor parallax that only the near layers get.
cbuffer Params : register(b1) {
    float fall_speed;     // screen heights per second at the middle depth
    float columns;        // columns per screen height
    float dash_len;       // streak length as a fraction of a cell
    float line_width;     // stroke thickness in desktop pixels
    float brightness;
    float depth_spread;   // how different the three layers look from each other
    float blur_near;      // out-of-focus amount on the near layer
    float fog;            // how much the far layer sinks into the background
    float parallax;       // cursor-driven x shift, per depth
    float vignette;
    int octaves;
};

float Glow(float d, float r) { return exp(-d * d / max(r * r, 1e-5)); }

float SoftBox(float d, float half_w, float feather) {
    return 1.0 - smoothstep(half_w, half_w + max(feather, 1e-4), d);
}

float3 Layer(float2 auv, float t, float scale, float wpx, float seed, float blur, float lmul,
             float gain, float3 tint, float2 shift, float prob) {
    float2 p = (auv + shift) * float2(scale, scale);
    p.y -= t * scale;
    float col = floor(p.x);
    float2 cr = Hash22(float2(col, seed));
    // Each column owns its own lane offset and its own row density, so the three columns never
    // line up into a grid the eye can read as a table.
    float cx = p.x - col - 0.5 - (cr.x - 0.5) * 0.30;
    float rows = p.y * (0.75 + cr.y * 0.6);
    float cell = floor(rows);
    float f = frac(rows) - 0.5;
    float2 h = Hash22(float2(cell, col * 1.31 + seed));

    float on = step(1.0 - prob, h.x);
    // Every stroke used to be exactly line_width wide. Uniform hairlines on a lattice are what read
    // as typeset text; one width per column (not per dash) keeps a column coherent as a rain streak.
    float w = max(1e-4, wpx * (0.62 + cr.y * 0.9));
    float len = dash_len * (0.35 + h.y) * lmul;
    float bx = SoftBox(abs(cx), w * 0.5, blur * 0.5 + w * 0.75);
    float by = SoftBox(abs(f), len * 0.5, blur * 0.6 + len * 0.18);
    float hot = 0.45 + 0.85 * step(0.80, Hash11(col * 2.13 + seed));
    // The bloom used to be 3.2x the stroke width, and this layer's stroke is already 2.1x the middle
    // one's, so on the near layer it spread over most of a cell and the dashes merged into a grid of
    // horizontal bars. Bloom stays a fixed proportion of the stroke, and a layer that is already
    // defocused does not also get one.
    float halo = Glow(abs(cx), w * 2.6) * by * 0.42 * saturate(1.0 - blur * 30.0);
    return tint * (bx * by + halo) * on * (0.30 + 0.70 * h.y) * hot * gain;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);
    float px = uPerf.z / uRes.y;
    float2 mouse = (uMouse.zw - 0.5) * parallax;
    float t = uTime.x * fall_speed;

    float3 tint = float3(0.42, 0.78, 1.00);

    float3 c = lerp(float3(0.002, 0.006, 0.015), float3(0.006, 0.019, 0.038), 1.0 - uv.y);
    float neb = Fbm(float2(auv.x * 1.1, auv.y * 1.1 - t * 0.03), max(2, octaves - 2));
    c += float3(0.04, 0.10, 0.17) * neb * 0.30;

    float base = max(4.0, columns);
    float w = max(0.004, line_width * px * base);
    float sp = depth_spread;

    c += Layer(auv, t * 1.55, base * (1.2 + sp * 0.4), w * 0.55, 3.0,  0.000, 1.0,
               0.42 * (1.0 - fog * 0.6), tint, mouse * 0.25, 0.34);
    c += Layer(auv, t * 1.00, base,                    w,        17.0, 0.004, 1.0, 1.00, tint, mouse * 0.60, 0.26);
    // The near layer is the out-of-focus one: it is wider, so its dashes also have to be longer or
    // the defocus turns a vertical tick into a fat blob.
    c += Layer(auv, t * 0.62, base * 0.48 / max(0.2, 1.0 - sp * 0.3), w * 2.1, 41.0, blur_near * 0.05, 1.6, 1.35,
               lerp(tint, float3(0.75, 0.92, 1.0), 0.12), mouse * 1.25, 0.16);

    c *= 1.0 - 0.55 * vignette * length((uv - float2(0.5, 0.55)) * float2(1.05, 0.95));
    c = ToneMap(c * brightness * 1.15);
    return float4(ToSrgb(c), 1.0);
}
