#include "Fullscreen.hlsl"
#include "ParticleCommon.hlsl"

// All motion is computed on the GPU: the CPU only uploads time, resolution and these parameters.
cbuffer Params : register(b1) {
    float fall_speed;
    float wind;
    float swirl;
    float flake_size;
    float depth_fade;
    float glow;
    float sky_top;
    float sky_bottom;
    float cloud;
    int octaves;
    int count;        // overwritten by the engine so the shader can never disagree with the buffer
};

static const uint2 CORNER[6] = {
    uint2(0, 0), uint2(1, 0), uint2(1, 1),
    uint2(0, 0), uint2(1, 1), uint2(0, 1)
};

float2 Rand2(float a, float b) {
    return Hash22(float2(a * 0.75487766, b * 0.5698403));
}

// The engine compiles this one file once per stage, and fxc rejects RWStructuredBuffer outside
// cs_5_0, so the simulation is only present in the compute compile (SW_COMPUTE comes from the engine).
#ifdef SW_COMPUTE

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if ((int)id.x >= count) return;

    Particle p = gParticlesRW[id.x];
    float dt = min(uTime.y, 0.05);
    float t = uTime.x;

    p.life -= dt;
    if (p.life <= 0.0 || p.pos.y > 1.15) {
        // Respawn above the top edge. The seed keeps drifting so the same index never repeats.
        p.seed = frac(p.seed * 1.6180339887 + t * 0.00390625 + (float)id.x * 0.0001);
        float2 r = Rand2(p.seed, (float)id.x);
        p.pos = float2(r.x, -0.05 - r.y * 0.15);
        p.maxLife = 6.0 + r.y * 10.0;
        p.life = p.maxLife;
        p.size = flake_size * (0.35 + r.x * r.x * 1.9);
        float depth = saturate(p.size / max(flake_size * 2.2, 0.0001));
        p.color = float4(lerp(float3(0.62, 0.72, 0.92), float3(1.0, 1.0, 1.0), depth),
                         lerp(0.35, 1.0, depth));
        p.vel = float2(0.0, fall_speed * (0.35 + depth * 0.9));
    }

    float depth = saturate(p.size / max(flake_size * 2.2, 0.0001));
    float gust = Fbm(float2(p.pos.y * 2.2 - t * 0.05, t * 0.03 + p.seed), max(1, octaves - 2)) - 0.5;
    p.vel.x = wind * (0.4 + depth) + gust * swirl;
    p.vel.y = fall_speed * (0.35 + depth * 0.9);

    p.pos += p.vel * dt;
    if (p.pos.x > 1.2) p.pos.x -= 1.4;
    if (p.pos.x < -0.2) p.pos.x += 1.4;

    gParticlesRW[id.x] = p;
}

#endif // SW_COMPUTE

// First pass: the night sky the flakes fall over. Drawn as one fullscreen triangle.
float4 PSBackground(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float3 top = float3(0.020, 0.032, 0.062) * sky_top;
    float3 bot = float3(0.075, 0.100, 0.150) * sky_bottom;
    float3 c = lerp(bot, top, smoothstep(0.0, 1.0, 1.0 - uv.y));

    float band = Fbm(float2(uv.x * 1.6, uv.y * 1.1 - uTime.x * 0.012), max(2, octaves - 1));
    c += float3(0.055, 0.070, 0.105) * smoothstep(0.42, 0.85, band) * cloud;

    float2 cell = uv * float2(190.0, 110.0);
    float2 id = floor(cell);
    float2 f = frac(cell) - 0.5;
    float2 r = Hash22(id);
    float tw = 0.55 + 0.45 * sin(uTime.x * (0.6 + r.x * 2.0) + r.y * 6.283);
    float star = smoothstep(0.075, 0.0, length(f - (r - 0.5) * 0.7)) * step(0.965, r.y) * tw;
    c += star * float3(0.85, 0.9, 1.0) * (1.0 - uv.y);

    c *= 1.0 - 0.28 * length((uv - 0.5) * float2(1.1, 1.0));
    return float4(ToSrgb(ToneMap(c * 1.35)), 1.0);
}

struct PVSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float depth : TEXCOORD1;
};

// Second pass: six vertices per particle, because D3D11 points are always 1x1 pixel.
// It is named ParticleVS, not VSMain: Fullscreen.hlsl already owns VSMain for the sky pass.
PVSOut ParticleVS(uint vid : SV_VertexID) {
    uint i = vid / 6;
    uint c = vid % 6;
    Particle p = gParticles[i];
    float2 corner = (float2)CORNER[c];

    float2 centerPx = p.pos * uRes.xy;
    // uPerf.z scales the drawn radius for previews (see Contract.hlsl). p.size itself stays in
    // desktop pixels because the depth fade below is normalised against it.
    float2 offPx = (corner * 2.0 - 1.0) * p.size * 0.5 * max(uPerf.z, 0.001);
    float2 px = centerPx + offPx;

    PVSOut o;
    o.pos = float4(px.x * uRes.z * 2.0 - 1.0, 1.0 - px.y * uRes.w * 2.0, 0.0, 1.0);
    o.uv = corner * 2.0 - 1.0;
    float fade = saturate(p.life / max(p.maxLife * 0.25, 0.0001));
    o.color = float4(p.color.rgb, p.color.a * fade);
    o.depth = saturate(p.size / max(flake_size * 2.2, 0.0001));
    return o;
}

float4 PSMain(PVSOut i) : SV_TARGET {
    float d = length(i.uv);
    if (d > 1.0) discard;
    float core = saturate(1.0 - d * d);
    float halo = exp(-d * d * 3.0) * 0.35 * glow;
    float3 c = i.color.rgb * (core + halo) * lerp(depth_fade, 1.0, i.depth);
    return float4(c * i.color.a, 1.0);
}
