"""The two things the geometry probe could not say: the per-ring clamp, and surviving a restart.

The clamp matters because the slider's ceiling is 64/rings - push 环上数量 past it with 环数=4 and the
ring must draw what the slider now reads, not what was asked for. The restart matters because 环数 and
形状 are the only two of the ring's knobs that are saved, so "it went back to one ring when I rebooted"
is a real question about them.
"""
import json
import subprocess
import time

CTL = r"H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe"
CFG = r"H:\wallpaper\bld\bin\RelWithDebInfo\config.json"


def status():
    r = subprocess.run([CTL, "--ctl", "status"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return json.loads(r.stdout)


def ctl(*args):
    subprocess.run([CTL, "--ctl", *args], capture_output=True, text=True,
                   encoding="utf-8", errors="replace")


def wait(**want):
    for _ in range(40):
        s = status()
        if all(s.get(k) == v for k, v in want.items()):
            return s
        time.sleep(0.3)
    return status()


def cfg():
    with open(CFG, encoding="utf-8") as f:
        return json.load(f)


print("pid before:", subprocess.run(["powershell", "-NoProfile", "-Command",
                                     "(Get-Process WallpaperRenderer).Id"],
                                    capture_output=True, text=True).stdout.strip())

s = wait(**{"orbit": True})
print("start: rings=%s per_ring=%s shape=%s shown=%s pickable=%s"
      % (s["orbit_rings"], s["orbit_per_ring"], s["orbit_shape"], s["orbit_shown"], s["orbit_pickable"]))

ctl("orbitrings", "4")
time.sleep(1.0)
ctl("orbitcount", "30")
s = wait(orbit_rings=4)
print("\nclamp: asked 30 per ring with 4 rings -> per_ring=%s shown=%s (ceiling is 64/4=16, "
      "and 43 shortcuts exist, so shown should be 43)" % (s["orbit_per_ring"], s["orbit_shown"]))

ctl("orbitrings", "3")
ctl("orbitshape", "star")
s = wait(orbit_rings=3, orbit_shape="star")
c = cfg()
print("\nsaved: status rings=%s shape=%s | config.json orbit_rings=%s orbit_shape=%s"
      % (s["orbit_rings"], s["orbit_shape"], c.get("orbit_rings"), c.get("orbit_shape")))

subprocess.run(["powershell", "-NoProfile", "-Command",
                "Stop-Process -Name WallpaperRenderer -Force"], capture_output=True)
print("\nkilled the renderer; waiting for the settings window's watchdog to bring it back")
pid = ""
for _ in range(60):
    time.sleep(1.0)
    pid = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Id"],
                         capture_output=True, text=True).stdout.strip()
    if pid:
        break
print("pid after:", pid or "none - the watchdog did not relaunch it")
time.sleep(3.0)
s = None
for _ in range(20):                       # the new instance opens the pipe a second or two after it exists
    try:
        s = status()
        break
    except Exception:                     # noqa: BLE001
        time.sleep(1.0)
if s is None:
    print("the restarted renderer never answered on the pipe")
    raise SystemExit(1)
print("after restart: rings=%s shape=%s per_ring=%s shown=%s icons=%s ready=%s"
      % (s["orbit_rings"], s["orbit_shape"], s["orbit_per_ring"], s["orbit_shown"],
         s["orbit_icons"], s["orbit_ready"]))

ctl("orbitshape", "ellipse")
ctl("orbitrings", "1")
ctl("orbitcount", "12")
s = wait(orbit_rings=1, orbit_shape="ellipse", orbit_per_ring=12)
c = cfg()
print("\nrestored: status rings=%s shape=%s per_ring=%s shown=%s | config orbit_rings=%s orbit_shape=%s"
      % (s["orbit_rings"], s["orbit_shape"], s["orbit_per_ring"], s["orbit_shown"],
         c.get("orbit_rings"), c.get("orbit_shape")))
