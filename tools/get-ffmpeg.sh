#!/bin/bash
# tools/get-ffmpeg.sh - fetch the ONE FFmpeg build this project is allowed to link, and prove it.
#
# Why a script instead of vendoring: the shared build is 74 MB of third-party binaries, and LGPL
# compliance is about *which* build you ship (no --enable-gpl, no --enable-nonfree) rather than about
# the source tree. So the repo keeps this file plus the checks below, and the payload stays in the
# gitignored build/ tree.
#
#   bash tools/get-ffmpeg.sh            # download + unpack + verify into build/ffm-lgpl
set -u
cd /h/wallpaper || exit 1

VER=n9.0
URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-${VER}-latest-win64-lgpl-shared-${VER#n}.zip"
DST=build/ffm-lgpl
ZIP=build/ffm-dl/ffmpeg-lgpl-shared.zip

if [ -x "$DST/bin/ffmpeg.exe" ]; then
  echo "already unpacked: $DST"
else
  mkdir -p build/ffm-dl
  echo "fetching $URL"
  curl -fL --retry 3 -o "$ZIP" "$URL" || { echo "DOWNLOAD FAILED"; exit 1; }
  # Recorded because the URL sits under a rolling "latest" tag: once the upstream publishes a newer
  # build this exact archive may no longer be fetchable, and the checksum is then the only proof of
  # which bytes we shipped. tools/ffmpeg-notice.sh cites it.
  sha256sum "$ZIP" | cut -d' ' -f1 > "$ZIP.sha256"
  echo "  sha256 recorded: $(cat "$ZIP.sha256")"
  rm -rf "$DST.tmp" && mkdir -p "$DST.tmp"
  python - "$ZIP" "$DST.tmp" <<'PY' || exit 1
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
  inner=$(find "$DST.tmp" -maxdepth 1 -type d -name 'ffmpeg-*' | head -1)
  [ -n "$inner" ] || { echo "unexpected archive layout"; exit 1; }
  rm -rf "$DST"; mv "$inner" "$DST"; rm -rf "$DST.tmp"
fi

echo "=== license-critical checks (these decide what we may ship) ==="
CONF=$("$DST/bin/ffmpeg.exe" -hide_banner -buildconf 2>&1)
if echo "$CONF" | grep -q -- "--enable-gpl"; then
  echo "REFUSE: this build has --enable-gpl -> the whole library is GPL, not LGPL"; exit 2
fi
if echo "$CONF" | grep -q -- "--enable-nonfree"; then
  echo "REFUSE: this build has --enable-nonfree"; exit 2
fi
echo "  no --enable-gpl, no --enable-nonfree: OK"
echo "$CONF" | grep -oE -- "--enable-version3" >/dev/null \
  && echo "  NOTE: --enable-version3 present -> the effective license is LGPLv3 (not just 2.1+)" \
  || echo "  no --enable-version3: LGPL 2.1+ terms apply"

for what in h264_cuvid hevc_cuvid av1_cuvid; do
  "$DST/bin/ffmpeg.exe" -hide_banner -loglevel error -decoders 2>/dev/null | grep -q " $what " \
    && echo "  decoder $what: present" || echo "  decoder $what: MISSING"
done
"$DST/bin/ffmpeg.exe" -hide_banner -hwaccels 2>/dev/null | grep -q "d3d11va" \
  && echo "  hwaccel d3d11va: present" || echo "  hwaccel d3d11va: MISSING"
ls "$DST/bin"/*.dll | sed 's#.*/#  dll: #'
echo "headers/libs: $(ls "$DST/include" 2>/dev/null | tr '\n' ' ')"
echo "license text: $(head -1 "$DST/LICENSE.txt" 2>/dev/null)"
