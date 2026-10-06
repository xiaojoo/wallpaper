#include "Fullscreen.hlsl"

// Parameters bound from wallpaper.json by reflected name (cbuffer Params, register b1).
cbuffer Params : register(b1) {
    float speed;
    float hue;
    float star_amount;
    float curtain_strength;
    float ground_height;
    float vignette;
    float moon_size;
    int octaves;
    float2 moon_pos;
    float glow_spread;
    float shimmer;
};

float3 Sky(float2 uv, float skyHue) {
    float3 top = float3(0.012, 0.02, 0.055);
    float3 mid = float3(0.03, 0.06, 0.12);
    float3 low = Palette(skyHue) * 0.08 + float3(0.02, 0.03, 0.06);
    float t = saturate(uv.y);
    float3 c = lerp(low, mid, smoothstep(0.0, 0.55, t));
    c = lerp(c, top, smoothstep(0.45, 1.0, t));
    return c;
}

float Stars(float2 uv, float density) {
    if (density <= 0.0) return 0.0;
    float2 cell = uv * 260.0;
    float2 id = floor(cell);
    float2 f = frac(cell) - 0.5;
    float2 r = Hash22(id);
    float twinkle = 0.5 + 0.5 * sin(uTime.x * (1.0 + r.x * 3.0) + r.y * 6.283);
    float d = length(f - (r - 0.5) * 0.6);
    float s = smoothstep(0.06, 0.0, d) * step(r.x, density) * (0.35 + 0.65 * twinkle);
    return s * smoothstep(0.15, 0.75, uv.y);
}

float3 Curtain(float2 uv, float strength, int oct, float skyHue) {
    if (strength <= 0.0) return 0.0;
    float t = uTime.x * speed;

    // Domain-wrapped vertical band: the noise field scrolls sideways while each column
    // stretches upward, which is what makes an aurora read as light rather than fog.
    float2 p = float2(uv.x * 2.4 + t * 0.16, uv.y * 1.1 - t * 0.06);
    float warp = Fbm(p * 1.4 + float2(t * 0.2, 0.0), oct) - 0.5;
    float bandY = 0.52 + warp * 0.34 + (uv.x - 0.5) * 0.08;
    float dh = uv.y - bandY;

    float ribbon = exp(-dh * dh * 26.0);
    float fall = exp(-max(0.0, -dh) * 3.2);            // light hangs downward from the ridge
    float detail = Fbm(p * 3.6 + float2(0.0, t * 0.5), oct);
    float rays = smoothstep(0.35, 0.85, detail);

    float glow = exp(-dh * dh * 6.0) * 0.35;
    float3 col = Palette(skyHue + 0.12 * warp + uv.y * 0.10);
    col = lerp(col, col * float3(0.65, 1.0, 0.85), rays * 0.6);

    float a = strength * (ribbon * (0.35 + 0.65 * rays) * fall + glow * glow_spread);
    a *= smoothstep(0.02, 0.25, uv.y) * smoothstep(1.0, 0.72, uv.y);
    return col * a;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    uv.y = 1.0 - uv.y; // y up, so shaders read like a scene, not a bitmap

    float3 c = Sky(uv, hue);

    c += Stars(uv * float2(uRes.x / uRes.y, 1.0), star_amount);

    float2 mp = moon_pos;
    float md = length((uv - mp) * float2(uRes.x / uRes.y, 1.0));
    float moon = smoothstep(moon_size, moon_size * 0.86, md);
    float halo = exp(-md * md * 90.0) * 0.25;
    c += (moon * 1.15 + halo) * float3(0.9, 0.95, 1.0);

    c += Curtain(uv, curtain_strength, octaves, hue);

    // Ground: a hard silhouette band so the light has something to hang over.
    float horizon = ground_height + 0.03 * sin(uv.x * 7.0) + 0.02 * Fbm(uv * 5.0, 2);
    float g = smoothstep(horizon + 0.004, horizon - 0.004, uv.y);
    c = lerp(c, float3(0.004, 0.006, 0.012), g);

    float v = 1.0 - vignette * length((uv - 0.5) * float2(1.15, 1.0));
    c *= v;
    c = ToneMap(c * (1.0 + shimmer * 0.2));
    return float4(ToSrgb(c), 1.0);
}
