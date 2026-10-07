#!/usr/bin/env python3
"""tools/watch-pace.py - sample the renderer over minutes and print the pace trend.

Why this exists instead of reading the slot's own numbers: a wallpaper's motion is stepped by the
`delta` the engine hands its shader, so the question "is it getting faster as time goes on" is a
question about that step and the real draw rate, measured from outside over a long window. The slot
reports `measured_fps` from its own rolling window, which keeps its last value while the cap is 0,
so it can read 9.94 in a state that is drawing nothing - the drawn/s column here is counted from the
`frames` counter instead.

What this instrument cannot see: the package's own HLSL may clamp the step after the engine hands it
out (snowfall used to do exactly that). The step and the factor below are the engine's view.

usage: watch-pace.py [buckets] [bucket_seconds]
"""
import json
import subprocess
import sys
import time

EXE = r"H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe"


def snap():
    # Decode as UTF-8 explicitly: the snapshot carries Chinese category names, and Python's default
    # here is the ANSI codepage (GBK), which dies on those bytes and hands back a partial read.
    r = subprocess.run([EXE, "--ctl", "status"], capture_output=True, timeout=20)
    out = r.stdout.decode("utf-8", "replace")
    i = out.find("{")
    if i < 0:
        raise RuntimeError("no json from the pipe: " + out[:120])
    return json.loads(out[i:])


def main() -> int:
    buckets = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    span = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    print(f"{'t':>5} {'wall':>9} {'state':>10} {'drawn/s':>8} {'meas':>7} {'eff':>5} {'cap':>4} "
          f"{'delta_s':>8} {'d x rate':>8} {'maxdt':>8} {'ch/s':>5}  reason")
    prev = prev_t = prev_ch = None
    t0 = time.monotonic()
    worst = {}
    for _ in range(buckets):
        time.sleep(span)
        s = snap()
        for m in s.get("monitors", []):
            f = m.get("frames", 0)
            ch = m.get("corner_samples_changed", 0)
            t = time.monotonic()
            rate = 0.0 if prev is None else (f - prev) / max(t - prev_t, 1e-6)
            chrs = 0.0 if prev_ch is None else (ch - prev_ch) / max(t - prev_t, 1e-6)
            prev, prev_t, prev_ch = f, t, ch
            dt = float(m.get("sim_delta_s", 0.0))
            tag = m.get("wallpaper", "")
            worst[tag] = max(worst.get(tag, 0.0), dt)
            print(f"{t - t0:5.0f} {tag:>9} {m.get('state',''):>10} {rate:8.2f} "
                  f"{float(m.get('measured_fps', 0)):7.2f} {m.get('effective_fps', 0):5.1f} "
                  f"{m.get('cap_fps', 0):4} {dt:8.5f} {dt * rate:8.3f} {worst[tag]:8.5f} "
                  f"{chrs:5.2f}  {m.get('state_reason','')}"[:178])
    return 0


if __name__ == "__main__":
    sys.exit(main())
