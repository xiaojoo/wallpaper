"""Build a before|after montage of the 1280x720 previews, and measure the 480x270 cards too.

The card is what the browse grid shows, so a fix that only reads at hero size is not a fix.
"""
import subprocess
import sys
from PIL import Image, ImageDraw

NAMES = ("datarain", "binarydrift", "neonfall", "embers")
BEFORE = "build/shots/density_before"
AFTER = "bld/bin/RelWithDebInfo/cache/thumbs"
LABEL = 26


def pair(name):
    a = Image.open(f"{BEFORE}/{name}_large.png").convert("RGB")
    b = Image.open(f"{AFTER}/{name}_large.png").convert("RGB")
    w, h = a.size
    out = Image.new("RGB", (w * 2 + 6, h + LABEL), (12, 12, 16))
    d = ImageDraw.Draw(out)
    out.paste(a, (0, LABEL))
    out.paste(b, (w + 6, LABEL))
    d.text((6, 6), f"{name}  BEFORE (left)  /  AFTER (right)", fill=(220, 220, 230))
    return out


rows = [pair(n) for n in NAMES]
w = max(r.width for r in rows)
h = sum(r.height for r in rows) + 6 * len(rows)
sheet = Image.new("RGB", (w, h), (8, 8, 12))
y = 0
for r in rows:
    sheet.paste(r, (0, y))
    y += r.height + 6
sheet.save("build/shots/density_before_after.png")
print(f"wrote build/shots/density_before_after.png {sheet.size[0]}x{sheet.size[1]}")

print("\ncards (480x270), after:")
print(subprocess.run([sys.executable, "tools/measure-mark-density.py"] +
                     [f"{AFTER}/{n}_still.png" for n in NAMES],
                     capture_output=True, text=True).stdout[:1200])
