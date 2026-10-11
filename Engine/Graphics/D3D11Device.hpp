#pragma once
// Engine/Graphics/D3D11Device.hpp - one device shared by every monitor, per plan (lower VRAM).
#include "Engine/Core/Platform.hpp"
#include <unordered_map>

struct IWICImagingFactory;

namespace sw {

struct AdapterInfo {
    LUID luid{};
    std::wstring name;
    bool software = false;
};

class D3D11Device {
public:
    bool Init(bool debugLayer);
    void Shutdown();

    ID3D11Device* device() const { return dev.Get(); }
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<IDXGIFactory2> factory;
    Com<IDCompositionDevice> comp;
    Com<IWICImagingFactory> wic;

    const std::vector<AdapterInfo>& adapters() const { return adapters_; }
    // Which adapter owns the output covering this desktop-pixel rect; -1 if unknown.
    int AdapterForMonitorRect(const RECT& desktopRect) const;
    std::wstring AdapterName(int index) const;

private:
    std::vector<AdapterInfo> adapters_;
    std::vector<LUID> adapterLuid_;
};

// How far back the chart panel's history reaches, in one-second samples. A multiple of four so the
// series is a whole number of float4 rows in the constant buffer.
constexpr int kChartSamples = 16;

// How many shortcuts the ring can draw at once. The per-pixel loop in Shaders/IconOrbit.hlsl runs this
// many times per pixel of the ring's bounding box, so this is a frame-cost ceiling, not a feature
// limit - the atlas itself holds far more than this.
constexpr int kMaxOrbitIcons = 16;

// The per-frame constants every wallpaper shader sees (cbuffer "Frame", register b0).
struct FrameCB {
    float time = 0.f, delta = 0.f, frame = 0.f, targetFps = 0.f;
    float resX = 1.f, resY = 1.f, invX = 1.f, invY = 1.f;
    float mouseX = 0.f, mouseY = 0.f, mouseNX = .5f, mouseNY = .5f;
    float quality = 2.f, pixelRatio = 1.f, monitorIndex = 0.f, monitorCount = 1.f;
    // uPerf.w was padding. The image fit mode (ImageFit in WallpaperManager.hpp) travels here so a
    // settings change takes effect on the next frame instead of a slot reload.
    float fpsMeasured = 0.f, renderScale = 1.f, sizeScale = 1.f, fit = 0.f;
    // uClock: the wall clock. `time` only counts from this wallpaper's start, so a package that
    // shows the hour had nothing to read - and a burnt-in time can never become the current one.
    float clockH = 0.f, clockM = 0.f, clockS = 0.f, clockSub = 0.f;
    // uHud: the status card's own rectangle on this monitor, in screen heights from its top-left
    // corner. The engine resolves it - not the shader - because it is the only thing that knows this
    // monitor's size, its taskbar strip and where the user dropped the card, and because the drag
    // window has to land on the same rectangle the card is drawn into. Like `fit` it travels in b0 so
    // flipping it in the settings window shows on the next frame rather than needing a slot reload.
    // A wallpaper never reads any of these: they exist only so the HUD pass can share b0 with the
    // wallpaper pass instead of owning a second b1, which would fight the package's own parameter
    // buffer over one shared upload slot.
    float hudX = 0.f, hudY = 0.f, hudW = 0.f, hudH = 0.f;
    // uHud2: x = which of Shaders/HudFrames.hlsl's measured borders draws the frame.
    float hudFrame = 0.f, hudPad0 = 0.f, hudPad1 = 0.f, hudPad2 = 0.f;
    // uChart / uChart2: the chart panel, which is the same arrangement one step over - its own
    // rectangle resolved by the engine for the same reasons, and its own border style. `chartType`
    // is 1 bar, 2 donut, 3 line; 0 means the panel is off and nothing is drawn.
    float chartX = 0.f, chartY = 0.f, chartW = 0.f, chartH = 0.f;
    float chartType = 0.f, chartFrame = 0.f, chartPad1 = 0.f, chartPad2 = 0.f;
    // uOrbit / uOrbit2 / uOrbit3: the desktop shortcut ring, the third overlay pass. Same reason for
    // living in b0 as the two panels above it - the engine and only the engine knows this monitor's
    // size, and a second b1 would fight the package's parameter buffer over the one shared upload
    // slot. The ring's geometry is no longer a radius and a phase because the click layer has to land
    // on exactly the pixels the shader paints; what is left here is the count and two atlas numbers.
    float orbitCount = 0.f, orbitPad0 = 0.f, orbitPad1 = 0.f, orbitPad2 = 0.f;
    float orbitPad3 = 0.f, orbitPad4 = 0.f, orbitCols = 8.f, orbitRows = 1.f;
    float orbitPad5 = 0.f, orbitPad6 = 0.f, orbitFadeBack = 0.25f, orbitHover = -1.f;
    float orbitPad7 = 0.f, orbitPad8 = 0.f, orbitPad9 = 0.f, orbitPad10 = 0.f;
    // uOrbitIcon: one row per ring slot, xy the centre in this surface's device pixels, z the half
    // size, w the depth. `orbitFadeBack` is the opacity an icon has at the very back of the ring; it
    // rises to 1 at the front. `kMaxOrbitIcons` is the same number
    // as `kMaxIcons` in Shaders/IconOrbit.hlsl and the `16` in the contract's array declaration; the
    // static_assert below catches the two drifting apart on this side.
    float orbitIcon[kMaxOrbitIcons][4];
    static_assert(kMaxOrbitIcons <= 32, "IconOrbit.hlsl only reads 16 slots");
};
static_assert(sizeof(FrameCB) % 16 == 0, "FrameCB must be 16-byte aligned");

// Once per frame, from every path that draws (desktop slots, live preview, thumbnails). GetLocalTime
// is served from the shared user-data page rather than a syscall, and it picks up a timezone change
// or a manual re-set without anyone having to invalidate anything.
inline void FillWallClock(FrameCB& f) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    f.clockH = float(st.wHour);
    f.clockM = float(st.wMinute);
    f.clockS = float(st.wSecond);
    f.clockSub = float(st.wMilliseconds) * 0.001f;
}

// What this computer is doing, as one constant buffer at register b2. The mirror of the "Stats" block
// in Shaders/Contract.hlsl: the two layouts have to stay in step field for field, and the order is
// float4-aligned so the HLSL and the C++ agree without packing attributes.
//
// -1 means "no answer", inherited from the rule in GpuMonitor.hpp: on a gauge, zero and unknown look
// identical, and a wallpaper that draws an unmeasured field as 0 is claiming the machine is idle.
// uFlags.x carries the validity bitmask as well, so a package can test a whole group at once.
struct StatsCB {
    float cpu[4];      // total %, total % from the second instrument, user %, privileged %
    float memory[4];   // used %, used MB, total MB, commit MB
    float gpu[4];      // busy %, vram busy %, vram used MB, vram total MB
    float gpu2[4];     // temp C, power W, SM clock MHz, unused
    float disk[4];     // read B/s, write B/s, active %, run queue length
    float disk2[4];    // free GB, total GB, process count, uptime s
    float net[4];      // recv B/s, send B/s, unused, unused
    float meta[4];     // validity bitmask, age of this sample s, logical core count, unused
    float core[32];    // per logical core %, -1 at and past meta.z
    // The last kChartSamples seconds of the four readouts the panels show, oldest first, and -1 in
    // the slots that have not been sampled yet - which is how a chart knows to leave them undrawn
    // instead of plotting a run of zeros for a minute it never measured. It rides here rather than in a
    // buffer of its own because the overlay pass already binds b2, and a second constant buffer for the
    // overlay would be a second upload slot every frame.
    // Layout: [sample * 4 + metric], metrics in card order: cpu, memory, disk, gpu. One row of four
    // per sample because the shader reads them as float4 - a scalar array would cost each element its
    // own 16-byte register on that side and the two would silently disagree. See uCore4 there.
    float series[4 * kChartSamples];
    float disk3[4];    // temp C, warning C, critical C, sensor count - all -1 when the drive says no
    float temp[4];     // the chart panel's marks: gpu C, drive C, motherboard thermal zone C, unused

    StatsCB() {
        // Same reason MachineStats does this: a zero-initialised constant buffer reads as an idle
        // machine, which is the one thing an unmeasured field must never look like.
        float* p = cpu;
        for (size_t i = 0; i < sizeof(StatsCB) / sizeof(float); ++i) p[i] = -1.f;
        // One exception, and it is a trap worth naming: the validity bitmask is carried as a float,
        // and uint(-1.0) is 0xFFFFFFFF - every group reported valid. An unmeasured buffer must claim
        // nothing, so the mask starts at zero rather than at -1.
        meta[0] = 0.f;
    }
};
static_assert(sizeof(StatsCB) % 16 == 0, "StatsCB must be 16-byte aligned");

} // namespace sw
