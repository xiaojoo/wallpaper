// Shaders/ParticleCommon.hlsl - the particle layout the engine's structured buffer uses.
// Include guards, not #pragma once: the engine resolves includes through a custom ID3DInclude.
#ifndef SW_PARTICLE_COMMON_HLSL
#define SW_PARTICLE_COMMON_HLSL

#include "Contract.hlsl"

// The stride is fixed at 48 bytes; changing any field changes ParticleSystem::Stride on the C++ side.
struct Particle {
    float2 pos;       // 0..1 screen space, y down
    float2 vel;       // units per second
    float4 color;     // rgb + alpha multiplier
    float life;       // seconds remaining
    float maxLife;
    float size;       // pixels
    float seed;
};

StructuredBuffer<Particle> gParticles : register(t0);   // the vertex shader reads

// fxc only defines RW* types for cs_5_0, so the write view exists only in the compute compile.
// The engine passes SW_COMPUTE=1 when it compiles the entry named in wallpaper.json entries.cs.
#ifdef SW_COMPUTE
RWStructuredBuffer<Particle> gParticlesRW : register(u0);
#endif

// Every CSMain in this engine is [numthreads(64, 1, 1)]: the engine divides its particle count by
// 64 to pick the group count, so a wallpaper that used another size would be dispatched wrongly.

#endif // SW_PARTICLE_COMMON_HLSL
