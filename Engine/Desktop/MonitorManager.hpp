#pragma once
// Engine/Desktop/MonitorManager.hpp - physical-pixel monitor geometry, DPI and refresh rate.
#include "Engine/Core/Platform.hpp"

namespace sw {

class D3D11Device;

struct MonitorInfo {
    std::wstring device;    // \\.\DISPLAY1
    std::string tag;        // short name used in logs, config and the IPC protocol
    int index = -1;         // left-to-right order, matches the digit in tag
    Rect px;                // full bounds, physical pixels, desktop coordinates
    Rect work;              // working area (excludes taskbar), physical pixels
    UINT dpi = 96;
    float scale = 1.f;
    UINT refreshHz = 60;
    bool primary = false;
    int adapterIndex = -1;  // index into D3D11Device::adapters(), -1 when unknown

    std::string describe() const;
};

class MonitorManager {
public:
    // Enumerates from Win32, then tags each monitor with the DXGI adapter that drives it.
    void Refresh(const D3D11Device* dev);

    const std::vector<MonitorInfo>& list() const { return monitors_; }
    size_t count() const { return monitors_.size(); }
    const MonitorInfo* byTag(std::string_view tag) const;
    const MonitorInfo* byIndex(size_t i) const;
    Rect virtualBounds() const;

private:
    std::vector<MonitorInfo> monitors_;
};

void MakeProcessPerMonitorAware();

} // namespace sw
