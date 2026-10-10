#include "Engine/Desktop/HudDragWindow.hpp"

#include <format>

namespace sw {
namespace {

constexpr wchar_t kClassName[] = L"SmartWallpaper.HudDrag";
// BGRA in a DWORD, i.e. 0xAARRGGBB. The interior is alpha 1: invisible over the card, but a pixel
// the mouse can land on. The ring is the only thing the eye sees.
constexpr uint32_t kFill = 0x01000000u;
constexpr uint32_t kInk = 0xFF00E2ECu;      // the card's own cyan
constexpr int kDashOn = 4, kDashOff = 3;    // 7 px period, wide enough to read as a dash at 4K

} // namespace

void HudDragWindow::Paint(int w, int h) {
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) bits_[y * w + x] = kFill;
    // Walk the ring once so the dashes run continuously around the corners instead of restarting on
    // each side, which reads as four separate lines.
    int per = 0;
    const int x0 = 1, y0 = 1, x1 = w - 2, y1 = h - 2;
    const auto dot = [&](int x, int y) {
        if (per % (kDashOn + kDashOff) < kDashOn) bits_[y * w + x] = kInk;
        ++per;
    };
    for (int x = x0; x <= x1; ++x) dot(x, y0);
    for (int y = y0 + 1; y <= y1; ++y) dot(x1, y);
    for (int x = x1 - 1; x >= x0; --x) dot(x, y1);
    for (int y = y1 - 1; y > y0; --y) dot(x0, y);
}

void HudDragWindow::Apply(const RECT& screenRect) {
    if (!hwnd_) return;
    const int w = screenRect.right - screenRect.left, h = screenRect.bottom - screenRect.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    if (!memDc_) memDc_ = CreateCompatibleDC(screen);
    if (w != w_ || h != h_ || !dib_) {
        if (dib_) { DeleteObject(dib_); dib_ = nullptr; bits_ = nullptr; }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;           // top-down, so bits_ is in screen order
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                reinterpret_cast<void**>(&bits_), nullptr, 0);
        if (dib_) { SelectObject(memDc_, dib_); Paint(w, h); w_ = w; h_ = h; }
    }
    if (!dib_) { ReleaseDC(nullptr, screen); return; }
    POINT dst{ screenRect.left, screenRect.top };
    SIZE sz{ w_, h_ };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;             // the per-pixel alpha is the whole story
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd_, screen, &dst, &sz, memDc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK HudDragWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<HudDragWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(
                              reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        return 0;

    // The whole rectangle is the handle, and after the alpha change the whole rectangle really is
    // this window's, so hit-testing has nothing to fall through to.
    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;

    case WM_LBUTTONDOWN:
        if (!self) return 0;
        self->BeginDrag();
        return 0;

    case WM_MOUSEMOVE:
        if (!self || !self->dragging_ || GetCapture() != h) return 0;
        self->FollowPointer(false);
        return 0;

    case WM_LBUTTONUP: {
        if (!self) return 0;
        const bool was = GetCapture() == h;
        self->dragging_ = false;
        if (was) ReleaseCapture();
        if (was) self->FollowPointer(true);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void HudDragWindow::BeginDrag() {
    RECT r{};
    GetWindowRect(hwnd_, &r);
    POINT ap{};
    GetCursorPos(&ap);
    // The grab offset is measured against the absolute pointer, not the message's client coordinate:
    // a client coordinate is a function of where this window already is, and this window is about to
    // move. That round trip through a moving frame is what made the card jump.
    offX_ = ap.x - r.left;
    offY_ = ap.y - r.top;
    dragging_ = true;
    SetCapture(hwnd_);
}

void HudDragWindow::FollowPointer(bool commit) {
    if (!hwnd_ || !onMove_) return;
    POINT ap{};
    GetCursorPos(&ap);
    const float cardH = cardH_ > 1.f ? cardH_ : 1.f;
    // Where the pointer wants it, in the card's own units - unclamped, because the engine is the one
    // that knows what "on the screen, clear of the taskbar" means for this monitor.
    const float x = float(ap.x - offX_ - work_.left) / cardH;
    const float y = float(ap.y - offY_ - work_.top) / cardH;
    const RECT want = onMove_(x, y, commit);
    // A drop takes this window down inside that call, so nothing here may touch the handle afterwards.
    if (commit) return;
    RECT now{};
    GetWindowRect(hwnd_, &now);
    if (now.left == want.left && now.top == want.top) return;
    // Moved here rather than by the frame loop: the outline has to be under the pointer in the same
    // message that decided where it goes, or the two of them take turns and the eye sees a jitter.
    Apply(want);
}

bool HudDragWindow::Create(HINSTANCE inst, const RECT& screenRect, const RECT& work,
                           MoveFn onMove, std::string& error) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &HudDragWindow::WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = std::format("RegisterClassExW for the status card drag layer failed: {}",
                                GetLastError());
            return false;
        }
        registered = true;
    }
    Destroy();
    work_ = work;
    cardH_ = float(work.bottom - work.top);
    onMove_ = std::move(onMove);
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName,
                            L"SmartWallpaper status card - drag to move", WS_POPUP,
                            screenRect.left, screenRect.top,
                            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) {
        error = std::format("CreateWindowExW for the status card drag layer failed: {}",
                            GetLastError());
        onMove_ = nullptr;
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    Apply(screenRect);
    return true;
}

void HudDragWindow::Move(const RECT& screenRect) { Apply(screenRect); }

void HudDragWindow::Destroy() {
    if (!hwnd_) return;
    const HWND h = hwnd_;
    hwnd_ = nullptr;                 // first, so a re-entrant WM_DESTROY cannot see a live handle
    dragging_ = false;
    if (GetCapture() == h) ReleaseCapture();
    DestroyWindow(h);
    onMove_ = nullptr;
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; bits_ = nullptr; }
    if (memDc_) { DeleteDC(memDc_); memDc_ = nullptr; }
    w_ = h_ = 0;
}

} // namespace sw
