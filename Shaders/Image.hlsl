#include "Fullscreen.hlsl"

Texture2D gImage : register(t0);
SamplerState LinearSampler : register(s0);

// Shared preset for image wallpapers: wallpaper.json points at this file and lists one texture.
cbuffer Params : register(b1) {
    float zoom;
    float src_aspect;
    float vignette;
    float drift;
    float saturation;
    float brightness;
    float src_w;        // picture width in pixels; the engine writes it from the package manifest
};

// uPerf.w, from the 壁纸铺展 setting on the 通用 page (Contract.hlsl lists the numbers).
#define FIT_FILL    0
#define FIT_FIT     1
#define FIT_STRETCH 2
#define FIT_CENTER  3
#define FIT_TILE    4

float4 PSMain(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float aspect = uRes.x / uRes.y;
    float sa = max(src_aspect, 0.0001);
    float fit = round(uPerf.w);
    // Centre and tile are measured in picture pixels, so a package that does not say how wide its
    // picture is cannot do them. It gets fill rather than a picture magnified by the screen width.
    if (src_w < 1.0 && (fit == FIT_CENTER || fit == FIT_TILE)) fit = FIT_FILL;

    // `win` is how much of the picture the screen holds, in picture-uv units: below 1 crops the
    // overflow away, above 1 leaves room around the picture. Cover fit is the axis that overflows
    // getting multiplied down. Dividing by these factors - what this line used to do - widens the
    // window past the texture instead, and with the sampler wrapping, a square photo on a 16:9
    // screen filled only the middle 56% of the height and the bands above and below it were the
    // opposite edge of the photo repeated.
    float2 win = float2(1.0, 1.0);   // stretch: the whole picture, whatever shape that makes it
    if (fit == FIT_FILL) win = (sa > aspect) ? float2(aspect / sa, 1.0) : float2(1.0, sa / aspect);
    // 适应 is 裁剪填充 turned inside out: the same axis is the binding one, but instead of dropping
    // the overflow the window widens past the picture, so the picture keeps its shape and the screen
    // gets bars. A square in a 16:9 frame is height-bound, so it is the window's x that goes over 1.
    else if (fit == FIT_FIT) win = (sa > aspect) ? float2(1.0, sa / aspect) : float2(aspect / sa, 1.0);
    else if (fit == FIT_CENTER || fit == FIT_TILE) {
        // Native size. uPerf.z is what this surface holds of the monitor (1.0 on the desktop, less in
        // the preview), so a centred picture there is the same picture as on the desktop and not a
        // corner of it blown up to fill the box.
        float2 px = float2(src_w, src_w / sa) * max(uPerf.z, 0.0001);
        win = uRes.xy / max(px, float2(1.0, 1.0));
    }

    // Tiling counts whole copies from the top-left; everything else places one copy at the centre.
    float2 p = (fit == FIT_TILE) ? uv * win - 0.5 : (uv - 0.5) * win;
    p /= max(zoom, 0.05);
    if (fit == FIT_FILL)
        p += float2(sin(uTime.x * 0.02) * drift, cos(uTime.x * 0.017) * drift * 0.6);   // slow Ken Burns
    p += 0.5;
    // The sampler wraps, so out-of-range coordinates would repeat the picture where 适应 and 居中
    // want a border. Tiling asks for the repeat, so it decides its own coordinates first.
    bool bare = (fit == FIT_FIT || fit == FIT_CENTER) && (any(p < 0.0) || any(p > 1.0));
    if (fit == FIT_TILE) p = frac(p);
    // Deliberately no y flip. WIC hands rows over top-down and Texture.cpp copies them straight in,
    // so texture v = 0 is the picture's top row; the big triangle already puts uv.y = 0 at the top of
    // the render target (Fullscreen.hlsl: pos.y = 1 - uv.y * 2). Flipping here drew every photo - the
    // desktop, the preview and the cached card thumbs - upside down.

    float3 c = gImage.Sample(LinearSampler, p).rgb;
    c = bare ? float3(0.0, 0.0, 0.0) : c;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(luma.xxx, c, saturation);
    c *= brightness;
    c *= 1.0 - vignette * length((uv - 0.5) * float2(1.1, 1.0));
    return float4(saturate(c), 1.0);
}
