"""Which wallpapers are GPU-bound, measured at the panel ceiling.

The slot's own avg_frame_ms is the pacing interval, not the cost, so the only way to price a
wallpaper here is to lift BOTH caps (config max_fps and the package's own "fps") and see where
delivery stops below the panel's 144 Hz. The package fps is edited in the *runtime* copy under
bld/bin/.../Wallpapers only, and restored in the finally block; `--ctl reload` re-scans so no
restart is needed.

usage: gpu-ceiling.py [ids...]
"""
import io, json, os, subprocess, sys, time

BIN = r"H:\wallpaper\bld\bin\RelWithDebInfo"
EXE = os.path.join(BIN, "WallpaperRenderer.exe")
WINDOW_S = 12
orig = {}


def ctl(*a):
    r = subprocess.run([EXE, "--ctl", *a], capture_output=True, timeout=30)
    s = r.stdout.decode("utf-8", "replace")
    i = s.find("{")
    return json.loads(s[i:]) if i >= 0 else None


def snap():
    d = ctl("status")
    m = d["monitors"][0]
    return d, m


def set_pkg_fps(wid, value):
    p = os.path.join(BIN, "Wallpapers", wid, "wallpaper.json")
    d = json.load(io.open(p, encoding="utf-8"))
    old = d.get("fps")
    d["fps"] = value
    io.open(p, "w", encoding="utf-8").write(json.dumps(d, ensure_ascii=False, indent=2))
    return old


def main():
    ids = sys.argv[1:] or ["aurora", "embers", "snowfall", "datarain", "neonfall", "binarydrift"]
    ctl("fps", "240")
    print(f"{'wallpaper':<11} {'pkg fps':>8} {'eff':>4} {'drawn/s':>8} {'of 144':>7} {'late':>5} "
          f"{'worst':>7} {'cpu%':>6}  verdict")
    try:
        for wid in ids:
            old = set_pkg_fps(wid, 240)
            orig[wid] = old
            ctl("reload")
            ctl("apply", wid, "M0")
            time.sleep(6)
            d0, m0 = snap()
            t0 = time.monotonic()
            time.sleep(WINDOW_S)
            d1, m1 = snap()
            el = time.monotonic() - t0
            drawn = (m1["frames"] - m0["frames"]) / el
            eff = m1["effective_fps"]
            late = m1["late_frames"] - m0["late_frames"]
            verdict = "display-bound (free)" if drawn >= 138 else f"GPU-bound above {drawn:.0f} fps"
            print(f"{wid:<11} {str(old):>8} {eff:>4} {drawn:>8.2f} {100*drawn/144:>6.1f}% {late:>5} "
                  f"{m1['worst_frame_ms']:>7} {d1['process']['cpu_percent']:>6.2f}  {verdict}")
    finally:
        for wid, old in orig.items():
            p = os.path.join(BIN, "Wallpapers", wid, "wallpaper.json")
            d = json.load(io.open(p, encoding="utf-8"))
            d["fps"] = old
            io.open(p, "w", encoding="utf-8").write(json.dumps(d, ensure_ascii=False, indent=2))
        ctl("fps", "60")
        ctl("reload")
        ctl("apply", "snowfall", "M0")
        d, m = snap()
        print(f"restored: {m['wallpaper']} eff={m['effective_fps']} max_fps={d['max_fps']} "
              f"pkg fps back to {sorted(orig.items())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
