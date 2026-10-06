#include "Engine/App/Tray.hpp"
#include "Engine/App/Application.hpp"
#include "Engine/Core/Json.hpp"
#include "Engine/Core/Log.hpp"

#include <shellapi.h>

namespace sw {
static constexpr const char* MOD = "tray";

namespace {
constexpr UINT kFirstCmd = 0x4D00;
constexpr UINT kSeparatorTag = 0;
} // namespace

bool Tray::Install(HWND owner, Application* app, std::string& error) {
    owner_ = owner;
    app_ = app;
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = owner_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uVersion = NOTIFYICON_VERSION_4;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION); // a real brand icon is a V0.2 item, not a blocker
    if (!nid_.hIcon) {
        error = "LoadIcon failed";
        return false;
    }
    wcscpy_s(nid_.szTip, L"SmartWallpaper");
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        error = "Shell_NotifyIcon(NIM_ADD) failed";
        return false;
    }
    installed_ = true;
    Info(MOD, "tray icon added");
    return true;
}

void Tray::Remove() {
    if (installed_) {
        nid_.uFlags = NIF_MESSAGE;
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        installed_ = false;
    }
}

void Tray::SetTip(const std::string& tip) {
    if (!installed_) return;
    std::wstring w = ToWide(tip);
    if (w.size() > 127) w.resize(127);
    wcsncpy_s(nid_.szTip, w.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::Notify(const std::string& title, const std::string& text) {
    if (!installed_) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, ToWide(title).c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, ToWide(text).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void Tray::Add(HMENU menu, UINT& nextId, Act act, const std::string& arg, const std::wstring& label, bool checked) {
    MENUITEMINFOW mii{};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE;
    mii.wID = nextId;
    mii.dwTypeData = const_cast<LPWSTR>(label.c_str());
    mii.cch = (UINT)label.size();
    mii.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
    AppendMenuW(menu, MF_STRING, nextId, label.c_str());
    commands_[nextId] = {act, arg};
    ++nextId;
    (void)mii;
}

void Tray::ShowMenu(HWND owner) {
    commands_.clear();
    UINT id = kFirstCmd;

    std::string listJson = app_ ? app_->Command(R"({"cmd":"list"})") : "{}";
    std::string statusJson = app_ ? app_->Command(R"({"cmd":"status"})") : "{}";
    std::string err;
    auto list = Json::Parse(listJson, err);
    auto status = Json::Parse(statusJson, err);

    std::string activeWallpaper, activeMonitor = activeMonitor_, quality;
    long long maxFps = 60;
    bool paused = false;
    if (list && list->isObject()) {
        quality = list->strOr("quality", "");
        maxFps = list->find("max_fps") ? list->find("max_fps")->asInt64(60) : 60;
        if (const Json* a = list->find("assignments"); a && a->isArray() && !a->items().empty()) {
            const Json& first = a->items().front();
            activeWallpaper = first.strOr("wallpaper", "");
            if (activeMonitor_.empty()) activeMonitor_ = first.strOr("monitor", "all");
        }
        if (activeMonitor_.empty()) activeMonitor_ = "all";
    }
    if (status && status->isObject()) paused = status->find("paused") ? status->find("paused")->asBool() : false;

    HMENU menu = CreatePopupMenu();

    if (list && list->isObject() && list->find("catalog")) {
        for (auto& w : list->find("catalog")->items()) {
            std::string wid = w.strOr("id", "");
            std::string name = w.strOr("name", wid);
            Add(menu, id, Act::Wall, wid + "|" + activeMonitor, ToWide(name), wid == activeWallpaper);
        }
    } else {
        Add(menu, id, Act::Nop, "", L"(no wallpapers found)", false);
    }

    AppendMenuW(menu, MF_SEPARATOR, kSeparatorTag, nullptr);
    if (status && status->isObject() && status->find("monitors")) {
        for (auto& m : status->find("monitors")->items()) {
            std::string tag = m.strOr("tag", "?");
            std::string dev = m.strOr("device", "");
            std::string px = m.strOr("px", "");
            Add(menu, id, Act::Mon, tag, ToWide(tag + "  " + dev + "  " + px), tag == activeMonitor);
        }
        Add(menu, id, Act::Mon, "all", L"All monitors", activeMonitor == "all");
    }

    AppendMenuW(menu, MF_SEPARATOR, kSeparatorTag, nullptr);
    for (const char* q : {"package", "ultra", "high", "medium", "low", "battery"})
        Add(menu, id, Act::Qual, q, ToWide(std::string("Quality: ") + q), q == quality);

    AppendMenuW(menu, MF_SEPARATOR, kSeparatorTag, nullptr);
    for (int f : {240, 144, 120, 90, 60, 30, 15})
        Add(menu, id, Act::Qual, "fps|" + std::to_string(f), ToWide("Max FPS: " + std::to_string(f)),
            (long long)f == maxFps);

    AppendMenuW(menu, MF_SEPARATOR, kSeparatorTag, nullptr);
    Add(menu, id, paused ? Act::Resume : Act::Pause, "", ToWide(paused ? "Resume rendering" : "Pause rendering"),
        false);
    Add(menu, id, Act::Reload, "", L"Reload wallpapers and config", false);
    Add(menu, id, Act::Status, "", L"Write status to log", false);
    Add(menu, id, Act::Quit, "", L"Quit", false);

    SetForegroundWindow(owner);
    POINT pt{};
    GetCursorPos(&pt);
    UINT trace = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
    if (trace) HandleCommand(trace);
}

bool Tray::HandleCommand(UINT id) {
    auto it = commands_.find(id);
    if (it == commands_.end()) return false;
    Act act = it->second.first;
    std::string arg = it->second.second;

    auto ask = [&](const std::string& line) {
        return app_ ? app_->Command(line) : std::string("{}");
    };

    switch (act) {
        case Act::Wall: {
            size_t bar = arg.find('|');
            std::string wid = bar == std::string::npos ? arg : arg.substr(0, bar);
            std::string mon = bar == std::string::npos ? "all" : arg.substr(bar + 1);
            Json q = Json::Object();
            q.set("cmd", Json::Of("apply"));
            q.set("arg", Json::Of(wid));
            q.set("arg2", Json::Of(mon));
            ask(q.dump());
            Notify("Wallpaper applied", wid + " -> " + mon);
            return true;
        }
        case Act::Mon:
            activeMonitor_ = arg; // tray-local: it only decides where the next pick goes
            return true;
        case Act::Qual: {
            std::string action = "quality", a = arg, b;
            if (size_t bar = arg.find('|'); bar != std::string::npos) { a = arg.substr(0, bar); b = arg.substr(bar + 1); }
            if (a == "fps") { action = "fps"; a = b; }
            Json q = Json::Object();
            q.set("cmd", Json::Of(action));
            q.set("arg", Json::Of(a));
            ask(q.dump());
            return true;
        }
        case Act::Pause: ask(R"({"cmd":"pause"})"); return true;
        case Act::Resume: ask(R"({"cmd":"resume"})"); return true;
        case Act::Reload:
            ask(R"({"cmd":"reload"})");
            Notify("Reloaded", "wallpaper catalog and config rescanned");
            return true;
        case Act::Status: {
            std::string s = ask(R"({"cmd":"status"})");
            Info(MOD, "tray status: {}", s);
            Notify("Status written to the log", s.size() > 200 ? s.substr(0, 200) + "..." : s);
            return true;
        }
        case Act::Quit:
            ask(R"({"cmd":"quit"})");
            return true;
        case Act::Nop: return true;
    }
    return false;
}

} // namespace sw
