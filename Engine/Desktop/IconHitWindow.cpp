#include "Engine/Desktop/IconHitWindow.hpp"

#include "Engine/Core/Log.hpp"
#include "Engine/Desktop/WorkerW.hpp"

#include <format>
#include <cstdlib>

namespace sw {

static constexpr const char* MOD = "orbit";

namespace {

constexpr wchar_t kClassName[] = L"SmartWallpaper.IconHit";
constexpr wchar_t kWindowName[] = L"SmartWallpaper shortcut ring - click layer";

// How far a box may sit from where the picture currently is before it is moved, and the step its size
// is rounded to. See the header: this is what keeps the ring from issuing sixteen SetWindowPos calls
// every frame.
constexpr int kDriftTol = 3, kSizeStep = 8;

} // namespace

POINT IconHitWindow::MessagePoint(LPARAM l) {
    // The SDK's own signed decoder, because LOWORD/HIWORD are unsigned.
    return POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
}

int IconHitWindow::SlotAt(POINT desktopPx) const {
    for (const auto& r : rects_)
        if (desktopPx.x >= r.px.left && desktopPx.x < r.px.right &&
            desktopPx.y >= r.px.top && desktopPx.y < r.px.bottom)
            return r.slot;
    return -1;
}

int IconHitWindow::countLive() const {
    int n = 0;
    for (HWND h : win_) if (h) ++n;
    return n;
}

RECT IconHitWindow::ToClient(const RECT& r) const {
    POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
    MapWindowPoints(HWND_DESKTOP, parent_, &tl, 1);
    MapWindowPoints(HWND_DESKTOP, parent_, &br, 1);
    return RECT{ tl.x, tl.y, br.x, br.y };
}

LRESULT CALLBACK IconHitWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<IconHitWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(
                              reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        return 0;

    // The whole window is the icon, which is the point of one window per icon: there is no region of
    // this window that has to hand a click back, because there is no such region.
    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;

    case WM_LBUTTONDOWN:
        // Consumed rather than passed to DefWindowProc: a child window taking a click must not also
        // try to take the foreground with it.
        return 0;

    case WM_LBUTTONUP: {
        if (!self) return 0;
        const int slot = static_cast<int>(GetWindowLongPtrW(h, GWLP_ID));
        Info(MOD, "orbit layer: button up on slot {}", slot);
        if (slot >= 0 && slot < kMaxSlots && self->onClick_) self->onClick_(slot);
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

void IconHitWindow::Ensure(int slot, const RECT& want) {
    // Round the box onto a grid so a picture that creeps along by a pixel a frame does not resize the
    // window every frame, and so the size only changes at the ends of the depth range.
    const int cx = (want.left + want.right) / 2, cy = (want.top + want.bottom) / 2;
    const int wd = ((want.right - want.left) + kSizeStep / 2) & ~(kSizeStep - 1);
    const int ht = ((want.bottom - want.top) + kSizeStep / 2) & ~(kSizeStep - 1);
    const RECT q{ cx - wd / 2, cy - ht / 2, cx - wd / 2 + wd, cy - ht / 2 + ht };

    HWND& h = win_[slot];
    if (!h) {
        const RECT c = ToClient(q);
        h = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClassName, kWindowName, WS_CHILD,
                            c.left, c.top, c.right - c.left, c.bottom - c.top, parent_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(slot)),
                            GetModuleHandleW(nullptr), this);
        if (!h) {
            Warn(MOD, "orbit click box {} could not be created: {}", slot, GetLastError());
            return;
        }
        // Over the icon layer and nowhere higher: that is what leaves every application window, and
        // the taskbar, able to keep the clicks that land on them.
        SetWindowPos(h, above_ ? GetWindow(above_, GW_HWNDPREV) : HWND_TOP,
                     c.left, c.top, c.right - c.left, c.bottom - c.top, SWP_NOACTIVATE);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        return;
    }
    RECT cur{};
    GetWindowRect(h, &cur);
    if (std::abs(cur.left - q.left) <= kDriftTol && std::abs(cur.top - q.top) <= kDriftTol &&
        std::abs(cur.right - q.right) <= kDriftTol && std::abs(cur.bottom - q.bottom) <= kDriftTol)
        return;
    const RECT c = ToClient(q);
    MoveWindow(h, c.left, c.top, c.right - c.left, c.bottom - c.top, TRUE);
}

void IconHitWindow::TakeDown(int slot) {
    if (!win_[slot]) return;
    DestroyWindow(win_[slot]);
    win_[slot] = nullptr;
}

void IconHitWindow::SetRects(std::vector<IconHitRect> rects) {
    rects_ = std::move(rects);
    bool seen[kMaxSlots] = {};
    for (const auto& r : rects_)
        if (r.slot >= 0 && r.slot < kMaxSlots) {
            seen[r.slot] = true;
            Ensure(r.slot, r.px);
        }
    for (int k = 0; k < kMaxSlots; ++k) if (!seen[k]) TakeDown(k);
}

bool IconHitWindow::Create(HINSTANCE inst, HWND parent, HWND above, const RECT& area,
                           ClickFn onClick, std::string& error) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &IconHitWindow::WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = std::format("RegisterClassExW for the shortcut ring click layer failed: {}",
                                GetLastError());
            return false;
        }
        registered = true;
    }
    if (!parent || !above) {
        error = "no icon layer to click over (the desktop host was not found)";
        return false;
    }
    Destroy();
    parent_ = parent;
    above_ = above;
    onClick_ = std::move(onClick);
    Info(MOD, "orbit click layer ready inside {} over {}, monitor {}x{}+{}+{} - boxes appear as the "
              "ring draws them",
         WorkerW::ClassOf(parent), WorkerW::ClassOf(above),
         area.right - area.left, area.bottom - area.top, area.left, area.top);
    return true;
}

void IconHitWindow::Destroy() {
    for (int k = 0; k < kMaxSlots; ++k) TakeDown(k);
    rects_.clear();
    onClick_ = nullptr;
    parent_ = nullptr;
    above_ = nullptr;
}

} // namespace sw
