"""Per-wallpaper pacing sweep: is the stutter specific to the hand-written shader wallpapers?

For each package: apply it, let the switch settle, then measure one fixed window and report the
delivered rate counted from the slot's own frame counter (its measured_fps is a rolling self-report),
the late-frame count accumulated *inside that window* (late_frames resets when the budget moves, so
the delta is the window's), the worst interval, and the process CPU. The wallpaper id is read at both
ends of every window - a concurrent session applying something else mid-window shows up as a
mismatch and the row is marked instead of believed.

usage: per-wallpaper-pace.py [seconds] [ids...]
"""
import json, subprocess, sys, time

EXE = r"H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe"
SETTLE_S = 7


def ctl(*a):
    r = subprocess.run([EXE, "--ctl", *a], capture_output=True, timeout=25)
    s = r.stdout.decode("utf-8", "replace")
    i = s.find("{")
    return json.loads(s[i:]) if i >= 0 else None


def snap():
    d = ctl("status")
    if not d:
        return None
    m = d["monitors"][0]
    return dict(pid=d["pid"], wall=m["wallpaper"], state=m["state"], target=m["target_fps"],
                eff=m["effective_fps"], late=m["late_frames"], frames=m["frames"],
                worst=float(m["worst_frame_ms"]), avg=float(m["avg_frame_ms"]),
                cpu=round(d["process"]["cpu_percent"], 2), ws=round(d["process"]["working_set_mb"], 1))


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 25
    ids = sys.argv[2:] or ["aurora", "embers", "snowfall", "datarain", "neonfall", "binarydrift", "local_01"]
    print(f"{'wallpaper':<11} {'kind':<7} {'state':>10} {'tgt':>4} {'eff':>4} {'ideal':>6} "
          f"{'drawn/s':>8} {'late':>5} {'worst':>7} {'avg':>7} {'cpu%':>6}")
    for wid in ids:
        ctl("apply", wid, "M0")
        time.sleep(SETTLE_S)
        a = snap()
        if not a:
            print(f"{wid:<11} no reply from the pipe"); continue
        time.sleep(secs)
        b = snap()
        if not b or b["pid"] != a["pid"] or b["wall"] != a["wall"]:
            print(f"{wid:<11} INVALID: the slot moved under the window "
                  f"({a['wall']}/{a['pid']} -> {b['wall'] if b else '-'}/{b['pid'] if b else '-'})")
            continue
        el = b["frames"] - a["frames"]
        kind = "image" if wid.startswith("local") else "shader"
        ideal = 1000.0 / b["eff"] if b["eff"] else 0
        print(f"{wid:<11} {kind:<7} {b['state']:>10} {b['target']:>4} {b['eff']:>4} {ideal:>6.2f} "
              f"{el/secs:>8.2f} {b['late']-a['late']:>5} {b['worst']:>7.2f} {b['avg']:>7.2f} {b['cpu']:>6.2f}"
              + ("   <- missed the budget" if b["eff"] and el/secs < b["eff"]*0.95 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
