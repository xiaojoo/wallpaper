#include "Engine/Performance/PowerManager.hpp"
#include "Engine/Core/Log.hpp"

#include <wtsapi32.h>
#include <powrprof.h>

namespace sw {
static constexpr const char* MOD = "power";

namespace {
constexpr const wchar_t* kNotifyClass = L"SmartWallpaperPowerNotify";

struct Self {
    PowerManager* self = nullptr;
};
Self g_self;

bool WindowCoversMonitor(const Rect& win, const MonitorInfo& m) {
    // A window "owns" the monitor when it lands exactly on its bounds; anything smaller is a
    // normal window and only covers part of it.
    return win.w >= m.px.w && win.h >= m.px.h && win.x <= m.px.x && win.y <= m.px.y &&
           win.x + win.w >= m.px.x + m.px.w && win.y + win.h >= m.px.y + m.px.h;
}

// Progman and everything parented to it (SHELLDLL_DefView, SysListView32, the wallpaper WorkerW)
// is the surface the wallpaper is composited into, so it can never be what covers the wallpaper.
// Progman is a WS_POPUP whose rect is exactly the monitor, so without this a plain click on the
// desktop reads as "a fullscreen game took over" and the animation stops at 0 FPS.
bool IsShellDesktop(HWND h) {
    const HWND shell = GetShellWindow();
    if (!shell || !h) return false;
    for (HWND p = h; p && p != GetDesktopWindow();) {
        if (p == shell) return true;
        HWND next = GetAncestor(p, GA_PARENT);
        if (!next || next == p) break;
        p = next;
    }
    return false;
}

bool WindowTouchesMonitor(const Rect& win, const MonitorInfo& m) {
    return win.w > 0 && win.h > 0 && win.x < m.px.x + m.px.w && win.y < m.px.y + m.px.h &&
           win.x + win.w > m.px.x && win.y + win.h > m.px.y;
}
} // namespace

const char* PowerManager::StateName(PowerState s) {
    switch (s) {
        case PowerState::Desktop: return "desktop";
        case PowerState::Background: return "background";
        case PowerState::Covered: return "covered";
        case PowerState::Fullscreen: return "fullscreen";
        case PowerState::Locked: return "locked";
        case PowerState::DisplayOff: return "display_off";
    }
    return "?";
}

LRESULT CALLBACK PowerManager::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = g_self.self;
    if (self) {
        if (m == WM_WTSSESSION_CHANGE) {
            if (w == WTS_SESSION_LOCK) {
                self->locked_ = true;
                Info(MOD, "session locked -> paused");
            } else if (w == WTS_SESSION_UNLOCK) {
                self->locked_ = false;
                Info(MOD, "session unlocked -> resuming");
            } else if (w == WTS_SESSION_LOGOFF || w == WTS_SESSION_LOGON || w == WTS_CONSOLE_DISCONNECT ||
                       w == WTS_CONSOLE_CONNECT) {
                Info(MOD, "session event {} received", (unsigned)w);
            }
        } else if (m == WM_POWERBROADCAST && w == PBT_POWERSETTINGCHANGE) {
            auto* s = reinterpret_cast<POWERBROADCAST_SETTING*>(l);
            if (s && s->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) {
                self->displayOff_ = s->Data[0] == 0;
                Info(MOD, "console display state -> {}", s->Data[0] == 0 ? "off" : "on");
            } else if (s && s->PowerSetting == GUID_MONITOR_POWER_ON) {
                self->displayOff_ = s->Data[0] == 0;
                Info(MOD, "monitor power -> {}", s->Data[0] ? "on" : "off");
            } else if (s && s->PowerSetting == GUID_BATTERY_PERCENTAGE_REMAINING) {
                self->UpdatePowerSource();
            }
        }
    }
    return DefWindowProcW(h, m, w, l);
}

bool PowerManager::Init(std::string& error) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &PowerManager::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kNotifyClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = "power notify class: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }
        registered = true;
    }
    g_self.self = this;

    // A hidden top-level window (not message-only): session and power notifications are
    // delivered to top-level windows.
    win_ = CreateWindowExW(WS_EX_TOOLWINDOW, kNotifyClass, L"SmartWallpaper power", WS_POPUP, 0, 0, 0, 0, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!win_) {
        error = "power notify window: " + HResultToString(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    if (!WTSRegisterSessionNotification(win_, NOTIFY_FOR_THIS_SESSION))
        Warn(MOD, "WTSRegisterSessionNotification failed (err {}) - lock detection falls back to polling",
             GetLastError());
    auto note = [&](const GUID& g, const char* what, bool required) {
        HANDLE h = RegisterPowerSettingNotification(win_, &g, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!h) {
            if (required)
                Warn(MOD, "RegisterPowerSettingNotification({}) failed (err {}) - that power signal will not be seen",
                     what, GetLastError());
            else
                Info(MOD, "RegisterPowerSettingNotification({}) failed (err {})", what, GetLastError());
            return;
        }
        powerNotes_.push_back(h);
        Info(MOD, "watching {}", what);
    };
    note(GUID_CONSOLE_DISPLAY_STATE, "GUID_CONSOLE_DISPLAY_STATE", true);
    note(GUID_MONITOR_POWER_ON, "GUID_MONITOR_POWER_ON", false);
    note(GUID_BATTERY_PERCENTAGE_REMAINING, "GUID_BATTERY_PERCENTAGE_REMAINING", false);
    UpdatePowerSource();
    Info(MOD, "power monitor ready: {}", StatusLine());
    return true;
}

void PowerManager::Shutdown() {
    if (win_) {
        for (HANDLE h : powerNotes_) UnregisterPowerSettingNotification(h);
        powerNotes_.clear();
        WTSUnRegisterSessionNotification(win_);
        DestroyWindow(win_);
        win_ = nullptr;
    }
    g_self.self = nullptr;
}

void PowerManager::LoadCaps(const Json& j) {
    auto num = [&](const char* k, int def) {
        const Json* v = j.find(k);
        return v && v->isNumber() ? v->asInt(def) : def;
    };
    caps_.desktop = num("desktop_fps", caps_.desktop);
    caps_.background = num("background_fps", caps_.background);
    caps_.covered = num("covered_fps", caps_.covered);
    caps_.fullscreen = num("fullscreen_fps", caps_.fullscreen);
    caps_.locked = num("locked_fps", caps_.locked);
    caps_.displayOff = num("display_off_fps", caps_.displayOff);
    caps_.onBattery = num("battery_fps", caps_.onBattery);
    caps_.batterySaver = num("battery_saver_fps", caps_.batterySaver);
}

bool PowerManager::SetCap(const std::string& key, int value) {
    if (value < 0) value = 0;
    auto& c = caps_;
    if (key == "desktop_fps") c.desktop = value;
    else if (key == "background_fps") c.background = value;
    else if (key == "covered_fps") c.covered = value;
    else if (key == "fullscreen_fps") c.fullscreen = value;
    else if (key == "locked_fps") c.locked = value;
    else if (key == "display_off_fps") c.displayOff = value;
    else if (key == "battery_fps") c.onBattery = value;
    else if (key == "battery_saver_fps") c.batterySaver = value;
    else return false;
    Info(MOD, "cap {} = {}", key, value);
    return true;
}

Json PowerManager::SaveCaps() const {
    Json j = Json::Object();
    j.set("desktop_fps", Json::Of(caps_.desktop));
    j.set("background_fps", Json::Of(caps_.background));
    j.set("covered_fps", Json::Of(caps_.covered));
    j.set("fullscreen_fps", Json::Of(caps_.fullscreen));
    j.set("locked_fps", Json::Of(caps_.locked));
    j.set("display_off_fps", Json::Of(caps_.displayOff));
    j.set("battery_fps", Json::Of(caps_.onBattery));
    j.set("battery_saver_fps", Json::Of(caps_.batterySaver));
    return j;
}

void PowerManager::UpdatePowerSource() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) {
        onBattery_ = false;
        batterySaver_ = false;
        return;
    }
    onBattery_ = sps.ACLineStatus == 0; // 0 = offline (battery), 1 = online, 255 = unknown
    batterySaver_ = (sps.SystemStatusFlag & 1) != 0;
}

void PowerManager::UpdateForeground() {
    Fore f;
    f.hwnd = GetForegroundWindow();
    if (f.hwnd) {
        RECT r{};
        GetWindowRect(f.hwnd, &r);
        f.rect = Rect{r.left, r.top, r.right - r.left, r.bottom - r.top};
        DWORD_PTR st = GetWindowLongPtrW(f.hwnd, GWL_STYLE);
        f.maximized = (st & WS_MAXIMIZE) != 0;
        f.popup = (st & WS_POPUP) != 0;
        f.shell = IsShellDesktop(f.hwnd);
    }
    fore_ = f;
}

void PowerManager::Poll(ULONGLONG nowMs) {
    if (nowMs - lastPoll_ < 250) return; // four checks a second is enough to react and costs nothing
    lastPoll_ = nowMs;
    UpdateForeground();
    UpdatePowerSource();
}

PowerManager::Decision PowerManager::Decide(const MonitorInfo& m) const {
    Decision d;
    if (locked_) {
        d.state = PowerState::Locked;
        d.cap = caps_.locked;
        d.why = "session locked";
    } else if (displayOff_) {
        d.state = PowerState::DisplayOff;
        d.cap = caps_.displayOff;
        d.why = "display powered off";
    } else if (!fore_.hwnd || fore_.rect.w == 0) {
        d.state = PowerState::Desktop;
        d.cap = caps_.desktop;
        d.why = "no foreground window";
    } else if (fore_.shell) {
        d.state = PowerState::Desktop;
        d.cap = caps_.desktop;
        d.why = "desktop shell is foreground";
    } else if (WindowCoversMonitor(fore_.rect, m)) {
        d.state = fore_.popup || fore_.maximized ? PowerState::Fullscreen : PowerState::Covered;
        d.cap = d.state == PowerState::Fullscreen ? caps_.fullscreen : caps_.covered;
        d.why = std::format("foreground 0x{:X} covers the monitor ({}{})", (uintptr_t)fore_.hwnd,
                            fore_.popup ? "popup" : "window", fore_.maximized ? ", maximized" : "");
    } else if (WindowTouchesMonitor(fore_.rect, m)) {
        d.state = PowerState::Background;
        d.cap = caps_.background;
        d.why = std::format("0x{:X} overlaps this monitor", (uintptr_t)fore_.hwnd);
    } else {
        d.state = PowerState::Desktop;
        d.cap = caps_.desktop;
        d.why = "monitor is fully visible";
    }

    if (batterySaver_ && d.cap > caps_.batterySaver) {
        d.cap = caps_.batterySaver;
        d.why += ", battery saver cap";
    } else if (onBattery_ && d.cap > caps_.onBattery) {
        d.cap = caps_.onBattery;
        d.why += ", battery cap";
    }
    return d;
}

std::string PowerManager::StatusLine() const {
    return std::format("locked={} displayOff={} battery={} saver={} foreground=0x{:X} ({},{}) {}x{}", locked_,
                       displayOff_, onBattery_, batterySaver_, (uintptr_t)fore_.hwnd, fore_.rect.x, fore_.rect.y,
                       fore_.rect.w, fore_.rect.h);
}

} // namespace sw
