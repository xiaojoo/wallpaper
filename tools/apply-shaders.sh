#!/bin/bash
# Apply wallpaper shader/param edits to the RUNNING renderer and regenerate its preview images.
#
# Why a script: the renderer only re-reads Wallpapers/ and Shaders/ at startup, and its in-memory
# thumbnail map means `--ctl reload` never rebuilds cache/thumbs - so the only path from an .hlsl
# edit to a picture you can measure is stage + restart. Skipping the staging step (or building only
# the WallpaperRenderer target) silently measures the OLD shader.
#
#   bash tools/apply-shaders.sh                 # all packages
#   bash tools/apply-shaders.sh datarain embers # just these
set -u
cd /h/wallpaper || exit 1
BIN=/h/wallpaper/bld/bin/RelWithDebInfo
FXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
WALLS=("$@")
if [ ${#WALLS[@]} -eq 0 ]; then
  WALLS=()
  for d in Wallpapers/*/; do WALLS+=("$(basename "$d")"); done
fi

export MSYS_NO_PATHCONV=1
fail=0
for w in "${WALLS[@]}"; do
  [ -f "Wallpapers/$w/main.hlsl" ] || { echo "SKIP $w (no main.hlsl)"; continue; }
  if ! "$FXC" /nologo /Tps_5_0 /EPSMain /I"Shaders" "Wallpapers/$w/main.hlsl" >/dev/null 2>"build/fxc_$w.txt"; then
    echo "COMPILE FAIL $w"; head -20 "build/fxc_$w.txt"; fail=1; continue
  fi
  echo "ps_5_0 ok   $w"
done
[ $fail -eq 1 ] && { echo "aborted before staging - nothing of yours was touched"; exit 1; }

# Copy, then prove the runtime copy is byte-identical. cmp against a wrong relative path once
# reported "not staged" for files that were fine, so the check uses absolute paths.
cp -r Wallpapers/. "$BIN/Wallpapers/" && cp -r Shaders/. "$BIN/Shaders/"
for w in "${WALLS[@]}"; do
  [ -f "Wallpapers/$w/main.hlsl" ] || continue
  cmp -s "/h/wallpaper/Wallpapers/$w/main.hlsl" "$BIN/Wallpapers/$w/main.hlsl" \
    && echo "staged        $w" || { echo "STAGE MISMATCH $w"; exit 1; }
done

# Restart the one renderer we know about, by image name, and come back on the same command line it
# was started with. tasklist's //FI filter is unreliable from here; ask PowerShell for the pid.
# MSYS_NO_PATHCONV is already set, so taskkill wants single slashes and PowerShell wants a real
# Windows path - a '/h/...' WorkingDirectory makes Start-Process fail with DirectoryNotFound.
WINBIN='H:\wallpaper\bld\bin\RelWithDebInfo'
PID=$(powershell -NoProfile -Command "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id" | tr -d '\r ')
if [ -z "$PID" ]; then
  echo "no renderer running - starting one"; else
  taskkill /PID "$PID" /F >/dev/null && echo "stopped pid $PID"
fi
powershell -NoProfile -Command "Start-Process -FilePath '$WINBIN\WallpaperRenderer.exe' -ArgumentList '--no-tray' -WorkingDirectory '$WINBIN'" >/dev/null
sleep 12
NEW=$(powershell -NoProfile -Command "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id" | tr -d '\r ')
echo "renderer pid $PID -> $NEW"
for w in "${WALLS[@]}"; do
  [ -f "$BIN/cache/thumbs/${w}_large.png" ] && printf "thumb %s  %s\n" "$w" \
    "$(stat -c %y "$BIN/cache/thumbs/${w}_large.png" 2>/dev/null | cut -c1-19)"
done
