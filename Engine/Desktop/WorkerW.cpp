#include "Engine/Desktop/WorkerW.hpp"
#include "Engine/Core/Log.hpp"

namespace sw {
static constexpr const char* MOD = "desk";

namespace {

const wchar_t* kProgman = L"Progman";
const wchar_t* kWorkerW = L"WorkerW";
const wchar_t* kDefView = L"SHELLDLL_DefView";

// 0x052C is the private Progman message that makes explorer build the wallpaper WorkerW layer.
constexpr UINT kCreateWallpaperLayer = 0x052C;

std::wstring WindowText(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 255);
    return buf;
}

std::string RectOf(HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    return std::format("({},{}) {}x{}", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

bool IsChildOf(HWND child, HWND parent) {
    for (HWND p = GetParent(child); p; p = GetParent(p))
        if (p == parent) return true;
    return false;
}

struct FindClassCtx {
    const wchar_t* cls;
    HWND found = nullptr;
};

BOOL CALLBACK FindClassProc(HWND h, LPARAM l) {
    auto* ctx = reinterpret_cast<FindClassCtx*>(l);
    wchar_t buf[256] = {};
    if (GetClassNameW(h, buf, 255) > 0 && wcscmp(buf, ctx->cls) == 0) {
        ctx->found = h;
        return FALSE;
    }
    return TRUE;
}

std::string Hex(HWND h) {
    char b[32];
    snprintf(b, sizeof(b), "0x%llX", (unsigned long long)(uintptr_t)h);
    return b;
}

} // namespace

std::string WorkerW::ClassOf(HWND h) {
    wchar_t buf[256] = {};
    int n = GetClassNameW(h, buf, 255);
    return n > 0 ? ToUtf8(std::wstring_view(buf, n)) : "";
}

HWND WorkerW::FindTopLevelByClass(const wchar_t* className) {
    FindClassCtx ctx{className, nullptr};
    EnumWindows(FindClassProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

HWND WorkerW::FindProgman() {
    // GetShellWindow() is documented to return the desktop's Program Manager. It is also the only
    // reliable way here: on build 26300 FindWindowW(L"Progman", nullptr) returns NULL even though
    // Progman exists and is visible (measured), so class-name search is the fallback.
    HWND h = GetShellWindow();
    if (h) {
        std::string cls = ClassOf(h);
        if (cls == "Progman") return h;
        Warn(MOD, "GetShellWindow returned class '{}', not Progman", cls);
    }
    FindClassCtx ctx{kProgman, nullptr};
    EnumWindows(FindClassProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

const char* DesktopHost::methodName() const {
    switch (method) {
        case HostMethod::WorkerWBelowIcons: return "WorkerW_below_icons";
        case HostMethod::ProgmanChildBelowIcons: return "Progman_child_below_icons";
        case HostMethod::OwnTopLevel: return "own_toplevel_bottom";
        default: return "none";
    }
}

std::string DesktopHost::describe() const {
    return std::format("host={} progman=0x{:X} defView=0x{:X} iconHost=0x{:X} origin=({},{}) client={}x{} :: {}",
                       methodName(), (uintptr_t)parent, (uintptr_t)defView, (uintptr_t)iconHost, origin.x, origin.y,
                       client.cx, client.cy, detail);
}

Rect DesktopHost::ToClient(const Rect& desktopPx) const {
    return Rect{desktopPx.x - origin.x, desktopPx.y - origin.y, desktopPx.w, desktopPx.h};
}

bool WorkerW::LocateLayer() {
    host_.defView = nullptr;
    host_.iconHost = nullptr;
    HWND defViewOwner = nullptr;

    // DefView is a child window; search the top-levels that host it.
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* out = reinterpret_cast<HWND*>(l);
            HWND child = FindWindowExW(h, nullptr, kDefView, nullptr);
            if (child) { *out = h; return FALSE; }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&defViewOwner));

    if (!defViewOwner) {
        host_.defView = nullptr;
        Warn(MOD, "no SHELLDLL_DefView found - icon layer unknown");
        return false;
    }
    host_.iconHost = defViewOwner;
    host_.defView = FindWindowExW(defViewOwner, nullptr, kDefView, nullptr);
    Info(MOD, "icon layer: defView=0x{:X} owned by 0x{:X} class '{}' {}", (uintptr_t)host_.defView,
         (uintptr_t)defViewOwner, ClassOf(defViewOwner).c_str(), RectOf(defViewOwner).c_str());

    // The wallpaper layer is the WorkerW sitting *below* the icon host in Z-order.
    for (HWND n = GetWindow(defViewOwner, GW_HWNDNEXT); n; n = GetWindow(n, GW_HWNDNEXT)) {
        if (ClassOf(n) == "WorkerW") {
            host_.parent = n;
            host_.method = HostMethod::WorkerWBelowIcons;
            host_.detail = "below icon host 0x" + [&] {
                char b[32];
                snprintf(b, sizeof(b), "%llX", (unsigned long long)(uintptr_t)defViewOwner);
                return std::string(b);
            }();
            return true;
        }
    }

    if (defViewOwner == host_.progman) {
        // On this build the layer that 0x052C builds is a *child* of Progman, not a top-level sibling,
        // so the walk above cannot see it. Parenting straight into Progman instead - what this function
        // used to do - puts our surface under the icon host, which paints the system wallpaper right on
        // top of us: the renderer keeps drawing frames and the screen keeps showing the desktop photo.
        // Children come out top-down, so the ones after DefView are below the icons: that is where a
        // wallpaper layer belongs.
        bool pastIcons = false;
        for (HWND c = GetWindow(defViewOwner, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (c == host_.defView) { pastIcons = true; continue; }
            if (!pastIcons || ClassOf(c) != "WorkerW") continue;
            host_.parent = c;
            host_.method = HostMethod::WorkerWBelowIcons;
            host_.detail = std::format("WorkerW is a Progman child 0x{:X}, below DefView in z-order",
                                        (uintptr_t)c);
            Info(MOD, "wallpaper layer: {}", host_.detail);
            return true;
        }
    }

    if (defViewOwner == host_.progman) {
        // Explorer did not build a WorkerW: parent into Progman itself and keep our windows at the
        // bottom of its child list so the icon sibling still paints and receives clicks above us.
        host_.parent = host_.progman;
        host_.method = HostMethod::ProgmanChildBelowIcons;
        host_.detail = "no wallpaper WorkerW after 0x052C, using Progman child list";
        return true;
    }
    host_.detail = "no candidate layer";
    return false;
}

bool WorkerW::Attach(std::string& error) {
    host_ = {};
    host_.progman = FindProgman();
    if (!host_.progman) {
        error = "Progman not found";
        return false;
    }
    Info(MOD, "progman=0x{:X} '{}' {}", (uintptr_t)host_.progman, ToUtf8(WindowText(host_.progman)),
         RectOf(host_.progman));

    if (IsChildOf(host_.progman, nullptr)) (void)0;

    // Ask explorer for the wallpaper layer. SendMessageTimeout is used instead of SendMessage so a
    // busy explorer cannot wedge us; the call is what creates the WorkerW.
    LRESULT res = 0;
    BOOL ok = SendMessageTimeoutW(host_.progman, kCreateWallpaperLayer, 0, 0, SMTO_NORMAL, 1000,
                                  reinterpret_cast<DWORD_PTR*>(&res));
    Info(MOD, "0x052C sent: ok={} result={}", ok ? "yes" : "no", (long long)res);

    if (!LocateLayer()) {
        host_.parent = nullptr;
        host_.method = HostMethod::OwnTopLevel;
        host_.detail = "falling back to a bottom-of-Z-order top-level window (icons get covered)";
        Warn(MOD, "{}", host_.detail);
    }

    if (host_.parent && host_.method != HostMethod::OwnTopLevel) {
        POINT tl{0, 0};
        ClientToScreen(host_.parent, &tl);
        host_.origin = tl;
        RECT cr{};
        GetClientRect(host_.parent, &cr);
        host_.client = SIZE{cr.right - cr.left, cr.bottom - cr.top};
    }
    Info(MOD, "{}", host_.describe());
    if (host_.method == HostMethod::OwnTopLevel && host_.client.cx == 0) {
        host_.origin = POINT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
        host_.client = SIZE{GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }
    if (host_.client.cx <= 0 || host_.client.cy <= 0) {
        error = "host client area is empty";
        return false;
    }
    return true;
}

void WorkerW::Recheck() {
    HWND before = host_.parent;
    std::string detail = host_.detail;
    if (LocateLayer()) {
        host_.detail = detail;
        POINT tl{0, 0};
        ClientToScreen(host_.parent, &tl);
        host_.origin = tl;
        RECT cr{};
        GetClientRect(host_.parent, &cr);
        host_.client = SIZE{cr.right - cr.left, cr.bottom - cr.top};
        if (before != host_.parent)
            Info(MOD, "layer re-resolved: 0x{:X} -> 0x{:X}", (uintptr_t)before, (uintptr_t)host_.parent);
    }
}

} // namespace sw
