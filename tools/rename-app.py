#!/usr/bin/env python3
r"""tools/rename-app.py - one-shot rename of the *displayed* program name, SmartWallpaper -> Wallpaper.

Deliberately narrow. What gets renamed is what a person reads: the settings window's title, the tray
tooltip, the message boxes, the usage banner, the executable target, and the tools and docs that
quote those. What does NOT get renamed is the set of identifiers two running processes agree on:

  \\.\pipe\SmartWallpaper.Renderer   the control pipe        Local\SmartWallpaper.Renderer  the mutex
  Local\SmartWallpaper.Preview       the preview section     SmartWallpaper.Renderer        the Run value
  SmartWallpaper / ui.ini            QSettings: lang, favorites, target
  SmartWallpaperApp, SmartWallpaperDesktop, SmartWallpaperPowerNotify   registered window classes

Renaming the QSettings pair would silently reset his language, favorites and target monitor to
defaults on next start; renaming the pipe or the mutex would let a second instance start and split
connections. Each is a one-line change plus a migration if he ever wants it - not a side effect of a
cosmetic pass.

Every replacement asserts its expected hit count and aborts before writing anything if one differs,
so a file that changed under us fails loudly instead of half-editing.
"""
import io
import sys

EDITS = [
    # file, old, new, expected count
    ("CMakeLists.txt", "qt_add_executable(SmartWallpaper WIN32", "qt_add_executable(Wallpaper WIN32", 1),
    ("CMakeLists.txt", "target_include_directories(SmartWallpaper", "target_include_directories(Wallpaper", 1),
    ("CMakeLists.txt", "target_compile_definitions(SmartWallpaper", "target_compile_definitions(Wallpaper", 1),
    ("CMakeLists.txt", "target_compile_options(SmartWallpaper", "target_compile_options(Wallpaper", 1),
    ("CMakeLists.txt", "target_link_libraries(SmartWallpaper", "target_link_libraries(Wallpaper", 1),
    ("CMakeLists.txt", "add_custom_command(TARGET SmartWallpaper", "add_custom_command(TARGET Wallpaper", 1),
    ("CMakeLists.txt", '"$<TARGET_FILE_DIR:SmartWallpaper>/qml"', '"$<TARGET_FILE_DIR:Wallpaper>/qml"', 1),
    ("CMakeLists.txt", "set_target_properties(SmartWallpaper", "set_target_properties(Wallpaper", 1),
    ("CMakeLists.txt", "building SmartWallpaper settings window", "building Wallpaper settings window", 1),
    ("CMakeLists.txt", "skipping the SmartWallpaper settings window", "skipping the Wallpaper settings window", 1),

    ("Wallpaper/Bridge.cpp", '{"app_title", "SmartWallpaper 壁纸设置", "SmartWallpaper settings"}',
     '{"app_title", "Wallpaper 壁纸设置", "Wallpaper settings"}', 1),
    ("main.cpp", '"SmartWallpaper renderer\\n"', '"Wallpaper renderer\\n"', 1),
    ("main.cpp", '"SmartWallpaper could not start: "', '"Wallpaper could not start: "', 1),
    ("main.cpp", 'MessageBoxA(nullptr, text.c_str(), "SmartWallpaper"',
     'MessageBoxA(nullptr, text.c_str(), "Wallpaper"', 2),
    ("Engine/App/Tray.cpp", 'nid_.szTip, L"SmartWallpaper"', 'nid_.szTip, L"Wallpaper"', 1),
    ("Engine/App/Application.cpp", '"SmartWallpaper - {}"', '"Wallpaper - {}"', 1),

    ("tools/launch-ui.ps1", "\\SmartWallpaper.exe", "\\Wallpaper.exe", 1),
    ("tools/dump-proc-cmd.ps1", '"SmartWallpaper.exe"', '"Wallpaper.exe"', 1),
    ("tools/guarded-click.ps1", "$Title = 'SmartWallpaper'", "$Title = 'Wallpaper'", 1),
    ("tools/close-window-by-title.ps1", '$Title = "SmartWallpaper"', '$Title = "Wallpaper"', 1),
    ("tools/cadence-screen.ps1", "'SmartWallpaper'", "'Wallpaper'", 1),
    ("tools/find-named-tray-element.ps1", '$Match = "SmartWallpaper"', '$Match = "Wallpaper"', 1),
    ("tools/probe-crash-events.ps1", "WallpaperRenderer|SmartWallpaper", "WallpaperRenderer|Wallpaper", 1),

    ("README.md", "# SmartWallpaper — Windows 动态壁纸引擎 V0.1", "# Wallpaper — Windows 动态壁纸引擎 V0.1", 1),
    ("README.md", "SmartWallpaper.exe --select", "Wallpaper.exe --select", 1),
    ("README.md", "SmartWallpaper.exe --settings         #", "Wallpaper.exe --settings         #", 1),
    ("README.md", "SmartWallpaper.exe --settings-tab", "Wallpaper.exe --settings-tab", 1),
    ("README.md", "Get-Process SmartWallpaper", "Get-Process Wallpaper", 1),
]


def main():
    texts = {}
    for path, old, new, want in EDITS:
        if path not in texts:
            texts[path] = io.open(path, encoding="utf-8", newline="").read()
        got = texts[path].count(old)
        if got != want:
            print("ABORT %s: %d hits for %r, expected %d" % (path, got, old[:48], want))
            sys.exit(1)
        texts[path] = texts[path].replace(old, new)
    for path, text in texts.items():
        io.open(path, "w", encoding="utf-8", newline="").write(text)
        print("rewrote %s" % path)
    print("done: %d replacements across %d files" % (len(EDITS), len(texts)))


main()
