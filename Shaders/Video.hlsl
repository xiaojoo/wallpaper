#include "Fullscreen.hlsl"

// Video wallpapers: the decoded frame arrives as two planes because that is what every video decoder
// produces - luma in R8, chroma in R8G8 at half height. The engine uploads them top-down and cropped
// to the display height, so this shader needs no row flip (the coded-vs-display extra rows and the
// plane order are handled once, in Nv12Uploader).
//
// The placement block below is deliberately a copy of Image.hlsl's: it is the same fit contract
// (uPerf.w = the 壁纸铺展 setting) applied to a picture instead of a photo. Keep the two in step if
// a mode is added there.
Texture2D gY : register(t0);
Texture2D gUV : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer Params : register(b1) {
    float zoom;
    float src_aspect;
    float vignette;
    float drift;
    float saturation;
    float brightness;
    float src_w;        // picture width in pixels; the engine writes it from the package manifest
    float bt601;        // 1 when the clip declares BT.601 primaries; anything else (including a
                        // manifest that predates this param) decodes as BT.709, the HD default
};

#define FIT_FILL    0
#define FIT_FIT     1
#define FIT_STRETCH 2
#define FIT_CENTER  3
#define FIT_TILE    4

float3 YuvToRgb(float y, float2 uv) {
    const float c = y - (16.0 / 255.0);
    const float d = uv.x - 0.5;
    const float e = uv.y - 0.5;
    float3 rgb;
    if (bt601 > 0.5) {
        rgb.r = 1.164383 * c + 1.596027 * e;
        rgb.g = 1.164383 * c - 0.391762 * d - 0.812968 * e;
        rgb.b = 1.164383 * c + 2.017232 * d;
    } else {
        rgb.r = 1.164383 * c + 1.792741 * e;
        rgb.g = 1.164383 * c - 0.213249 * d - 0.532909 * e;
        rgb.b = 1.164383 * c + 2.112402 * d;
    }
    return rgb;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float sa = max(src_aspect, 0.0001);
    float fit = round(uPerf.w);
    if (src_w < 1.0 && (fit == FIT_CENTER || fit == FIT_TILE)) fit = FIT_FILL;

    float2 win = float2(1.0, 1.0);
    if (fit == FIT_FILL) win = (sa > aspect) ? float2(aspect / sa, 1.0) : float2(1.0, sa / aspect);
    else if (fit == FIT_FIT) win = (sa > aspect) ? float2(1.0, sa / aspect) : float2(aspect / sa, 1.0);
    else if (fit == FIT_CENTER || fit == FIT_TILE) {
        float2 px = float2(src_w, src_w / sa) * max(uPerf.z, 0.0001);
        win = uRes.xy / max(px, float2(1.0, 1.0));
    }

    float2 p = (fit == FIT_TILE) ? uv * win - 0.5 : (uv - 0.5) * win;
    p /= max(zoom, 0.05);
    p += 0.5;
    bool bare = (fit == FIT_FIT || fit == FIT_CENTER) && (any(p < 0.0) || any(p > 1.0));
    if (fit == FIT_TILE) p = frac(p);

    // Normalized coordinates are shared by both planes: a half-height chroma plane covers the same
    // picture, so chroma row L/2 sits at v = (L/2 + 0.5) / (H/2) = (L + 1) / H, which is p.y. Halving
    // p.y here read chroma from a quarter of the picture away - inside one flat colour band it looks
    // perfect, and only the luma ramp exposed it (the grey steps came back cyan).
    float3 c = YuvToRgb(gY.Sample(LinearSampler, p).r, gUV.Sample(LinearSampler, p).rg);
    c = bare ? float3(0.0, 0.0, 0.0) : c;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, saturation);
    c *= brightness;
    c *= 1.0 - vignette * length((uv - 0.5) * float2(1.1, 1.0));
    return float4(saturate(c), 1.0);
}
