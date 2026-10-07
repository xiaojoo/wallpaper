#pragma once
// Engine/Desktop/TrayTransparency.hpp - the taskbar's opacity, driven from outside explorer.
#include <windows.h>

namespace sw {

// Windows offers one bit here (Personalize\EnableTransparency). Measured on 26300 with two solid
// fixtures 255 steps apart, the bit is worth this much of the wallpaper reaching the bar:
//     on  = slope 0.149 R / 0.163 B   off = slope 0.000 (a flat 238 grey, sd 0.0)
// The three ways an outside process can go past those two states, measured with tools/tray-slope.sh:
//   DWMWA_SYSTEMBACKDROP_TYPE  no effect at all - slope unchanged to the last digit at 1, 2 and 3
//   ACCENT_POLICY              only ever lowers the slope (0.133 at alpha 0 down to 0.022 at 255):
//                              it paints a layer over the bar instead of letting the desktop through
//   WS_EX_LAYERED + LWA_ALPHA  continuous and monotonic, 0.795 at 64, 0.578 at 128, 0.365 at 192,
//                              1.014 at 0 - and it fits slope = (A/255)*0.15 + (1 - A/255) within
//                              0.01, which is why this class exists and the other two do not.
// The price is that the whole window fades, clock and icons included: the contrast of the tray
// segment drops from sd 43.5 to 22.4 at A=128. Only the background would have needed the backdrop
// API, and that one does nothing to the bar.
class TrayTransparency {
public:
    // Percent of transparency, 0 = leave the bar to Windows' own material. Idempotent, so the
    // shell watchdog calls it again every second: explorer drops the style when it rebuilds the
    // bar (monitor change, resolution change, its own restart), and re-finding the windows every
    // time is how that rebuild gets noticed.
    void Apply(int pct);

    // 1..255, never 0. A=0 is the one value that must not be written: with a fully transparent
    // layered window the bar stops owning its own pixels - WindowFromPoint over the taskband
    // returned SysListView32 at A=0 and Shell_TrayWnd at every value from 1 up - so the user would
    // lose the taskbar until the next call puts it back.
    static int ToAlpha(int pct);

    int applied() const { return applied_; }

private:
    int applied_ = -1; // what the shell last was told, for logging only
};

} // namespace sw
