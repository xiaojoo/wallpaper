#pragma once
// Engine/Performance/PowerManager.hpp - decides how many frames a wallpaper may spend.
#include "Engine/Core/Json.hpp"
#include "Engine/Core/Platform.hpp"
#include "Engine/Desktop/MonitorManager.hpp"

namespace sw {

enum class PowerState { Desktop, Background, Covered, Fullscreen, Locked, DisplayOff };

struct PowerCaps {
    int desktop = 60;      // nothing in front of the wallpaper
    // 10 -> 30 -> 60 for this one. The animation's speed over a second does not depend on the cap
    // (measured: 22-26% of pixels change per second at 10, 30 and 60 alike), but the size of each
    // visible step does - at 10 fps every step carries 100 ms of motion. It stopped at 30 because a
    // cap was thought to be saving work, but "background" fires for any window that merely overlaps
    // the monitor, so it is the steady state while the desktop is still visible all around that
    // window - and every 60 <-> 30 move halves or doubles the motion in one frame (32 budget moves
    // in a 5.6 h run). Visible costs full rate; only covered and fullscreen, where it cannot be seen,
    // drop. FPSController then snaps these to whole refresh periods (60 -> 48 on a 144 Hz panel).
    int background = 60;   // another program has focus but the desktop is still partly visible
    int covered = 24;      // a window sits over this monitor
    int fullscreen = 0;    // a fullscreen game/app owns this monitor
    int locked = 0;        // session locked
    int displayOff = 0;    // monitor powered down
    int onBattery = 30;    // upper bound while not on AC
    int batterySaver = 15; // upper bound while battery saver is on
};

class PowerManager {
public:
    struct Decision {
        PowerState state = PowerState::Desktop;
        int cap = 60;
        std::string why;
    };

    bool Init(std::string& error);
    void Shutdown();

    void LoadCaps(const Json& j);
    // Used by the settings window; returns false for an unknown key so the UI cannot invent one.
    bool SetCap(const std::string& key, int value);
    Json SaveCaps() const;
    PowerCaps& caps() { return caps_; }
    const PowerCaps& caps() const { return caps_; }

    // Cheap refresh of foreground geometry, session and battery state.
    void Poll(ULONGLONG nowMs);
    Decision Decide(const MonitorInfo& monitor) const;

    static const char* StateName(PowerState s);
    bool Interactive() const { return !locked_ && !displayOff_; }
    std::string StatusLine() const;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void UpdatePowerSource();
    void UpdateForeground();

    HWND win_ = nullptr;
    std::vector<HANDLE> powerNotes_; // HPOWERNOTIFY handles, required to unregister
    PowerCaps caps_{};
    bool locked_ = false;
    bool displayOff_ = false;
    bool onBattery_ = false;
    bool batterySaver_ = false;
    ULONGLONG lastPoll_ = 0;
    struct Fore {
        HWND hwnd = nullptr;
        Rect rect{};
        bool maximized = false;
        bool popup = false;
        bool shell = false;   // the desktop's own window chain, which can never cover the wallpaper
    } fore_;
};

} // namespace sw
