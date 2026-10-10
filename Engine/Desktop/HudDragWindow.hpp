#pragma once
// Engine/Desktop/HudDragWindow.hpp - the invisible rectangle that lets the status card be dragged.
//
// The card itself is not a window: it is composited into the wallpaper's surface, which is a child of
// WorkerW and sits *below* the desktop's icon layer, so it never receives a mouse message - the icon
// layer answers every click on the desktop. This is the piece that can be clicked: a top-level window
// the same size as the card, created only while the user has switched "allow dragging" on, and
// destroyed the moment the card is dropped.
//
// It carries a per-pixel alpha rather than a colour key, and that is the whole point: with a colour
// key the keyed pixels do not just look transparent, they are transparent *to the mouse*, so only the
// 1 px dashed ring could be grabbed and pressing anywhere on the card itself fell through to the
// desktop. Here the interior is alpha 1 - one part in 255 over the card, which nothing can see - so
// every pixel of the card is a handle, and the ring stays at full strength.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace sw {

class HudDragWindow {
public:
    // Called for every mouse move while the button is down, and once more on release. `x, y` are where
    // the pointer wants the card's top-left, in screen heights from the monitor's top-left - the units
    // the card is laid out in - and `commit` says whether this is the drop. The return value is the
    // rectangle the card may actually occupy, in desktop pixels: the engine owns the clamping, so the
    // outline follows the card into a corner instead of running past it.
    using MoveFn = std::function<RECT(float x, float y, bool commit)>;

    ~HudDragWindow() { Destroy(); }

    // `screenRect` is where the card is now, in desktop pixels; `work` is that monitor's full pixel
    // rect, which is what converts a pointer back into the card's own units.
    bool Create(HINSTANCE inst, const RECT& screenRect, const RECT& work,
                MoveFn onMove, std::string& error);
    void Destroy();
    bool active() const { return hwnd_ != nullptr; }
    // The button is down inside this window: while it is, this window owns its own position, because
    // a second mover reading last frame's rectangle is what made the card jitter under the pointer.
    bool dragging() const { return dragging_; }
    // Keeps the grab rectangle under the card while the card moves for another reason (the scale
    // slider, a different border) - otherwise the outline drifts off what it is supposed to be for.
    void Move(const RECT& screenRect);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    void BeginDrag();
    void FollowPointer(bool commit);
    void Apply(const RECT& screenRect);
    void Paint(int w, int h);

    HWND hwnd_ = nullptr;
    RECT work_{};             // the monitor, in desktop pixels
    float cardH_ = 1.f;       // one screen height, in pixels: the divisor for every conversion
    int offX_ = 0, offY_ = 0; // the pointer's offset from the card's top-left, in pixels
    bool dragging_ = false;
    MoveFn onMove_;

    HDC memDc_ = nullptr;     // the DC UpdateLayeredWindow reads the picture from
    HBITMAP dib_ = nullptr;
    uint32_t* bits_ = nullptr;
    int w_ = 0, h_ = 0;       // what that picture currently is
};

} // namespace sw
