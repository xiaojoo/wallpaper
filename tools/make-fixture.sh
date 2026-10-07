#!/bin/bash
# tools/make-fixture.sh - build and run the synthetic video fixture generator.
# One command recreates both clips the video wallpaper tests measure against.
# The compile itself is in tools/mkfixture-build.bat: Git Bash rewrites MSVC arguments that start
# with a slash, so cl has to be invoked from a batch file.
set -eu
cd /h/wallpaper
mkdir -p build/fixtures

( cd tools && cmd.exe //c mkfixture-build.bat ) | tr -d '\r'
[ -f build/mkfixture.exe ] || { echo "compile failed - see the cl output above"; exit 1; }

if [ "${1:-}" != "--build-only" ]; then
  # 10 s, 30 fps. The block crosses the frame once, so a seek to t lands on a predictable x.
  # Two sizes, so that "does the CPU pay for pixels" has two points to compare.
  build/mkfixture.exe H:/wallpaper/build/fixtures/fixture_1080.mp4 1920 1080 10 30 4000
  build/mkfixture.exe H:/wallpaper/build/fixtures/fixture_2160.mp4 3840 2160 10 30 14000
  ls -la build/fixtures
fi
