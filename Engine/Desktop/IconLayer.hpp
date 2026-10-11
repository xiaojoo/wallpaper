#pragma once
// Engine/Desktop/IconLayer.hpp - hiding the desktop's real shortcuts, and giving them back.
//
// The ring is meant to replace the icons the shell draws, so the shell's have to go. Two ways to do
// that were measured on this machine with tools/probe-icon-restore.ps1, and neither one alone is
// enough:
//
//   ShowWindow(SW_HIDE) on the icon list            - the icons go away, but the desktop's own
//       显示桌面图标 item still reads [On] and explorer's HideIcons value stays 0. The shell does not
//       know it happened, so the checkmark lies, and the user's one obvious way back is not a way back.
//
//   explorer's own HideIcons value                  - that is what the menu item writes (measured:
//       toggling it moved HideIcons from 0 to 1), so this is the state the shell agrees it owns. But
//       writing it at runtime does not restyle a view that is already up.
//
// So this does both, which is what makes the restore path real: after we hide, the item reads [Off],
// and one click on it - by the user, in a session where our process is a memory of a crash - puts
// HideIcons back to 0 and shows the list again. That is the only recovery route that does not depend
// on us being alive, which is the whole requirement.
//
// Hiding the *list* rather than SHELLDLL_DefView is deliberate too: with the list hidden the desktop's
// context menu still appears (measured - 10 items, 查看 among them), so the menu that restores the
// icons is reachable while they are gone. Hiding DefView takes that menu away with it.

#include "Engine/Core/Platform.hpp"

namespace sw {

namespace iconlayer {

// What explorer's own 显示桌面图标 item reads: 0 shown, 1 hidden, -1 unreadable.
int ShellState();

// Both take DesktopHost::iconList. They move the shell's state and the window together; either half
// alone leaves the two disagreeing, which is the bug this file exists to avoid.
bool Hide(HWND list, std::string& error);
bool Show(HWND list, std::string& error);

// Whether the icons are off the screen right now, as the window says it.
bool Hidden(HWND list);

} // namespace iconlayer

} // namespace sw
