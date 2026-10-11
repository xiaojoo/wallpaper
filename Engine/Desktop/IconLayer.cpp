#include "Engine/Desktop/IconLayer.hpp"

#include "Engine/Core/Log.hpp"

#include <format>

namespace sw {
namespace iconlayer {
namespace {

constexpr const wchar_t* kAdvanced =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr const wchar_t* kHideValue = L"HideIcons";

// One-way on purpose: this file never creates the value, only writes it if it is already there. A
// setting explorer has never had on this machine is not ours to invent - the menu item we are trying
// to stay honest with would not know what to do with it either.
bool WriteShellState(DWORD v) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvanced, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(key, kHideValue, 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

} // namespace

int ShellState() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvanced, 0, KEY_READ, &key) != ERROR_SUCCESS) return -1;
    DWORD v = 0xFFFFFFFFu, type = 0, size = sizeof(v);
    const LONG r = RegQueryValueExW(key, kHideValue, nullptr, &type,
                                    reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD || v > 1) return -1;
    return static_cast<int>(v);
}

bool Hidden(HWND list) { return list && !IsWindowVisible(list); }

bool Hide(HWND list, std::string& error) {
    if (!list) { error = "no icon list window to hide"; return false; }
    if (!WriteShellState(1))
        error = "HideIcons could not be written - the desktop menu will not agree with what we did";
    if (!IsWindowVisible(list)) return true;
    if (!ShowWindow(list, SW_HIDE))
        error = std::format("ShowWindow(SW_HIDE) on the icon list reported failure: {}", GetLastError());
    return true;
}

bool Show(HWND list, std::string& error) {
    if (!list) { error = "no icon list window to show"; return false; }
    // Written before the window is shown and regardless of whether this process hid it: the point of
    // this call is that the shell's own state stops claiming the icons are hidden.
    if (!WriteShellState(0))
        error = "HideIcons could not be written back to 0";
    if (IsWindowVisible(list)) return true;
    if (!ShowWindow(list, SW_SHOW))
        error = std::format("ShowWindow(SW_SHOW) on the icon list reported failure: {}", GetLastError());
    // The list has to be told to paint again: it was hidden while its parent was not, and explorer
    // does not notice a sibling window coming back.
    InvalidateRect(list, nullptr, TRUE);
    return true;
}

} // namespace iconlayer
} // namespace sw
