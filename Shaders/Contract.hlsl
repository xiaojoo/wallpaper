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
    float4 uClock;  // x: hour 0..23, y: minute, z: second, w: fraction of the running second
    float4 uHud;    // the status card's rectangle on this monitor, in screen heights from its
                    // top-left: x, y, width, height. The engine resolves it, not the card: only the
                    // engine knows this monitor, its taskbar strip, and where the user dragged to.
    float4 uHud2;   // x: which of the measured borders draws the frame
    float4 uChart;  // the chart panel's rectangle, same units as uHud
    float4 uChart2;  // x: 1 bar, 2 donut, 3 line, 0 off. y: which border draws its frame
    // uOrbit: the desktop shortcut ring, the third engine-level overlay pass. x: how many slots this
    // frame offers, 0 turns the pass off. The rest of this row and all of uOrbit4 are spare; the ring's
    // geometry is no longer here because it is not derived any more - see uOrbitIcon.
    float4 uOrbit;
    // z: atlas columns. w: atlas rows. These two are the atlas's own shape and cannot be inferred from
    // uOrbit.x - the ring draws 12 of the 43 icons the grid holds, and deriving rows from that read
    // every cell three rows too far down, which came out as multicoloured horizontal stripes.
    float4 uOrbit2;
    // z: the opacity of the icon at the very back of the ring (1.0 means no fade). w: which slot the
    // pointer is over, -1 for none.
    float4 uOrbit3;
    float4 uOrbit4;
    // The icon boxes, in this surface's device pixels: xy = centre, z = half width, w = depth, +1 at
    // the front of the ring and -1 at the back.
    //
    // Placed by Application.cpp rather than from an angle here, because making the ring clickable put
    // a second reader on the same geometry - the invisible window that turns a pixel into a program -
    // and two independent trigonometries drift apart. One side computes, the other follows.
    float4 uOrbitIcon[64];
    // Which atlas cell each of those slots paints, in .x (the other three components are padding the
    // constant buffer's 16-byte rows force anyway). Slot order and cell order are the same thing only
    // when the ring paints every shortcut on the desktop in name order; as soon as the user can pick a
    // subset, the slot is a position on the ring and the cell is which shortcut sits at it.
    float4 uOrbitCell[64];
// uHud and uHud2 belong to the engine's own overlay pass, not to packages. A wallpaper that reads them
// gets nothing useful: they are where the HUD is going to be drawn over it, in screen heights.
// The card's margins are not here either - they are part of the card's own design, measured off the
// reference, and only the monitor-specific facts travel from the engine.
// uClock is the wall clock, not uTime. A wallpaper that shows the time has to read it from here:
// uTime restarts whenever the slot reloads, so it can only ever show "how long this has been up".
// w exists so the digit that just changed can flash for a moment instead of swapping silently.
// uPerf.w is the user's 壁纸铺展 setting, only meaningful to a wallpaper that draws a picture:
// 0 fill (scale to cover, crop the overflow) 1 fit (whole picture, black bars) 2 stretch (distort to
// fill) 3 center (native pixels, black around) 4 tile (native pixels, repeated). See Image.hlsl.
// uPerf.z is 1.0 on a real screen. Previews and thumbnails render the same shader into a much
// smaller surface, and a package that sizes anything in absolute pixels (snowfall's flake radius)
// would otherwise draw it at the same pixel size on a sixth of the canvas - i.e. six times too big
// for the picture. The engine puts the surface's fraction of the monitor height here, so pixel
// extents can scale while parameters keep meaning "pixels on the desktop".
};

// What this computer is doing, at register b2. The mirror of StatsCB in
// Engine/Graphics/D3D11Device.hpp - the two have to stay in step field for field.
//
// Declared here so every wallpaper can see it, and harmless to the ones that do not: fxc strips an
// unused cbuffer, so a package that never reads uStats has no b2 slot at all and the engine binding
// one costs it nothing.
//
// Every field is -1 when its source did not answer, never 0. A gauge drawn from an unmeasured field
// as zero claims the machine is idle, which is a different and much worse lie than a blank panel.
// Test uFlags.x against the kStat bits below before drawing a group.
cbuffer Stats : register(b2) {
    float4 uCpu;     // x: total %, y: total % from the second instrument, z: user %, w: privileged %
    float4 uMemory;  // x: used %, y: used MB, z: total MB, w: commit MB
    float4 uGpu;     // x: busy %, y: vram busy %, z: vram used MB, w: vram total MB
    float4 uGpu2;    // x: temp C, y: power W, z: SM clock MHz, w: unused
    float4 uDisk;    // x: read B/s, y: write B/s, z: active %, w: run queue length
    float4 uDisk2;   // x: free GB, y: total GB, z: process count, w: uptime s
    float4 uNet;     // x: recv B/s, y: send B/s, zw: unused
    float4 uMeta;    // x: kStat bitmask, y: age of this sample in s, z: logical core count, w: unused
    // Per logical core %, -1 at and past uMeta.z, four cores to a register.
    //
    // It has to be declared as float4 rows rather than `float uCore[32]`: fxc gives every element of a
    // scalar array its own 16-byte register (a probe with `float a[32]` reflects Size 500, not 128),
    // while the C++ StatsCB packs them four to a register. Declared as a scalar array, `uCore[9]` read
    // byte 272 of a buffer whose ninth core sits at byte 164 - the wallpaper's per-core bars were
    // showing cores 0,4,8,... and running off the end of the array.
    float4 uCore4[8];
    // The last 16 seconds of the chart panel's four marks, one sample per row: x cpu %, y gpu %,
    // z drive temperature C, w gpu %. -1 means "never sampled yet" or "this drive does not answer",
    // so four seconds after the engine starts a panel has four points and twelve absences rather than a
    // flat zero claiming an idle machine.
    //
    // This order is the chart's, not the card's (cpu / memory / disk / gpu): the card shows disk space
    // and the chart shows drive temperature, so sharing one order across both would make one of the two
    // wrong somewhere. The chart's own labels are what identify its marks.
    float4 uSeries[16];
    float4 uDisk3;   // x: drive temp C, y: its warning C, z: its critical C, w: sensor count; -1 = no answer
    float4 uTemp;    // the chart panel's three marks: x gpu C, y drive C, z motherboard zone C, w unused
};

// One logical core's %, by index 0..31. See uCore4 for why this is a function.
float StatCore(int i) {
    float4 r = uCore4[i >> 2];
    return (i & 3) == 0 ? r.x : (i & 3) == 1 ? r.y : (i & 3) == 2 ? r.z : r.w;
}

// One metric's history sample, metrics in card order: 0 cpu, 1 memory, 2 disk, 3 gpu.
float StatSeries(int m, int i) {
    float4 r = uSeries[i];
    return m == 0 ? r.x : m == 1 ? r.y : m == 2 ? r.z : r.w;
}

// Bits of uFlags.x, matching MachineStats::Valid.
#define kStatCpu      (1u << 0u)
#define kStatCores    (1u << 1u)
#define kStatMemory   (1u << 2u)
#define kStatGpu      (1u << 3u)
#define kStatDisk     (1u << 4u)
#define kStatNetwork  (1u << 5u)
#define kStatSystem   (1u << 6u)

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
