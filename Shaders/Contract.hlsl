// Shaders/Contract.hlsl - the engine <-> wallpaper constants. Every wallpaper sees exactly this.
// Include guards, not #pragma once: the engine resolves includes through a custom ID3DInclude that
// hands the compiler a fresh buffer per Open(), so pragma once treats one file as several.
#ifndef SW_CONTRACT_HLSL
#define SW_CONTRACT_HLSL

cbuffer Frame : register(b0) {
    float4 uTime;   // x: seconds since this wallpaper started, y: frame delta, z: frame index, w: target fps
    float4 uRes;    // xy: render target pixels, zw: 1/pixels
    float4 uMouse;  // xy: cursor in this monitor's pixels, zw: same normalised 0..1
    float4 uScene;  // x: quality 0..4 (battery..ultra), y: DPI scale, z: monitor index, w: monitor count
    float4 uPerf;   // x: measured fps, y: render scale, z: size scale (see below), w: image fit mode
// uPerf.w is the user's 壁纸铺展 setting, only meaningful to a wallpaper that draws a picture:
// 0 fill (scale to cover, crop the overflow) 1 fit (whole picture, black bars) 2 stretch (distort to
// fill) 3 center (native pixels, black around) 4 tile (native pixels, repeated). See Image.hlsl.
// uPerf.z is 1.0 on a real screen. Previews and thumbnails render the same shader into a much
// smaller surface, and a package that sizes anything in absolute pixels (snowfall's flake radius)
// would otherwise draw it at the same pixel size on a sixth of the canvas - i.e. six times too big
// for the picture. The engine puts the surface's fraction of the monitor height here, so pixel
// extents can scale while parameters keep meaning "pixels on the desktop".
};

float Hash11(float p) {
    p = frac(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return frac(p);
}

float2 Hash22(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx + p3.yz) * p3.zy);
}

float Hash21(float2 p) {
    float2 q = frac(p * float2(127.1, 311.7));
    return frac(dot(q, q.yx + q.xy * 0.7) + q.x * q.y);
}

float Noise2(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash21(i);
    float b = Hash21(i + float2(1.0, 0.0));
    float c = Hash21(i + float2(0.0, 1.0));
    float d = Hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float Fbm(float2 p, int octaves) {
    float sum = 0.0;
    float amp = 0.5;
    [loop]
    for (int i = 0; i < octaves; ++i) {
        sum += amp * Noise2(p);
        p = p * 2.03 + float2(1.7, 9.2);
        amp *= 0.5;
    }
    return sum;
}

float3 Palette(float t) {
    return saturate(0.5 + 0.5 * cos(6.2831853 * (t + float3(0.0, 0.33, 0.67))));
}

float3 ToneMap(float3 c) {
    return c / (c + 1.0);
}

// "linear" is a reserved HLSL interpolation modifier, so the parameter is named c.
float3 ToSrgb(float3 c) {
    c = max(c, float3(0.0, 0.0, 0.0));
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}

#endif // SW_CONTRACT_HLSL
