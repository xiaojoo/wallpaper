#!/usr/bin/env bash
# tools/check-shader.sh - compile a wallpaper package's HLSL before it ever reaches the desktop.
#
# Why this exists: on 2026-10-09 a wallpaper whose main.hlsl failed to compile took the whole desktop
# slot down with it - `slots=0`, the composited surface frozen on its last frame - and the only thing
# that said so was a renderer log line. A screenshot taken after that measures a still image and looks
# like a result. fxc answers the same question in about a second without touching his screen.
#
# usage: bash tools/check-shader.sh [Wallpapers/<pkg>] [...]     (default: every package)
set -u
cd "$(dirname "$0")/.." || exit 1
FXC="/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/fxc.exe"
[ -x "$FXC" ] || { echo "fxc not found at $FXC"; exit 2; }

pkgs=("$@")
[ ${#pkgs[@]} -eq 0 ] && pkgs=($(ls -d Wallpapers/*/ | sed 's:/$::'))
fail=0
for p in "${pkgs[@]}"; do
  src="$p/main.hlsl"
  [ -f "$src" ] || { printf '%-34s no main.hlsl, skipped\n' "$p"; continue; }
  out=""
  # PSMain is the contract every package satisfies; snowfall carries CSMain/ParticleVS/PSBackground
  # too, and those are its own business - this gate is for the pass the desktop draws with.
  # Dash-prefixed switches, deliberately: MSYS rewrites a bare "/T ps_5_0" into the path "T:/",
  # which fxc rejects before it reads the file, and the gate then reports every package as failing
  # with an empty reason. That is how this script's first version behaved.
  for entry in PSMain VSMain; do
    prof=$([ "$entry" = PSMain ] && echo ps_5_0 || echo vs_5_0)
    m="$("$FXC" -nologo -T "$prof" -E "$entry" -I Shaders -I "$p" "$src" -Fo NUL 2>&1)" && rc=0 || rc=$?
    [ $rc -ne 0 ] && out="$out $entry: $(echo "$m" | tr -d '\r' | grep -oE 'error X[0-9]+: .*' | head -3 | tr '\n' ';')"
  done
  if [ -n "$out" ]; then printf '%-34s FAIL%s\n' "$p" "$out"; fail=1
  else printf '%-34s ok\n' "$p"; fi
done
exit $fail
