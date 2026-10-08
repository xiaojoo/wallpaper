#pragma once
// Engine/Desktop/DesktopWindow.hpp - one child window per monitor, hosted behind the desktop icons.
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Desktop/WorkerW.hpp"
#include "Engine/Graphics/D3D11Renderer.hpp"

namespace sw {

class DesktopWindow {
public:
    bool Create(const DesktopHost& host, const MonitorInfo& monitor, std::string& error);
    void Destroy();

    // The HWND is ours to clean up. `BuildSlots` clears slots_ on every 重载壁纸 and on every monitor
    // change, and without this each of those left a visible full-screen child of WorkerW behind
    // (measured 2026-10-08: 3 -> 4 -> 5 of our windows after successive reloads) with the live one
    // underneath, so the desktop looked like it had dropped out to the plain Windows wallpaper while
    // the engine went on presenting at 48 fps into a window nobody could see.
    ~DesktopWindow() { Destroy(); }

    HWND hwnd() const { return hwnd_; }
    const MonitorInfo& monitor() const { return monitor_; }
    RenderSurface& surface() { return surface_; }

    bool EnsureSize(ID3D11Device* dev, IDCompositionDevice* comp, std::string& error);
    // Re-parents after explorer rebuilt the desktop layer (WorkerW can be destroyed when
    // the view changes, and a new one appears with a different handle).
    bool Rebind(const DesktopHost& host, std::string& error);
    // Measured offset between where we asked to be and where the window actually is.
    std::string VerifyPlacement(std::string& warnOut) const;
    void Show(bool visible);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    HWND hwnd_ = nullptr;
    MonitorInfo monitor_{};
    RenderSurface surface_;
    bool topLevel_ = false;
};

} // namespace sw
