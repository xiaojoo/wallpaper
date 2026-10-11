#pragma once
// Engine/Desktop/IconHitWindow.hpp - the invisible boxes that make the shortcut ring clickable.
//
// The ring is drawn by the wallpaper's shader, into a surface that sits *below* the desktop's own icon
// layer, so it owns no click: stage 0 measured 53 of 53 desktop icon points landing on SysListView32,
// with the wallpaper on and off giving the identical answer. Something has to stand between the mouse
// and the icons, and this is that something.
//
// One window per visible icon, each exactly the size of its own picture - not one window over the
// monitor that answers HTTRANSPARENT outside the icons. That was the first design and it was measured
// wrong: with a full-monitor layer armed, a click on empty desktop at a point outside every icon box
// did not reach the shell (the icon selection the probe had made through UI Automation stayed set),
// while the same click with the layer down cleared it. HTTRANSPARENT is documented as "ask the window
// underneath" and works for top-level windows; on a child of Progman on this build it does not
// hand the click back. So the fallthrough is not a hit-test answer any more, it is geometry: outside
// these boxes this code owns no pixels at all, and WindowFromPoint says SysListView32 again.
//
// Where the windows sit is the other half of the design. They are children of the window that owns
// SHELLDLL_DefView, ordered directly above DefView and nowhere higher, so every application window and
// the taskbar stays above them and cannot lose a click to a ring icon that happens to overlap it.
//
// They carry WS_EX_NOREDIRECTIONBITMAP because they must paint nothing: a plain child would put a
// rectangle of its own over the wallpaper. That part is measured too - the patch under one of these
// windows differs between the layer being up and down by no more than the same patch differs between
// two frames of the same state, whereas a window that painted would read as the whole patch.
//
// Moving them is quantised: a box only moves when its picture has drifted past 3 px, and its size is
// rounded to 8 px. The front of the ring travels about thirty pixels a second, so this is a handful of
// SetWindowPos calls a second rather than sixteen per frame, at the cost of a click box that can trail
// the picture by up to 4 px on a 106..178 px target.

#include "Engine/Core/Platform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sw {

// One ring slot the pointer can be put on. `slot` is the ring's own index, which is also the atlas
// cell and the row of uOrbitIcon the picture came from - the three have to stay one number, or a
// click launches somebody else's program.
struct IconHitRect {
    RECT px{};      // desktop pixels
    int slot = -1;
};

class IconHitWindow {
public:
    // One box per ring slot, so this is the ring's own ceiling - kMaxOrbitIcons in
    // Engine/Graphics/D3D11Device.hpp, which Application.cpp asserts against it rather than trusting
    // two files to keep saying 16.
    static constexpr int kMaxSlots = 16;

    // Called on the thread that pumps messages, with the slot the button came up on.
    using ClickFn = std::function<void(int slot)>;

    ~IconHitWindow() { Destroy(); }

    // `parent` is the window that owns the icon layer and `above` is the icon layer itself: every box
    // this creates goes immediately over it. `area` is the monitor in desktop pixels, which is only
    // used to say where the layer is in the log - no window covers it.
    bool Create(HINSTANCE inst, HWND parent, HWND above, const RECT& area,
                ClickFn onClick, std::string& error);
    void Destroy();
    bool active() const { return parent_ != nullptr; }

    // The rectangles the ring is currently drawn into, in desktop pixels, from the same maths that
    // fills the constant buffer. A slot the centre object hides is simply not in here, and the window
    // it had is taken down.
    void SetRects(std::vector<IconHitRect> rects);

    // The slot under these desktop pixels, -1 for none. Polled by the frame loop rather than pushed by
    // mouse messages, because the ring moves under a pointer that is not.
    int SlotAt(POINT desktopPx) const;
    int countLive() const;

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    // The LPARAM of a mouse message is signed, and plain LOWORD/HIWORD are not: a point on a monitor
    // sitting left of the primary decodes to a wrapped positive otherwise.
    static POINT MessagePoint(LPARAM l);

    void Ensure(int slot, const RECT& desktopPx);
    void TakeDown(int slot);
    // Desktop pixels -> the parent's client pixels, which is what a child window is positioned in.
    RECT ToClient(const RECT& desktopPx) const;

    HWND parent_ = nullptr;
    HWND above_ = nullptr;         // DefView: each box is ordered directly over this
    HWND win_[kMaxSlots] = {};     // one per ring slot, null while that slot is hidden or absent
    std::vector<IconHitRect> rects_;
    ClickFn onClick_;
};

} // namespace sw
