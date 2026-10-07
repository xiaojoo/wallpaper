#include "Engine/Desktop/TrayTransparency.hpp"
#include "Engine/Core/Log.hpp"
#include <vector>

namespace sw {
static constexpr const char* MOD = "trayfx";

namespace {

BOOL CALLBACK CollectTrays(HWND h, LPARAM l) {
    wchar_t cls[64]{};
    if (GetClassNameW(h, cls, 64) == 0) return TRUE;
    const std::wstring c(cls);
    // One window per screen: the primary bar plus a secondary bar on every other monitor.
    if (c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd")
        reinterpret_cast<std::vector<HWND>*>(l)->push_back(h);
    return TRUE;
}

} // namespace

int TrayTransparency::ToAlpha(int pct) {
    if (pct <= 0) return 255;
    if (pct >= 100) return 1;   // never 0: that value hands the bar's pixels to the desktop
    return 255 - pct * 254 / 100;
}

void TrayTransparency::Apply(int pct) {
    std::vector<HWND> trays;
    EnumWindows(CollectTrays, reinterpret_cast<LPARAM>(&trays));
    if (trays.empty()) {
        if (applied_ > 0) Warn(MOD, "no taskbar window found");
        return;
    }
    const int alpha = ToAlpha(pct);
    // Only a style we believed was already on can have been lost by the shell. The first call after
    // the user turns this on adds the same bit, and calling that a rebuild would be a lie the log
    // tells every time somebody moves the slider off zero.
    const bool believed = applied_ == pct && pct > 0;
    int repaired = 0;
    for (HWND h : trays) {
        const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (pct <= 0) {
            if (ex & WS_EX_LAYERED) {
                SetWindowLongW(h, GWL_EXSTYLE, ex & ~LONG(WS_EX_LAYERED));
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            continue;
        }
        if (!(ex & WS_EX_LAYERED)) {
            SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            ++repaired;  // the shell rebuilt this bar, so its alpha was lost
        }
        SetLayeredWindowAttributes(h, 0, BYTE(alpha), LWA_ALPHA);
    }
    if (pct <= 0) {
        if (applied_ > 0) Info(MOD, "taskbar transparency off, style removed");
        applied_ = 0;
        return;
    }
    if (repaired && believed)
        Info(MOD, "shell rebuilt the taskbar, alpha {} put back on {} window(s)", alpha, trays.size());
    else if (repaired)
        Info(MOD, "taskbar transparency {}%, alpha {} set on {} window(s)", pct, alpha, trays.size());
    applied_ = pct;
}

} // namespace sw
