// trayalpha.cpp - probe whether an outside process can make the Windows taskbar continuously
// translucent. It only touches Shell_TrayWnd, prints what each call returned, and undoes itself.
//
// Windows itself exposes transparency as one bit (Personalize\EnableTransparency), so this file
// tries the three undocumented/semi-documented knobs that could turn that bit into a range:
//   accent    SetWindowCompositionAttribute + ACCENT_POLICY, whose gradient colour carries an alpha
//   layered   WS_EX_LAYERED + SetLayeredWindowAttributes, a uniform alpha over the whole window
//   backdrop  DWMWA_SYSTEMBACKDROP_TYPE, documented in Win11 but a choice of material, not a level
//
// Which of these actually blend with what is behind the taskbar is measured from real screen pixels
// by tools/tray-band.ps1, not judged by eye from here.
//
// Build: tools/trayalpha-build.bat -> build/trayalpha.exe
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr int WCA_ACCENT_POLICY = 19;

struct AccentPolicy {
    LONG accentState;
    LONG accentFlags;
    DWORD gradientColor;
    LONG accentAnimationId;
};

struct WincompattrData {
    INT attribute;
    PVOID data;
    SIZE_T size;
};

// Exported by user32 on Windows 10 and later, declared in no public header.
typedef BOOL(WINAPI *SetWindowCompositionAttribute_t)(HWND, WincompattrData *);

HWND tray() {
    HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!h) std::fwprintf(stderr, L"no Shell_TrayWnd\n");
    return h;
}

void printRect(const wchar_t *label, HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    std::wprintf(L"%s hwnd=%p rect=%ld,%ld %ldx%ld\n", label, (void *)h, r.left, r.top,
                 r.right - r.left, r.bottom - r.top);
}

std::string regTransparency() {
    HKEY k{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &k) != ERROR_SUCCESS)
        return "unreadable";
    DWORD v = 0, type = 0, cb = sizeof v;
    const LONG rc = RegQueryValueExA(k, "EnableTransparency", nullptr, &type, (LPBYTE)&v, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return "missing";
    return std::to_string(v);
}

void showState(HWND h) {
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    std::printf("exstyle=0x%08lX layered=%s\n", ex, (ex & WS_EX_LAYERED) ? "yes" : "no");
    std::printf("EnableTransparency=%s\n", regTransparency().c_str());
}

int applyAccent(HWND h, LONG state, LONG flags, int a, int r, int g, int b, bool bgra) {
    const DWORD color = bgra ? (DWORD((a & 255) << 24) | DWORD((b & 255) << 16) |
                               DWORD((g & 255) << 8) | DWORD(r & 255))
                             : (DWORD((a & 255) << 24) | DWORD((r & 255) << 16) |
                                DWORD((g & 255) << 8) | DWORD(b & 255));
    AccentPolicy ap{state, flags, color, 0};
    WincompattrData wd{WCA_ACCENT_POLICY, &ap, sizeof ap};
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto fn = (SetWindowCompositionAttribute_t)GetProcAddress(u, "SetWindowCompositionAttribute");
    if (!fn) {
        std::printf("SetWindowCompositionAttribute not present in user32\n");
        return 1;
    }
    const BOOL ok = fn(h, &wd);
    std::printf("accent state=%ld flags=0x%lX color=0x%08lX (%s) -> %s\n", state, flags, color,
                bgra ? "bgra" : "rgba", ok ? "TRUE" : "FALSE");
    return ok ? 0 : 1;
}

int applyLayered(HWND h, int alpha) {
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) {
        SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        std::printf("added WS_EX_LAYERED (was 0x%08lX)\n", ex);
    }
    const BOOL ok = SetLayeredWindowAttributes(h, 0, BYTE(alpha & 255), LWA_ALPHA);
    std::printf("layered alpha=%d -> %s\n", alpha, ok ? "TRUE" : "FALSE");
    return ok ? 0 : 1;
}

int removeLayered(HWND h) {
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) {
        std::printf("layered was not set\n");
        return 0;
    }
    SetWindowLongW(h, GWL_EXSTYLE, ex & ~LONG(WS_EX_LAYERED));
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    std::printf("removed WS_EX_LAYERED\n");
    return 0;
}

// DWMSBT_AUTO 0, NONE 1, MICA 2, ACRYLIC 3, CUSTOM 4
int applyBackdrop(HWND h, int kind) {
    DWORD v = DWORD(kind);
    const HRESULT hr = DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &v, sizeof v);
    std::printf("backdrop=%d -> hr=0x%08lX %s\n", kind, unsigned long(hr),
                SUCCEEDED(hr) ? "ok" : "failed");
    return SUCCEEDED(hr) ? 0 : 1;
}

// Explorer owns the taskbar's own material and re-applies it when the shell setting flips, so the
// way back from a bad accent or backdrop is the toggle Windows does expose: 0 then 1 again.
int flipTransparency(int value) {
    HKEY k{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        std::printf("cannot open Personalize\n");
        return 1;
    }
    const DWORD v = DWORD(value);
    const LONG rc = RegSetValueExA(k, "EnableTransparency", 0, REG_DWORD, (const BYTE *)&v, sizeof v);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        std::printf("registry write failed=%ld\n", rc);
        return 1;
    }
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) L"ImmersiveColorSet",
                        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 1000, nullptr);
    std::printf("EnableTransparency=%d and WM_SETTINGCHANGE(ImmersiveColorSet) broadcast\n", value);
    return 0;
}

void usage() {
    std::printf(
        "trayalpha probe | rect | state\n"
        "trayalpha accent <state> <flags> <a> <r> <g> <b> [bgra]\n"
        "trayalpha layered <0-255> | unlayered\n"
        "trayalpha backdrop <0-4>\n"
        "trayalpha transparency <0|1>\n"
        "trayalpha restore            unlayered + accent state 0 + transparency 1\n");
}

} // namespace

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::string mode = argc > 1 ? argv[1] : "";
    HWND h = tray();
    if (!h) return 2;

    if (mode == "probe" || mode == "state") {
        printRect(L"primary", h);
        for (HWND s = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr); s;
             s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr))
            printRect(L"secondary", s);
        showState(h);
        return 0;
    }
    if (mode == "rect") {
        printRect(L"primary", h);
        return 0;
    }
    if (mode == "accent" && argc >= 8) {
        const bool bgra = argc > 8 && std::strcmp(argv[8], "bgra") == 0;
        return applyAccent(h, std::atol(argv[2]), std::atol(argv[3]), std::atoi(argv[4]),
                           std::atoi(argv[5]), std::atoi(argv[6]), std::atoi(argv[7]), bgra);
    }
    if (mode == "layered" && argc >= 3) return applyLayered(h, std::atoi(argv[2]));
    if (mode == "unlayered") return removeLayered(h);
    if (mode == "backdrop" && argc >= 3) return applyBackdrop(h, std::atoi(argv[2]));
    if (mode == "transparency" && argc >= 3) return flipTransparency(std::atoi(argv[2]));
    if (mode == "restore") {
        removeLayered(h);
        applyAccent(h, 0, 0, 0, 0, 0, 0, false);
        return flipTransparency(1);
    }
    usage();
    return 2;
}
