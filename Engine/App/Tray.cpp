#include "Engine/App/Tray.hpp"
#include "Engine/App/Application.hpp"
#include "Engine/Core/Json.hpp"
#include "Engine/Core/Log.hpp"

#include <shlobj.h>

namespace sw {
static constexpr const char* MOD = "tray";

namespace {

// Row geometry, measured off his reference menu (the 135x266 crop): plate #F9F9F9 starting at y=5,
// three 1 px #D8D8D8 rules at y=57/158/236, four groups of 2/4/3/1 rows -> 25~26 px per row. The
// QMenu stylesheet in Wallpaper/TrayMenu.cpp says 22; the picture says 25, and the picture is what
// he compares against.
constexpr int kRowH = 25;
constexpr int kSepH = 1;
constexpr int kPadL = 13;      // QMenu::item padding-left
constexpr int kCheckW = 15;    // the column the tick lives in
constexpr int kCheckGap = 4;   // "the check gap = left padding + 4 (so 13 -> 17)"
constexpr int kPadR = 24;      // QMenu::item padding-right

const COLORREF kPlate = RGB(0xF9, 0xF9, 0xF9);
const COLORREF kSel = RGB(0xD7, 0xE6, 0xFD);
const COLORREF kRule = RGB(0xD8, 0xD8, 0xD8);
const COLORREF kInk = RGB(0x1E, 0x1E, 0x1E);
const COLORREF kInkOff = RGB(0xA0, 0xA0, 0xA0);

constexpr UINT kFirstCmd = 0x4D00;
constexpr UINT kByeThread = WM_APP + 0x41;
constexpr const wchar_t* kMenuClass = L"SmartWallpaperTrayMenu";

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

void Tray::Paint(LPDRAWITEMSTRUCT dis, const Item& it) const {
    HDC dc = dis->hDC;
    const RECT r = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;

    HBRUSH plate = CreateSolidBrush(selected ? kSel : kPlate);
    FillRect(dc, &r, plate);
    DeleteObject(plate);

    if (it.sep) {   // the plate fills the band; the rule is the one line through its middle
        HPEN pen = CreatePen(PS_SOLID, 1, kRule);
        HGDIOBJ old = SelectObject(dc, pen);
        const int y = r.top + (r.bottom - r.top) / 2;
        MoveToEx(dc, r.left + 1, y, nullptr);
        LineTo(dc, r.right - 1, y);
        SelectObject(dc, old);
        DeleteObject(pen);
        return;
    }

    HGDIOBJ oldF = SelectObject(dc, MenuFont());
    SetBkMode(dc, TRANSPARENT);
    if (it.checked) {
        SetTextColor(dc, it.enabled ? kInk : kInkOff);
        RECT cr{ r.left + kPadL, r.top, r.left + kPadL + kCheckW, r.bottom };
        DrawTextW(dc, L"✓", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(dc, it.enabled ? kInk : kInkOff);
    RECT tr{ r.left + kPadL + kCheckW + kCheckGap, r.top, r.right - kPadR, r.bottom };
    DrawTextW(dc, it.label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldF);
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
            // Foreground rights follow the thread that got the click, and the icon is registered to
            // this window - so this is the thread that is allowed to ask.
            SetForegroundWindow(h);

            TrayState s = self->Snapshot();
            s.zh = LanguageIsZh();
            std::vector<Item> items = self->Build(s);

            HMENU menu = CreatePopupMenu();
            for (size_t i = 0; i < items.size(); ++i)
                AppendMenuW(menu, MF_OWNERDRAW, kFirstCmd + static_cast<UINT>(i),
                            reinterpret_cast<LPWSTR>(&items[i]));
            // `items` has to outlive the tracking call: the itemData pointers go straight into it.
            const UINT picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                                               pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (picked >= kFirstCmd) {
                const size_t idx = picked - kFirstCmd;
                // `id`, not the position in `items`: the vector carries the separators too, so
                // every row after a group line would otherwise be off by how many came before it.
                if (idx < items.size() && items[idx].enabled)
                    PostMessageW(self->owner_, WM_TRAY_RESULT, static_cast<WPARAM>(items[idx].id), 0);
            }
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            auto* it = reinterpret_cast<Item*>(mi->itemData);
            mi->itemHeight = it->sep ? kSepH : kRowH;
            if (it->sep) { mi->itemWidth = 1; return TRUE; }
            HDC dc = GetDC(h);
            HGDIOBJ oldF = SelectObject(dc, MenuFont());
            SIZE sz{};
            GetTextExtentPoint32W(dc, it->label.c_str(), static_cast<int>(it->label.size()), &sz);
            SelectObject(dc, oldF);
            ReleaseDC(h, dc);
            mi->itemWidth = kPadL + kCheckW + kCheckGap + sz.cx + kPadR;
            return TRUE;
        }
        case WM_DRAWITEM: {
            if (!self) return FALSE;
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            self->Paint(dis, *reinterpret_cast<Item*>(dis->itemData));
            return TRUE;
        }
        case kByeThread:
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
