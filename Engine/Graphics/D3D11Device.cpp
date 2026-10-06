#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Core/Log.hpp"

#include <dxgidebug.h>
#include <wincodec.h>

namespace sw {
static constexpr const char* MOD = "gfx";

bool D3D11Device::Init(bool debugLayer) {
    HRESULT hr = S_OK;
    // WIC / DComp live in the same apartment as the render thread that owns this device.
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Error(MOD, "CoInitializeEx failed: {}", HResultToString(hr));
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;

    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got{};

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want,
                           (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr) && debugLayer) {
        Warn(MOD, "debug-layer device creation failed ({}), retrying without it", HResultToString(hr));
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want,
                               (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION, &dev, &got, &ctx);
    }
    if (FAILED(hr)) {
        Error(MOD, "D3D11CreateDevice failed: {}", HResultToString(hr));
        return false;
    }
    Info(MOD, "D3D11 device ready, feature level 0x{:X}", (unsigned)got);

        // A factory we create ourselves exposes the DXGI 1.2+ interfaces; the device's own parent is
    // only IDXGIFactory1 and QIs to nothing newer on this driver stack.
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { Error(MOD, "CreateDXGIFactory2: {}", HResultToString(hr)); return false; }
    factory->MakeWindowAssociation(nullptr, DXGI_MWA_NO_ALT_ENTER);

    Com<IDXGIFactory5> f5;
    if (SUCCEEDED(factory.As(&f5))) {
        BOOL mirror = FALSE;
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &mirror, sizeof(mirror));
        Info(MOD, "DXGI allow-tearing supported: {}", mirror ? "yes" : "no");
    }

    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> a;
        HRESULT r = factory->EnumAdapters1(i, &a);
        if (r == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(r)) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        AdapterInfo info;
        info.luid = d.AdapterLuid;
        info.name = d.Description;
        info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!info.software) {
            adapters_.push_back(info);
            adapterLuid_.push_back(d.AdapterLuid);
            Info(MOD, "adapter[{}] {} (VRAM {} MB)", adapters_.size() - 1, ToUtf8(d.Description),
                 (unsigned long long)(d.DedicatedVideoMemory >> 20));
        }
    }

    Com<IDXGIDevice> dxgiDev;
    hr = dev.As(&dxgiDev);
    if (FAILED(hr)) { Error(MOD, "query IDXGIDevice for DComp: {}", HResultToString(hr)); return false; }
    hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&comp));
    if (FAILED(hr)) { Error(MOD, "DCompositionCreateDevice: {}", HResultToString(hr)); return false; }

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) { Error(MOD, "WIC factory: {}", HResultToString(hr)); return false; }

    return true;
}

void D3D11Device::Shutdown() {
    if (ctx) ctx->ClearState();
    if (ctx) ctx->Flush();
    wic.Reset();
    comp.Reset();
    factory.Reset();
    ctx.Reset();
    dev.Reset();
    CoUninitialize();
}

int D3D11Device::AdapterForMonitorRect(const RECT& desktopRect) const {
    int fallback = -1;
    for (size_t ai = 0; ai < adapterLuid_.size(); ++ai) {
        Com<IDXGIAdapter1> a;
        Com<IDXGIFactory1> f1;
        if (FAILED(dev.As(&f1))) continue;
        if (FAILED(factory->EnumAdapters1((UINT)ai, &a))) continue;
        for (UINT oi = 0;; ++oi) {
            Com<IDXGIOutput> o;
            if (FAILED(a->EnumOutputs(oi, &o))) break;
            DXGI_OUTPUT_DESC od{};
            o->GetDesc(&od);
            if (od.DesktopCoordinates.left == desktopRect.left && od.DesktopCoordinates.top == desktopRect.top &&
                od.DesktopCoordinates.right == desktopRect.right && od.DesktopCoordinates.bottom == desktopRect.bottom)
                return (int)ai;
            RECT inter;
            if (IntersectRect(&inter, &od.DesktopCoordinates, &desktopRect)) {
                if (fallback < 0) fallback = (int)ai;
            }
        }
    }
    return fallback;
}

std::wstring D3D11Device::AdapterName(int index) const {
    if (index < 0 || index >= (int)adapters_.size()) return L"?";
    return adapters_[index].name;
}

} // namespace sw
