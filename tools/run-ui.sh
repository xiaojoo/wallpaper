#!/bin/bash
# Stages qml/ + the hero transition's compiled shader, then runs the settings window with Qt's bin
# on PATH. Edits to Main.qml or to shaders/*.frag only need this script again, not a rebuild.
set -u
QT=/d/Program/Qt/6.11.2/msvc2022_64
BIN=/h/wallpaper/bld/bin/RelWithDebInfo
"/d/Program/CMake/bin/cmake" -E copy_directory /h/wallpaper/Wallpaper/qml "$BIN/qml" >/dev/null
# Same flags Qt's own qt_add_shaders uses. The stage is taken from the file *extension*, so the
# source has to stay named .frag - a .qsl suffix makes qsb bake a vertex shader, which the scene
# graph then refuses with "Failed to create pixel shader" while QML still reports status Ready.
if [ -x "$QT/bin/qsb.exe" ]; then
  "$QT/bin/qsb.exe" --glsl "100es,120,150" --hlsl 50 --msl 12 \
    /h/wallpaper/Wallpaper/shaders/HeroLiquid.frag --o "$BIN/qml/HeroLiquid.frag.qsb" \
    && echo "shader staged" || { echo "SHADER COMPILE FAILED"; exit 1; }
else
  echo "no qsb.exe in $QT/bin - the staged shader next to the exe is whatever was there before"
fi
cd "$BIN" || exit 1
export PATH="$QT/bin:$PATH"
export QML_IMPORT_PATH="$QT/qml"
if [ "${1:-}" = "--with-renderer" ]; then
  ./WallpaperRenderer.exe --no-tray >/dev/null 2>&1 &
  sleep 2
fi
# The target was renamed SmartWallpaper -> Wallpaper, so pick whichever exe is actually here rather
# than failing with "no such file" on a tree that was built by the other name.
UI=$(ls -t Wallpaper.exe SmartWallpaper.exe 2>/dev/null | head -1)
if [ -z "${UI:-}" ]; then echo "no settings-window exe in $BIN - build it first"; exit 1; fi
echo "running $UI"
"./$UI" "$@"
