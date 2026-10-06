#!/bin/bash
# Stages qml/ + runs the settings window with Qt's bin on PATH. Edits to Main.qml only need this
# script again, not a rebuild.
set -u
QT=/d/Program/Qt/6.11.2/msvc2022_64
BIN=/h/wallpaper/bld/bin/RelWithDebInfo
"/d/Program/CMake/bin/cmake" -E copy_directory /h/wallpaper/SmartWallpaper/qml "$BIN/qml" >/dev/null
cd "$BIN" || exit 1
export PATH="$QT/bin:$PATH"
export QML_IMPORT_PATH="$QT/qml"
if [ "${1:-}" = "--with-renderer" ]; then
  ./WallpaperRenderer.exe --no-tray >/dev/null 2>&1 &
  sleep 2
fi
./SmartWallpaper.exe "$@"
