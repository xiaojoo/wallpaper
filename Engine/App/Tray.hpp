#pragma once
// Engine/App/Tray.hpp - tray icon plus the menu that drives it. The owner window receives
// WM_TRAYICON/WM_COMMAND and forwards them here, so one window serves icon, IPC wake and display
// change instead of three hidden ones.
#include "Engine/Core/Platform.hpp"
#include <shellapi.h>
#include <map>

namespace sw {

class Application;

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_IPC_WAKE = WM_APP + 2;
constexpr UINT WM_APP_QUIT = WM_APP + 3;

class Tray {
public:
    bool Install(HWND owner, Application* app, std::string& error);
    void Remove();

    // Built fresh on every click so the menu always shows the real catalog and assignment.
    void ShowMenu(HWND owner);
    // Returns true when the command id belongs to the tray menu.
    bool HandleCommand(UINT id);

    void SetTip(const std::string& tip);
    void Notify(const std::string& title, const std::string& text);
    bool installed() const { return installed_; }

private:
    enum class Act { Wall, Mon, Qual, Pause, Resume, Reload, Status, Quit, Nop };
    void Add( HMENU menu, UINT& nextId, Act act, const std::string& arg, const std::wstring& label, bool checked);

    NOTIFYICONDATAW nid_{};
    HWND owner_ = nullptr;
    Application* app_ = nullptr;
    bool installed_ = false;
    std::string activeMonitor_ = "all"; // which monitor the next wallpaper pick targets
    std::map<UINT, std::pair<Act, std::string>> commands_;
};

} // namespace sw
