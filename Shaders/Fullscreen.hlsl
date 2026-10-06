// Shaders/Fullscreen.hlsl - the engine contract every wallpaper includes.
// The engine draws one triangle that covers the render target, so uv runs 0..1 across the monitor.
// Include guards, not #pragma once: the engine resolves includes through a custom ID3DInclude that
// hands the compiler a fresh buffer per Open(), so pragma once treats one file as several.
#ifndef SW_FULLSCREEN_HLSL
#define SW_FULLSCREEN_HLSL

#include "Contract.hlsl"

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

#endif // SW_FULLSCREEN_HLSL
