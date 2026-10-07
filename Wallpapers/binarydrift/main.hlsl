#include "Fullscreen.hlsl"

// Light Drift: soft round motes at three depths, brightest at the top and dissolving into the haze
// below. These used to be 0 and 1 glyphs from a 3x5 bitmap font - at desktop scale the columns read
// as a terminal full of text (measured: 37 % of the pixels above background, one mark every 27 px),
// so the characters are gone and only the light they were drawn in stayed.
cbuffer Params : register(b1) {
    float fall_speed;     // screen heights per second at the middle depth
    float density;        // mote cells per screen height
    float brightness;
    float fade_height;    // how far down the top-weighted brightness reaches
    float haze;           // the wash the motes dissolve into
    float blur_near;      // out-of-focus on the near layer
    float depth_spread;   // how far apart the three layers look
    float parallax;       // cursor-driven x shift
    float mote_size;      // disc radius as a fraction of its cell
    int octaves;
};

float3 MoteLayer(float2 auv, float t, float scale, float seed, float blur, float gain,
                 float3 tint, float2 shift, float size, float prob) {
    float2 p = (auv + shift) * float2(scale, scale);
    p.y -= t * scale;
    float2 cell = floor(p);
    float2 f = frac(p) - 0.5;
    float2 h = Hash22(cell + seed);

    float on = step(1.0 - prob, h.x);
    // Sitting on the cell centre made every mote part of a lattice you could count. The offset is
    // hashed per cell, so the depths overlap without ever lining up into rows.
    float2 off = (Hash22(cell * 1.7 + seed + 3.0) - 0.5) * 0.26;
    // 0.86 keeps the disc round: a cell is square in p but the y axis is the one the layout spans.
    float d = length((f - off) * float2(1.0, 0.86));
    // A blob whose support stays inside its own cell. A Gaussian of the same radius is shorter to
    // write but never reaches zero before the cell edge, and the neighbouring pixel samples a
    // different cell's hash there - that seam drew a hard square around every bright mote.
    // 0.155 is the largest radius that still fits: 2r / 0.86 in y plus 0.13 of jitter < half a cell.
    float r = min(size * (0.55 + h.y * 0.9) * (1.0 + blur * 1.5), 0.155);
    float s = saturate(1.0 - (d * d) / max(4.0 * r * r, 1e-8));
    float disc = s * s * (3.0 - 2.0 * s);
    float hot = 0.45 + 0.85 * step(0.80, Hash11(cell.x * 1.7 + seed));
    return tint * disc * gain * (0.45 + 0.55 * h.y) * hot * on;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float2 auv = float2(uv.x * aspect, uv.y);
    float2 mouse = (uMouse.zw - 0.5) * parallax;
    float t = uTime.x * fall_speed;
    float3 tint = float3(0.36, 0.74, 0.98);

    float3 c = lerp(float3(0.0012, 0.004, 0.010), float3(0.005, 0.018, 0.036), pow(1.0 - uv.y, 1.6));
    float neb = Fbm(float2(auv.x * 1.0, auv.y * 1.0 - t * 0.02), max(2, octaves - 1));
    c += float3(0.04, 0.11, 0.19) * neb * haze * 0.45;

    float sc = max(6.0, density);
    float sp = depth_spread;
    float size = clamp(mote_size, 0.005, 0.5);

    // The far layer used to be 2.9x the middle one's cell count, which is where the text-like field
    // came from; 1.5x still reads as depth without turning into a screen full of dots.
    c += MoteLayer(auv, t * 1.5, sc * (1.15 + sp * 0.35), 2.0,  0.0,
                   0.40 * (1.0 - haze * 0.4), tint, mouse * 0.22, size * 0.75, 0.34);
    c += MoteLayer(auv, t, sc, 13.0, 0.35, 1.0, tint, mouse * 0.55, size, 0.26);
    c += MoteLayer(auv, t * 0.6, sc * 0.7, 29.0, blur_near,
                   0.7, lerp(tint, float3(0.8, 0.93, 1.0), 0.12), mouse * 1.15, size * 1.7, 0.16);

    // Top-weighted, like the reference: the columns dissolve as they fall.
    c *= lerp(0.30, 1.0, pow(1.0 - uv.y, max(0.25, fade_height)));
    c += float3(0.02, 0.05, 0.09) * haze * pow(uv.y, 2.0);

    c *= 1.0 - 0.5 * length((uv - 0.5) * float2(1.02, 0.95));
    c = ToneMap(c * brightness * 1.25);
    return float4(ToSrgb(c), 1.0);
}
