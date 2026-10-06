#!/bin/bash
set -u
cd /h/wallpaper
mkdir -p build
if [ ! -f build/vc.env ] || [ CMakeLists.txt -nt build/vc.env ]; then
  winenv='cmd.exe'
  "$winenv" /c 'call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >/dev/null 2>&1 && set' > build/vc.env 2>/dev/null
fi
while IFS= read -r line; do
  case "$line" in
    [A-Za-z_]*=*) export "$line" ;;
  esac
done < build/vc.env
echo "cl: $(command -v cl || echo MISSING)"
echo "ninja: $(command -v ninja || echo MISSING)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo > build/configure.log 2>&1
rc=$?
if [ $rc -ne 0 ]; then echo "CONFIGURE_FAILED rc=$rc"; tail -40 build/configure.log; exit $rc; fi
cmake --build build > build/build.log 2>&1
rc=$?
echo "BUILD_RC=$rc"
if [ $rc -ne 0 ]; then grep -E "error|fatal" build/build.log | head -60; fi
