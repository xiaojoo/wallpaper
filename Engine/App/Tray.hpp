#pragma once
// Engine/App/Tray.hpp - the notify icon, and the menu that hangs off it.
//
// The icon belongs to a window on its OWN thread. That is not tidiness: TrackPopupMenu does not
// return while the menu is up, and with the icon on the renderer's window the whole draw loop was
// stuck for as long as the menu was open - measured at 17.45 s without a single `loop:` line on a
// live desktop. On its own thread the menu can block all it likes, and the shell's foreground
// permission (which a click in the notification area grants to the thread that RECEIVED the
// message) lands on the thread that needs it.
#include "Engine/Core/Platform.hpp"

#include <shellapi.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sw {

class Application;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_IPC_WAKE = WM_APP + 2;
constexpr UINT WM_APP_QUIT = WM_APP + 3;
// Menu thread -> the renderer's window: one pick, or "the user left-clicked the icon".
constexpr UINT WM_TRAY_RESULT = WM_APP + 0x31;
constexpr UINT WM_TRAY_OPEN_GUI = WM_APP + 0x32;

// Everything the menu needs to tick and grey its rows. The renderer is the only authority for all
// of it, so this is a copy rather than a request - and it is why the menu stays correct when no
// settings window is open at all.
struct TrayState {
    bool zh = true;                // the settings window's language switch, read from ui.ini
    bool rotateOn = false;
    bool paused = false;
    bool audioMuted = false;
    bool orbitOn = false;
    bool orbitHeld = false;
    bool iconsHidden = false;
    // The ring needs its icons read AND its shader compiled before switching it does anything; a
    // row that moves its tick and changes nothing is a dead control.
    bool orbitActionable = false;
    // A still picture has no motion to stop, so *starting* a pause would show nothing. Undoing one
    // is always actionable.
    bool pauseActionable = false;
};

class Tray {
public:
    struct Item {
        std::wstring label;
        UINT id = 0;
        bool checked = false;
        bool enabled = true;
        bool sep = false;
    };

    // exeDir is where Wallpaper.exe lives, so a left-click on the icon can open the settings window.
    bool Install(HWND owner, Application* app, const std::wstring& exeDir, std::string& error);
    void Remove();

    // The hover card: this process's memory, CPU share and GPU share, one per line. gpuPct below
    // zero means no answer, and the GPU line is then left out.
    void SetTip(double wsMb, double cpuPct, double gpuPct);
    bool installed() const { return installed_; }
    // Safe from any thread; the menu reads it when it opens.
    void Publish(const TrayState& s);
    // Runs on the renderer's thread. n is 1..10, the row order the menu was built in.
    bool HandleCommand(UINT n);

private:
    static LRESULT CALLBACK MenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    void ThreadMain();
    std::vector<Item> Build(const TrayState& s) const;
    void Paint(LPDRAWITEMSTRUCT dis, const Item& it) const;
    TrayState Snapshot() const;

    NOTIFYICONDATAW nid_{};
    HWND owner_ = nullptr;        // the renderer's window: results come back here
    // This thread's window; the icon is registered to IT. Atomic because Install() waits on it
    // from the renderer's thread while the menu thread is the one that creates it.
    std::atomic<HWND> menuWnd_{nullptr};
    Application* app_ = nullptr;
    std::wstring exeDir_;
    std::thread thread_;
    mutable std::mutex mtx_;
    TrayState state_;
    bool installed_ = false;
};

} // namespace sw
