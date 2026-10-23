#include "Engine/App/Tray.hpp"
#include "Engine/App/Application.hpp"
#include "Engine/Core/Json.hpp"
#include "Engine/Core/Log.hpp"

#include <shlobj.h>

#include <cmath>

namespace sw {
static constexpr const char* MOD = "tray";

namespace {

// Row geometry, measured off his reference popup (187x206 crop: plate #F9F9F9 at x=10..177,
// y=15..199). Two-character labels are 24 px of ink 11 px tall - 12 px per glyph, the same face size
// we already draw - the pitch inside a group is 22 px and across a group line 31, so the band a
// separator sits in is 9. The tick's ink begins 12 px inside the plate and is 8 px wide; the words
// begin at 37. The frame is one pixel of #E5E5E5 with a ~4 px corner, not the shell's #A0A0A0.
constexpr int kRowH = 22;
constexpr int kSepH = 9;
constexpr int kPadL = 12;      // where the tick's ink begins
constexpr int kTextL = 37;     // where the words begin, tick column or not
constexpr int kRuleInset = 8;  // the group line stops 8 px short of each edge (152 of 168 px)
constexpr int kPadR = 56;      // his reference leaves 59 px past the longest row; ours left 56
constexpr int kPadV = 4;       // the plate's own margin above the first row and below the last one
constexpr int kRadius = 4;

const COLORREF kPlate = RGB(0xF9, 0xF9, 0xF9);
const COLORREF kSel = RGB(0xD7, 0xE6, 0xFD);
const COLORREF kBorder = RGB(0xE5, 0xE5, 0xE5);
const COLORREF kRule = RGB(0xD7, 0xD7, 0xD7);
const COLORREF kInk = RGB(0x00, 0x00, 0x00);   // the reference's text core is black, not #1E1E1E
const COLORREF kInkOff = RGB(0xA0, 0xA0, 0xA0);

constexpr UINT kByeThread = WM_APP + 0x41;
// Opening the popup is not done from the tray callback: while that callback runs, explorer is still
// inside its own click handling and still holds the mouse. A window created and capture-snatched in
// that window gets no clicks at all and leaves the shell showing the wait cursor forever. So the
// callback posts this and returns; the popup is built one message later.
constexpr UINT kOpenPopup = WM_APP + 0x50;
constexpr const wchar_t* kMenuClass = L"SmartWallpaperTrayMenu";
constexpr const wchar_t* kPopupClass = L"SmartWallpaperTrayPopup";

// The words, taken from the settings window's own table (Wallpaper/Bridge.cpp) so the two screens
// cannot drift apart. zh is the default; the switch lives in the settings window and lands in ui.ini.
struct Word { const wchar_t* zh; const wchar_t* en; };
const Word wNext{L"下一张", L"Next"};
const Word wPrev{L"上一张", L"Previous"};
const Word wAuto{L"自动播放", L"Auto play"};
const Word wStop{L"停止播放", L"Stop playing"};
const Word wPauseWall{L"暂停壁纸", L"Pause the wallpaper"};
const Word wMute{L"静音壁纸", L"Mute wallpaper"};
const Word wRing{L"快捷键环", L"Shortcut ring"};
const Word wPauseRing{L"暂停图标", L"Pause the icons"};
const Word wHideIcons{L"隐藏桌面图标", L"Hide desktop icons"};
const Word wQuit{L"退出", L"Quit"};

std::wstring Of(const Word& w, bool zh) { return zh ? w.zh : w.en; }
// The font native menus use, i.e. the same face and size the settings window's QMenu gets on this
// machine. DEFAULT_GUI_FONT is the old 8pt system face and reads visibly smaller next to it - that
// was the first thing wrong with the drawn menu.
HFONT MenuFont() {
    static HFONT f = [] {
        NONCLIENTMETRICSW nc{};
        nc.cbSize = sizeof(nc);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(nc), &nc, 0))
            return CreateFontIndirectW(&nc.lfMenuFont);
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }();
    return f;
}

// The settings window writes its language here (QSettings IniFormat, SmartWallpaper / ui). Read it,
// never write it: one owner, and this is the reader.
bool LanguageIsZh() {
    wchar_t appdata[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return true;
    const std::wstring ini = std::wstring(appdata) + L"\\SmartWallpaper\\ui.ini";
    wchar_t val[16]{};
    GetPrivateProfileStringW(L"General", L"lang", L"zh", val, 16, ini.c_str());
    return lstrcmpiW(val, L"en") != 0;
}

} // namespace

// ------------------------------------------------------------------ install / tear down

bool Tray::Install(HWND owner, Application* app, const std::wstring& exeDir, std::string& error) {
    owner_ = owner;
    app_ = app;
    exeDir_ = exeDir;

    // The thread that will own the icon has to exist before NIM_ADD, because the icon is registered
    // to a window on it.
    thread_ = std::thread([this] { ThreadMain(); });
    for (int i = 0; i < 200 && menuWnd_.load() == nullptr; ++i) Sleep(10);
    HWND mw = menuWnd_.load();
    if (!mw) { error = "tray menu thread never made its window"; return false; }

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = mw;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uVersion = NOTIFYICON_VERSION_4;
    nid_.uCallbackMessage = WM_TRAYICON;
    // The brand mark this executable carries (resources/app.rc, resource id 1) at the size the
    // notification area actually uses, so the shell never shrinks the 256 px frame itself.
    nid_.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                               IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!nid_.hIcon) nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!nid_.hIcon) { error = "LoadIcon failed"; return false; }
    wcsncpy_s(nid_.szTip, L"Wallpaper", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) { error = "Shell_NotifyIcon(NIM_ADD) failed"; return false; }
    installed_ = true;
    Info(MOD, "tray icon added, menu on its own thread");
    return true;
}

void Tray::Remove() {
    if (!thread_.joinable()) return;
    if (installed_) {
        nid_.uFlags = NIF_MESSAGE;
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        installed_ = false;
    }
    HWND mw = menuWnd_.load();
    if (!mw) {
        // No window to close means nothing is pumping; joining would hang the shutdown this very
        // line exists to serve. Let the thread go and get on with quitting.
        thread_.detach();
        return;
    }
    PostMessageW(mw, kByeThread, 0, 0);
    thread_.join();
    menuWnd_.store(nullptr);
}

void Tray::Publish(const TrayState& s) {
    std::lock_guard lk(mtx_);
    state_ = s;
}

TrayState Tray::Snapshot() const {
    std::lock_guard lk(mtx_);
    return state_;
}

// Three numbers the card answers with: what this process costs the machine. One per line under the
// product name, because that is how a status card reads; the shell honours \n inside szTip. A GPU
// share below zero means the counter never answered, and a line that cannot answer is left out
// rather than written as a zero.
void Tray::SetTip(double wsMb, double cpuPct, double gpuPct) {
    if (!installed_) return;
    const bool zh = LanguageIsZh();
    std::wstring tip = L"Wallpaper";
    auto add = [&tip, zh](std::wstring label, const std::string& value) {
        tip += L"\n";
        tip += label;
        tip += zh ? L"：" : L": ";
        tip += ToWide(value);
    };
    add(zh ? L"内存" : L"Memory", std::format("{:.1f} MB", wsMb));
    add(L"CPU", std::format("{:.2f} %", cpuPct));
    if (gpuPct >= 0.0) add(L"GPU", std::format("{:.2f} %", gpuPct));
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);   // NIM_MODIFY is not thread-affine
    // Once, not every second: this is the only instrument for what the hover card actually says -
    // the shell draws the tip itself, so nothing here can see it. Newlines shown as " | ". The first
    // status update can land before the GPU counter has a window behind it, so give it 20 seconds of
    // chances to show all three lines before settling for whatever the card looked like first.
    static bool logged = false;
    static int waits = 0;
    if (!logged && (tip.find(L"GPU") != std::wstring::npos || ++waits > 20)) {
        logged = true;
        std::string flat = ToUtf8(tip);
        for (auto& c : flat) if (c == '\n') c = '|';
        Info(MOD, "tip ({} wide chars): {}", tip.size(), flat);
    }
}

// ------------------------------------------------------------------ building the menu

std::vector<Tray::Item> Tray::Build(const TrayState& s) const {
    std::vector<Item> v;
    auto add = [&](const Word& w, UINT n, bool checked, bool enabled) {
        v.push_back(Item{ Of(w, s.zh), n, checked, enabled, false });
    };
    auto sep = [&] { v.push_back(Item{ L"", 0, false, false, true }); };

    add(wNext, 1, false, true);
    add(wPrev, 2, false, true);
    sep();
    add(wAuto, 3, s.rotateOn, true);
    add(wStop, 4, !s.rotateOn, true);
    add(wPauseWall, 5, s.paused, s.pauseActionable);
    add(wMute, 6, s.audioMuted, true);
    sep();
    add(wRing, 7, s.orbitOn, s.orbitActionable);
    add(wPauseRing, 8, s.orbitHeld, s.orbitOn);
    add(wHideIcons, 9, s.iconsHidden, true);
    sep();
    add(wQuit, 10, false, true);
    return v;
}

namespace {

// Which row a pointer y is over, or -1 for a group line, a disabled row, or outside. The y comes in
// as a client coordinate, so the plate's own top margin is part of the stack it walks.
int RowAt(const std::vector<Tray::Item>& items, int y) {
    y -= kPadV;   // below the plate's own top gap there are no rows
    if (y < 0) return -1;
    int acc = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        const int h = items[i].sep ? kSepH : kRowH;
        if (y >= acc && y < acc + h)
            return (!items[i].sep && items[i].enabled) ? static_cast<int>(i) : -1;
        acc += h;
    }
    return -1;
}

int StackHeight(const std::vector<Tray::Item>& items) {
    int h = 0;
    for (const auto& it : items) h += it.sep ? kSepH : kRowH;
    return h;
}

// The state a popup window carries: the rows it shows and which one the pointer is on.
struct PopupState {
    Tray* tray = nullptr;
    std::vector<Tray::Item> items;
    int hover = -1;
};

// One popup at a time, so the mouse hook has something to point at. The menu thread owns both.
HWND g_popupHwnd = nullptr;
HHOOK g_mouseHook = nullptr;

// A press outside the plate closes it and is not passed on - that is what a menu does with the click
// that dismisses it, and it is why the desktop under the plate does not also get clicked.
LRESULT CALLBACK DismissOnOutside(int code, WPARAM wp, LPARAM lp) {
    if (code < 0) return CallNextHookEx(nullptr, code, wp, lp);
    const HWND pop = g_popupHwnd;
    const bool press = wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN
                    || wp == WM_NCLBUTTONDOWN || wp == WM_NCRBUTTONDOWN;
    if (pop && press) {
        const auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        RECT rc{};
        bool inside = false;
        if (GetWindowRect(pop, &rc)) {
            inside = PtInRect(&rc, mi->pt) != FALSE;
        }
        if (!inside) {
            PostMessageW(pop, WM_CLOSE, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace

// The plate's colours, drawn into a 32 bpp buffer. GDI writes no alpha, so this is only the RGB half
// of the picture; RebuildLayer turns the rounded corner and the shadow into an alpha channel after.
void Tray::PaintPopup(HDC dc, const RECT& plate, const std::vector<Item>& items, int hover) const {
    const int arc = kRadius * 2;
    HRGN clip = CreateRoundRectRgn(plate.left, plate.top, plate.right, plate.bottom, arc, arc);
    SelectClipRgn(dc, clip);
    HBRUSH fill = CreateSolidBrush(kPlate);
    FillRect(dc, &plate, fill);
    DeleteObject(fill);

    HGDIOBJ oldF = SelectObject(dc, MenuFont());
    SetBkMode(dc, TRANSPARENT);
    int y = plate.top + kPadV;   // the plate's own gap above the first row
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        const int h = it.sep ? kSepH : kRowH;
        const RECT r{ plate.left, y, plate.right, y + h };
        if (!it.sep && static_cast<int>(i) == hover) {
            HBRUSH sel = CreateSolidBrush(kSel);
            FillRect(dc, &r, sel);
            DeleteObject(sel);
        }
        if (it.sep) {   // the band is 9 px of plate with one rule through its middle
            HPEN pen = CreatePen(PS_SOLID, 1, kRule);
            HGDIOBJ old = SelectObject(dc, pen);
            const int ly = y + h / 2;
            MoveToEx(dc, plate.left + kRuleInset, ly, nullptr);
            LineTo(dc, plate.right - kRuleInset, ly);
            SelectObject(dc, old);
            DeleteObject(pen);
        } else {
            SetTextColor(dc, it.enabled ? kInk : kInkOff);
            if (it.checked) {
                // Left-aligned at kPadL: centred in the gutter put the tick's ink at 20 px, and his
                // reference has it at 12.
                RECT cr{ plate.left + kPadL, y, plate.left + kTextL, y + h };
                DrawTextW(dc, L"✓", -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            RECT tr{ plate.left + kTextL, y, plate.right - kPadR, y + h };
            DrawTextW(dc, it.label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        y += h;
    }
    SelectObject(dc, oldF);

    // One pixel of #E5E5E5 around the plate - the frame his reference has and the shell's menu does not.
    // Measured on his own crop: with the rect pulled in by one the ring landed on x=163 / y=253 and the
    // plate's last row and column stuck out a pixel of #F9F9F9 - the lip he is pointing at. GDI's
    // outline already sits on the rect's exclusive edges, so the buffer's own size is the right rect.
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, plate.left, plate.top, plate.right, plate.bottom, arc, arc);
    SelectObject(dc, oldB);
    SelectObject(dc, old);
    DeleteObject(pen);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

// Signed distance to a rounded rectangle: negative inside, positive outside, in pixels. The whole
// point is that one expression gives both the antialiased corner (a 1 px ramp across sd = 0) and the
// shadow's falloff (how far outside).
float RoundRectSd(float px, float py, float hw, float hh, float r) {
    const float dx = std::fabs(px) - hw + r;
    const float dy = std::fabs(py) - hh + r;
    const float ax = std::fmax(dx, 0.0f);
    const float ay = std::fmax(dy, 0.0f);
    return std::fmin(std::fmax(dx, dy), 0.0f) + std::sqrt(ax * ax + ay * ay) - r;
}

// Build the frame and hand it to the compositor. This is what makes the corner round instead of
// chamfered, and what stops a black fringe appearing wherever the arc cuts the rectangle: outside the
// plate the pixel is *transparent*, not unpainted.
void Tray::RebuildLayer(HWND h, const std::vector<Item>& items, int hover) {
    RECT wr{};
    GetWindowRect(h, &wr);
    const int bw = wr.right - wr.left;
    const int bh = wr.bottom - wr.top;
    if (bw <= 0 || bh <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;   // top-down, so the buffer reads the way the loop does
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        Warn(MOD, "popup buffer failed: win32 error {}", static_cast<unsigned>(GetLastError()));
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ oldB = SelectObject(mem, dib);
    // The base is the plate's own colour, not black: the corner pixels are partly transparent, and
    // whatever colour sits under them is what blends with the desktop. With black there they read as a
    // dark line all the way round - the edge he has now pointed at twice.
    RECT whole{ 0, 0, bw, bh };
    HBRUSH base = CreateSolidBrush(kPlate);
    FillRect(mem, &whole, base);
    DeleteObject(base);
    const RECT plate{ 0, 0, bw, bh };
    PaintPopup(mem, plate, items, hover);

    const float hw = (plate.right - plate.left) / 2.0f;
    const float hh = (plate.bottom - plate.top) / 2.0f;
    const float cx = plate.left + hw;
    const float cy = plate.top + hh;
    auto* px = static_cast<unsigned char*>(bits);
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            unsigned char* p = px + (static_cast<size_t>(y) * bw + x) * 4;
            const float sd = RoundRectSd(x + 0.5f - cx, y + 0.5f - cy, hw, hh, static_cast<float>(kRadius));
            // The plate: the 1 px ramp sits OUTSIDE the rectangle, so the plate's own outermost row is
            // fully opaque. Measured with the ramp centred on the edge instead, that row came out 50%
            // transparent and read as a dark line along all four sides - the "black bottom edge" he saw.
            // 1.0 - sd puts the 1 px ramp entirely OUTSIDE the rectangle, so every pixel of the plate
            // itself is fully opaque. There is no shadow: an outer margin filled with a dark colour is
            // exactly the halo and the dark edge he is pointing at.
            const float a = std::fmin(std::fmax(1.0f - sd, 0.0f), 1.0f);
            const unsigned av = static_cast<unsigned>(a * 255.0f + 0.5f);
            p[3] = static_cast<unsigned char>(av);
            p[0] = static_cast<unsigned char>(p[0] * av / 255u);   // premultiplied, as ULW wants
            p[1] = static_cast<unsigned char>(p[1] * av / 255u);
            p[2] = static_cast<unsigned char>(p[2] * av / 255u);
        }
    }

    SIZE sz{ bw, bh };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;   // per-pixel alpha does the work; this one stays at full
    bf.AlphaFormat = AC_SRC_ALPHA;
    RECT outer{};
    GetWindowRect(h, &outer);
    POINT where{ outer.left, outer.top };
    if (!UpdateLayeredWindow(h, screen, &where, &sz, mem, &src, 0, &bf, ULW_ALPHA))
        Warn(MOD, "popup present failed: win32 error {}", static_cast<unsigned>(GetLastError()));
    SelectObject(mem, oldB);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK Tray::PopupWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    PopupState* ps = reinterpret_cast<PopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    auto close = [h] { DestroyWindow(h); };
    auto pick = [h, ps, close] {
        if (!ps || ps->hover < 0 || ps->hover >= static_cast<int>(ps->items.size())) {
            // A click that lands between rows, or before the pointer ever moved over the plate, is the
            // failure this line exists to catch: the menu closes and nothing else happens.
            Warn(MOD, "popup click with no row under it (hover={})", ps ? ps->hover : -999);
            close();
            return;
        }
        const Item& it = ps->items[ps->hover];
        if (it.enabled && !it.sep)
            PostMessageW(ps->tray->owner_, WM_TRAY_RESULT, static_cast<WPARAM>(it.id), 0);
        close();
    };

    switch (m) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            return DefWindowProcW(h, m, w, l);
        case WM_PAINT: {
            PAINTSTRUCT pnt{};
            BeginPaint(h, &pnt);
            if (ps && ps->tray) ps->tray->RebuildLayer(h, ps->items, ps->hover);
            EndPaint(h, &pnt);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!ps) return 0;
            const int idx = RowAt(ps->items, GET_Y_LPARAM(l));
            if (idx != ps->hover) {
                ps->hover = idx;
                ps->tray->RebuildLayer(h, ps->items, ps->hover);   // a layered window gets no WM_PAINT
            }
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            // Leaving takes the highlight off but keeps the menu open - that is what a real menu does,
            // and closing on a stray sweep to the taskbar would be worse.
            if (ps && ps->hover != -1) { ps->hover = -1; ps->tray->RebuildLayer(h, ps->items, ps->hover); }
            return 0;
        case WM_LBUTTONDOWN:
            // Two questions this answers at once: does a press reach this window at all, and where in
            // it. Without them a menu that "does nothing" is one symptom with three causes.
            Info(MOD, "popup press at {},{}", GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
            // Without a capture, a message only reaches this window when the pointer was over it, so
            // there is no "outside" case left to handle here - the hook owns that.
            pick();
            return 0;
        case WM_RBUTTONUP:
            close();
            return 0;
        case WM_KEYDOWN:
            switch (w) {
                case VK_ESCAPE:
                    close();
                    return 0;
                case VK_RETURN:
                    pick();
                    return 0;
                case VK_UP:
                case VK_DOWN: {
                    if (!ps) return 0;
                    const int n = static_cast<int>(ps->items.size());
                    int i = ps->hover < 0 ? (w == VK_DOWN ? -1 : n) : ps->hover;
                    // Walk over the group lines and the greyed rows: a keyboard-only user cannot see
                    // which rows are dead and would otherwise land on one.
                    for (int step = 0; step < n; ++step) {
                        i = (i + (w == VK_DOWN ? 1 : n - 1)) % n;
                        if (ps->items[i].enabled && !ps->items[i].sep) break;
                    }
                    ps->hover = i;
                    ps->tray->RebuildLayer(h, ps->items, ps->hover);   // a layered window gets no WM_PAINT
                    return 0;
                }
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            Info(MOD, "popup closing");
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            // The hook has to go with the window: a low-level mouse hook left installed keeps every
            // click in the session passing through this thread, and if this thread is busy the whole
            // desktop waits on it - which is the spinning cursor.
            if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
            g_popupHwnd = nullptr;
            Info(MOD, "popup destroyed");
            if (GetCapture() == h) ReleaseCapture();
            if (ps) {
                if (ps->tray) ps->tray->popup_ = nullptr;
                delete ps;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Tray::MenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Tray* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
        case WM_CREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            return 0;
        case WM_TRAYICON: {
            if (!self) return 0;
            const UINT ev = LOWORD(l);   // NOTIFYICON_VERSION_4 keeps the event in the low word
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                PostMessageW(self->owner_, WM_TRAY_OPEN_GUI, 0, 0);
                return 0;
            }
            if (ev != WM_RBUTTONUP && ev != WM_CONTEXTMENU) return 0;

            POINT pt{};
            GetCursorPos(&pt);
            self->pendingPt_ = pt;
            // The popup is NOT opened from here. While this callback runs, explorer is still inside
            // its own click handling, and a window created and given the mouse capture in that window
            // gets no clicks at all. Post it and return; the next message opens it.
            PostMessageW(h, kOpenPopup, 0, 0);
            return 0;
        }
        case kOpenPopup: {
            if (!self) return 0;
            if (self->popup_) { DestroyWindow(self->popup_); self->popup_ = nullptr; }
            POINT pt = self->pendingPt_;

            TrayState s = self->Snapshot();
            s.zh = LanguageIsZh();

            auto* ps = new PopupState{ self, self->Build(s), -1 };
            // The plate is as wide as its longest row plus the two gutters.
            int textW = 0;
            HDC dc = GetDC(h);
            HGDIOBJ oldF = SelectObject(dc, MenuFont());
            for (const auto& it : ps->items) {
                SIZE sz{};
                GetTextExtentPoint32W(dc, it.label.c_str(), static_cast<int>(it.label.size()), &sz);
                if (sz.cx > textW) textW = sz.cx;
            }
            SelectObject(dc, oldF);
            ReleaseDC(h, dc);

            const int width = kTextL + textW + kPadR;
            const int height = StackHeight(ps->items) + 2 * kPadV;
            // His ask: the click point is the plate's BOTTOM-LEFT corner, so the menu grows up and to
            // the right from where the pointer is. The window is bigger than the plate by the shadow
            // margin, so that margin comes off the position too. Only pushed back inside the work area
            // when it would otherwise hang off an edge.
            int x = pt.x, y = pt.y - height;
            const int bw = width, bh = height;
            HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(hm, &mi)) {
                if (x + bw > mi.rcWork.right) x = mi.rcWork.right - bw;
                if (x < mi.rcWork.left) x = mi.rcWork.left;
                if (y < mi.rcWork.top) y = mi.rcWork.top;
                if (y + bh > mi.rcWork.bottom) y = mi.rcWork.bottom - bh;
            }

            // WS_EX_LAYERED because the frame is presented with UpdateLayeredWindow, not painted: that
            // is the only way the corner can be a curve and the pixels outside it be transparent.
            HWND p = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kPopupClass,
                                     L"Wallpaper", WS_POPUP, x, y, bw, bh, h, nullptr,
                                     GetModuleHandleW(nullptr), ps);
            if (!p) {
                // A menu that will not come up is otherwise silent: the icon is there, the click is
                // accepted, and nothing appears.
                Warn(MOD, "popup create failed at {},{} {}x{}: win32 error {}", x, y, bw, bh,
                     static_cast<unsigned>(GetLastError()));
                delete ps;
                return 0;
            }
            self->popup_ = p;
            // No SetForegroundWindow, no SetCapture, no SetActiveWindow. Holding a capture that no
            // button press owns is what made the whole desktop show the wait cursor, and taking the
            // foreground from here is what swallowed the first click. The plate is topmost, clicks
            // inside it arrive as usual, and the low-level mouse hook below does the dismiss-on-click-
            // outside that a capture was standing in for.
            g_popupHwnd = p;
            g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, DismissOnOutside, GetModuleHandleW(nullptr), 0);
            ShowWindow(p, SW_SHOW);
            // Composed here, not from WM_PAINT: a layered window presented through
            // UpdateLayeredWindow never gets a paint message, so the frame built in response to one
            // would never be built at all - the menu is there, sized, and completely invisible.
            self->RebuildLayer(p, ps->items, ps->hover);
            // The plate's own left and bottom, not the buffer's: those are the two numbers the ask is
            // about (bottom-left at the click point), and the shadow margin would otherwise hide them.
            Info(MOD, "popup opened plate left={} bottom={} buffer {}x{} over {} rows, hook={}",
                 x, y + height, bw, bh, ps->items.size(),
                 g_mouseHook ? "yes" : "NO");
            return 0;
        }
        case kByeThread:
            if (self && self->popup_) { DestroyWindow(self->popup_); self->popup_ = nullptr; }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

void Tray::ThreadMain() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Tray::MenuWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMenuClass;
    RegisterClassExW(&wc);
    // The popup's own class. No CS_DROPSHADOW: that is the system's window shadow, drawn down and to
    // the right of whatever we present, and it has no intensity setting - the only choice is on or
    // off. He asked for it lighter, and off is the lighter end of what this knob can do.
    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = Tray::PopupWndProc;
    pc.hInstance = wc.hInstance;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = kPopupClass;
    if (!RegisterClassExW(&pc))
        Warn(MOD, "popup class not registered: win32 error {}", static_cast<unsigned>(GetLastError()));
    // A real hidden window, not HWND_MESSAGE: the owner-draw messages for a tracked menu go to the
    // menu's owner window, and a message-only window is outside the hierarchy that loop expects.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, kMenuClass, L"Wallpaper tray", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!h) return;
    menuWnd_.store(h);   // Install() waits for exactly this; without it the icon is never added

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(kMenuClass, wc.hInstance);
    UnregisterClassW(kPopupClass, wc.hInstance);
}

// ------------------------------------------------------------------ the picks, on the main thread

bool Tray::HandleCommand(UINT n) {
    if (!app_ || n < 1 || n > 10) return false;
    const TrayState s = Snapshot();
    std::string line;

    switch (n) {
        case 1: line = R"({"cmd":"rotate","arg":"next"})"; break;
        case 2: line = R"({"cmd":"rotate","arg":"prev"})"; break;
        case 3: line = R"({"cmd":"rotate","arg":"on"})"; break;
        case 4: line = R"({"cmd":"rotate","arg":"off"})"; break;
        // Every toggle is sent as an explicit on/off: these verbs do not understand "toggle", and
        // the state the menu just showed is the state the user judged against.
        case 5: line = s.paused ? R"({"cmd":"resume"})" : R"({"cmd":"pause"})"; break;
        case 6: line = std::format(R"({{"cmd":"mute","arg":"{}"}})", s.audioMuted ? "off" : "on"); break;
        case 7: line = std::format(R"({{"cmd":"orbit","arg":"{}"}})", s.orbitOn ? "off" : "on"); break;
        case 8: line = std::format(R"({{"cmd":"orbithold","arg":"{}"}})", s.orbitHeld ? "off" : "on"); break;
        case 9: line = std::format(R"({{"cmd":"icons","arg":"{}"}})", s.iconsHidden ? "off" : "on"); break;
        case 10: line = R"({"cmd":"quit"})"; break;
        default: return false;
    }
    // The row that was clicked and the command it turned into, both in the log: whether a pick
    // reached the renderer at all, and with which id, is otherwise only observable from the desktop.
    Info("tray", "menu pick row {} -> {}", n, line);
    app_->Command(line);
    return true;
}

} // namespace sw
