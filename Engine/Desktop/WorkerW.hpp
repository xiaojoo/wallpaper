#pragma once
// Engine/Desktop/WorkerW.hpp - finds the window layer that sits *behind* the desktop icons.
#include "Engine/Core/Platform.hpp"

namespace sw {

enum class HostMethod { None, WorkerWBelowIcons, ProgmanChildBelowIcons, OwnTopLevel };

struct DesktopHost {
    HWND parent = nullptr;      // what our per-monitor windows are parented to
    HWND progman = nullptr;
    HWND defView = nullptr;     // SHELLDLL_DefView (the icon surface)
    HWND iconHost = nullptr;    // the top-level window that owns defView
    HostMethod method = HostMethod::None;
    POINT origin{};             // parent client (0,0) in desktop pixels
    SIZE client{};
    std::string detail;

    const char* methodName() const;
    std::string describe() const;
    // Desktop pixel rect -> parent client rect.
    Rect ToClient(const Rect& desktopPx) const;
};

class WorkerW {
public:
    // Asks explorer to build the wallpaper layer, then locates it. Reversible: nothing of ours
    // survives process exit, and the icon layer is never reparented by us.
    bool Attach(std::string& error);
    // --window development mode: skip the desktop entirely and use plain top-level windows.
    void SetOverrideHost(const DesktopHost& h) { host_ = h; }
    const DesktopHost& host() const { return host_; }
    void Recheck();

    static HWND FindProgman();
    static HWND FindTopLevelByClass(const wchar_t* className);
    static std::string ClassOf(HWND h);

private:
    bool LocateLayer();
    DesktopHost host_;
};

} // namespace sw
