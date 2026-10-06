#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Core/Log.hpp"

#include <algorithm>
#include <ShellScalingApi.h>

namespace sw {
static constexpr const char* MOD = "mon";

void MakeProcessPerMonitorAware() {
    // Must happen before any window exists: a DPI-unaware process gets virtualised coordinates,
    // which would silently break per-monitor pixel math.
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        Info(MOD, "per-monitor-aware-v2 set");
        return;
    }
    if (SetProcessDPIAware()) Warn(MOD, "falling back to system DPI awareness");
    else Warn(MOD, "no DPI awareness applied - monitor pixels may be virtualised");
}

std::string MonitorInfo::describe() const {
    return std::format("{} {} px=({},{}) {}x{} work={}x{} dpi={} scale={:.2f} refresh={}Hz{} adapter={}", tag,
                       ToUtf8(device), px.x, px.y, px.w, px.h, work.w, work.h, dpi, scale, refreshHz,
                       primary ? " primary" : "", adapterIndex);
}

namespace {

struct Collect {
    std::vector<MonitorInfo> out;
};

BOOL CALLBACK OnMonitor(HMONITOR h, HDC, LPRECT clip, LPARAM lparam) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi)) return TRUE;
    auto* c = reinterpret_cast<Collect*>(lparam);
    MonitorInfo m;
    m.device = mi.szDevice;
    m.px = Rect{clip->left, clip->top, clip->right - clip->left, clip->bottom - clip->top};
    m.work = Rect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left,
                  mi.rcWork.bottom - mi.rcWork.top};
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT ax = 96, ay = 96;
    if (SUCCEEDED(GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &ax, &ay))) {
        m.dpi = ax;
        m.scale = ax / 96.f;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 0)
        m.refreshHz = dm.dmDisplayFrequency;

    c->out.push_back(std::move(m));
    return TRUE;
}

} // namespace

void MonitorManager::Refresh(const D3D11Device* dev) {
    Collect c;
    EnumDisplayMonitors(nullptr, nullptr, OnMonitor, reinterpret_cast<LPARAM>(&c));
    monitors_ = std::move(c.out);
    std::sort(monitors_.begin(), monitors_.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        return a.px.x != b.px.x ? a.px.x < b.px.x : a.px.y < b.px.y;
    });
    int i = 0;
    for (auto& m : monitors_) {
        m.tag = "M" + std::to_string(i);
        m.index = i++;
        if (dev) m.adapterIndex = dev->AdapterForMonitorRect(ToRECT(m.px));
    }
    for (auto& m : monitors_) Info(MOD, "{}", m.describe());
    if (monitors_.empty()) Warn(MOD, "no monitors enumerated");
}

const MonitorInfo* MonitorManager::byTag(std::string_view tag) const {
    for (auto& m : monitors_)
        if (m.tag == tag) return &m;
    return nullptr;
}

const MonitorInfo* MonitorManager::byIndex(size_t i) const {
    return i < monitors_.size() ? &monitors_[i] : nullptr;
}

Rect MonitorManager::virtualBounds() const {
    if (monitors_.empty()) return {};
    long l = monitors_.front().px.x, t = monitors_.front().px.y;
    long r = l + monitors_.front().px.w, b = t + monitors_.front().px.h;
    for (auto& m : monitors_) {
        l = std::min(l, m.px.x);
        t = std::min(t, m.px.y);
        r = std::max(r, m.px.x + m.px.w);
        b = std::max(b, m.px.y + m.px.h);
    }
    return Rect{l, t, r - l, b - t};
}

} // namespace sw
