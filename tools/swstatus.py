import json
import subprocess
import sys

CTL = r"H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe"

d = json.loads(subprocess.run([CTL, "--ctl", "status"], capture_output=True, text=True,
                              encoding="utf-8", errors="replace").stdout)
m = d["monitors"][0]
p = d.get("preview") or {}
print("showing=%s preview_on=%s preview_id=%s preview_drew=%s state=%s measured_fps=%s"
      % (m["wallpaper"], p.get("on"), p.get("id"), p.get("drew"), m["state"], m["measured_fps"]))
