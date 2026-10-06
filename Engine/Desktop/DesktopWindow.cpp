#include "Engine/Desktop/DesktopWindow.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "win";

namespace {
constexpr const wchar_t* kClassName = L"SmartWallpaperDesktop";
ATOM g_classAtom = 0;

bool EnsureClass() {
    if (g_classAtom) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = &DesktopWindow::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    g_classAtom = RegisterClassExW(&wc);
    if (!g_classAtom) {
        Error(MOD, "RegisterClassExW failed: {}", HResultToString(HRESULT_FROM_WIN32(GetLastError())));
        return false;
    }
    return true;
}
} // namespace

LRESULT CALLBACK DesktopWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1; // the GPU fills every pixel; erasing would flicker
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_WINDOWPOSCHANGED:
            return 0; // size is reconciled by EnsureSize in the render loop
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

bool DesktopWindow::Create(const DesktopHost& host, const MonitorInfo& monitor, std::string& error) {
    if (!EnsureClass()) { error = "window class registration failed"; return false; }
    monitor_ = monitor;
    topLevel_ = host.method == HostMethod::OwnTopLevel;

    DWORD style = topLevel_ ? (WS_POPUP | WS_DISABLED) : (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    DWORD ex = WS_EX_NOACTIVATE;
    HWND parent = topLevel_ ? nullptr : host.parent;
    Rect c = host.ToClient(monitor.px);

    hwnd_ = CreateWindowExW(ex, kClassName, L"SmartWallpaper", style, c.x, c.y, c.w, c.h, parent, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        error = std::format("CreateWindowExW for {} failed: {}", monitor.tag,
                            HResultToString(HRESULT_FROM_WIN32(GetLastError())));
        return false;
    }
    // Sibling position inside the parent decides whether the icons and the shell's own wallpaper
    // layer end up above us. On build 26300 the wallpaper WorkerW is a *child* of Progman, so
    // HWND_BOTTOM would bury us under it; inserting directly below DefView puts us over the
    // shell's wallpaper but under the icon layer.
    HWND insertAfter = topLevel_ ? HWND_BOTTOM : (host.defView ? host.defView : HWND_BOTTOM);
    SetWindowPos(hwnd_, insertAfter, c.x, c.y, c.w, c.h,
                 SWP_NOACTIVATE | (topLevel_ ? SWP_NOMOVE | SWP_NOSIZE : 0));
    ShowWindow(hwnd_, SW_SHOWNA);
    Info(MOD, "{} window 0x{:X} at client ({},{}) {}x{} parent=0x{:X}", monitor.tag, (uintptr_t)hwnd_, c.x, c.y, c.w,
         c.h, (uintptr_t)parent);
    return true;
}

bool DesktopWindow::Rebind(const DesktopHost& host, std::string& error) {
    if (!hwnd_) { error = "no window"; return false; }
    if (topLevel_) return true;
    HWND want = host.parent;
    if (GetParent(hwnd_) == want) return true;
    Rect c = host.ToClient(monitor_.px);
    if (!SetParent(hwnd_, want)) {
        error = "SetParent failed: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    SetWindowPos(hwnd_, HWND_BOTTOM, c.x, c.y, c.w, c.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Info(MOD, "{} rebound to layer 0x{:X} at ({},{}) {}x{}", monitor_.tag, (uintptr_t)want, c.x, c.y, c.w, c.h);
    return true;
}

bool DesktopWindow::EnsureSize(ID3D11Device* dev, IDCompositionDevice* comp, std::string& error) {
    if (!hwnd_) { error = "no window"; return false; }
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    UINT w = (UINT)(cr.right - cr.left), h = (UINT)(cr.bottom - cr.top);
    if (!w || !h) { error = std::format("{} has an empty client area", monitor_.tag); return false; }
    if (surface_.valid() && surface_.width() == w && surface_.height() == h) return true;
    if (!surface_.valid()) return surface_.Init(dev, comp, hwnd_, w, h, error);
    return surface_.Resize(w, h, error);
}

std::string DesktopWindow::VerifyPlacement(std::string& warnOut) const {
    if (!hwnd_) return "no window";
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    long x = wr.left, y = wr.top, w = wr.right - wr.left, h = wr.bottom - wr.top;
    long dx = x - monitor_.px.x, dy = y - monitor_.px.y;
    long dw = w - monitor_.px.w, dh = h - monitor_.px.h;
    std::string got = std::format("({},{}) {}x{}", x, y, w, h);
    if (dx || dy || dw || dh) {
        warnOut = std::format("{} wants {} but measures {} (off by {},{}) size delta {},{}", monitor_.tag,
                              std::format("({},{}) {}x{}", monitor_.px.x, monitor_.px.y, monitor_.px.w,
                                          monitor_.px.h),
                              got, dx, dy, dw, dh);
    }
    return got;
}

void DesktopWindow::Show(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
}

void DesktopWindow::Destroy() {
    if (hwnd_) {
        surface_.Release();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

} // namespace sw
