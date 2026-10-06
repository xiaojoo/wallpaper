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

// The per-frame constants every wallpaper shader sees (cbuffer "Frame", register b0).
struct FrameCB {
    float time = 0.f, delta = 0.f, frame = 0.f, targetFps = 0.f;
    float resX = 1.f, resY = 1.f, invX = 1.f, invY = 1.f;
    float mouseX = 0.f, mouseY = 0.f, mouseNX = .5f, mouseNY = .5f;
    float quality = 2.f, pixelRatio = 1.f, monitorIndex = 0.f, monitorCount = 1.f;
    // uPerf.w was padding. The image fit mode (ImageFit in WallpaperManager.hpp) travels here so a
    // settings change takes effect on the next frame instead of a slot reload.
    float fpsMeasured = 0.f, renderScale = 1.f, sizeScale = 1.f, fit = 0.f;
};
static_assert(sizeof(FrameCB) % 16 == 0, "FrameCB must be 16-byte aligned");

} // namespace sw
