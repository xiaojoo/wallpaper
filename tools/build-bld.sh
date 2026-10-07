#!/bin/bash
# One shot: stop the running renderer, build the tree his desktop exe actually comes from, prove the
# binary is newer, then start it again on the same command line it was started with.
# Why bld/ and not tools/build.sh: bld/ is the Visual Studio generator tree whose output is
# bld/bin/RelWithDebInfo/*.exe - the files that run on the desktop. build/ is a Ninja tree with no
# bin/ at all, so building it silently changes nothing he can see.
set -u
cd /h/wallpaper || exit 1
CMAKE=/d/Program/CMake/bin/cmake
BIN='H:\wallpaper\bld\bin\RelWithDebInfo'
LOG=build/bld_p1.log

PID=$(powershell -NoProfile -Command "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id" | tr -d '\r ')
echo "renderer pid before: ${PID:-none}"
BEFORE=$(stat -c %Y bld/bin/RelWithDebInfo/WallpaperRenderer.exe)

if [ -n "$PID" ]; then
  # MSYS rewrites /PID into a path, so the kill goes through PowerShell and is verified: an
  # unverified kill cost a LNK1168 link against the still-running exe.
  powershell -NoProfile -Command "Stop-Process -Id $PID -Force" 2>/dev/null
  sleep 2
  STILL=$(powershell -NoProfile -Command "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id" | tr -d '\r ')
  if [ -n "$STILL" ]; then echo "STILL RUNNING: $STILL - aborting before the link"; exit 9; fi
  echo "stopped $PID"
fi

"$CMAKE" --build bld --config RelWithDebInfo --target WallpaperRenderer > "$LOG" 2>&1
RC=$?
echo "BUILD_RC=$RC"
if [ $RC -ne 0 ]; then
  grep -E "error|fatal|LNK" "$LOG" | head -30
  echo "NOT restarting - the exe on disk is the old one"
  exit $RC
fi
AFTER=$(stat -c %Y bld/bin/RelWithDebInfo/WallpaperRenderer.exe)
echo "exe mtime before=$BEFORE after=$AFTER newer=$([ "$AFTER" -gt "$BEFORE" ] && echo yes || echo NO)"

powershell -NoProfile -Command "Start-Process -FilePath '$BIN\WallpaperRenderer.exe' -WorkingDirectory '$BIN'" >/dev/null
sleep 8
NEW=$(powershell -NoProfile -Command "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id" | tr -d '\r ')
echo "renderer pid after: $NEW"
