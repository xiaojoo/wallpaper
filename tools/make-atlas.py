"""Build the glyph atlas the HUD draws its text with.

The wallpaper used to write text from a 5x6 bitmap baked into the shader, which reads as dot matrix.
This renders the printable ASCII range from a real face and packs it into a grid the shader can index.

The cell is 32x48, not the 40x48 it was, because the card the text has to fit is: the reference draws
"SYSTEM STATUS" across 60% of a card 0.95 as wide as it is tall, in a condensed face whose advance is
0.74 of its cap height. Consolas at 40x48 measures 1.18, so fitting the same thirteen characters into
the same card cost a third of the cap height. Arial Narrow Bold measures 0.74 and keeps the letters
at the size the reference uses.

The atlas is saved as an 8-bit **grayscale** PNG on purpose. The engine's texture loader converts
everything to DXGI_FORMAT_B8G8R8A8, so in the shader .r is the blue channel - a colour atlas would
work but invites the same mistake that made the figure's line art vanish. With a grayscale image all
three channels carry the coverage and any of them reads correctly.
"""
import os
from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 32, 48
COLS = 16
FIRST, LAST = 32, 126
OUT = r'Shaders\glyphs.png'

codes = list(range(FIRST, LAST + 1))
rows = (len(codes) + COLS - 1) // COLS
img = Image.new('L', (COLS * CELL_W, rows * CELL_H), 0)
d = ImageDraw.Draw(img)

CANDIDATES = [
    r'C:\Windows\Fonts\arialnb.ttf',      # condensed bold sans: the reference's own proportions
    r'C:\Windows\Fonts\ARIALNB.TTF',
    r'C:\Windows\Fonts\consola.ttf',
]
path = next((p for p in CANDIDATES if os.path.exists(p)), None)
if not path:
    raise SystemExit('no font found among %s' % CANDIDATES)

# Fit by cap height, not by advance: the shader's layout is driven by how tall the letters are, and
# the cell only has to be wide enough that no glyph's ink is clipped. '@' and 'W' are the widest ink
# in the face (0.80 em at em=100), so those two decide the ceiling.
TARGET_CAP = CELL_H * 0.562
size = 8
for cand in range(8, 200):
    f = ImageFont.truetype(path, cand)
    cap = f.getbbox('H')[3] - f.getbbox('H')[1]
    widest = max(f.getbbox(g)[2] - f.getbbox(g)[0] for g in 'W@M8%')
    if cap > TARGET_CAP or widest > CELL_W * 0.94:
        break
    size = cand

font = ImageFont.truetype(path, size)
asc, desc = font.getmetrics()
cap = font.getbbox('H')[3] - font.getbbox('H')[1]
print('font %s size %d  cap %d/%d = %.3f of cell  widest ink %d of %d  cell %dx%d  atlas %dx%d  glyphs %d'
      % (os.path.basename(path), size, cap, CELL_H, cap / CELL_H,
         max(font.getbbox(g)[2] - font.getbbox(g)[0] for g in 'W@M8%'), CELL_W,
         CELL_W, CELL_H, img.width, img.height, len(codes)))

for n, code in enumerate(codes):
    ch = chr(code)
    cx = (n % COLS) * CELL_W + CELL_W // 2
    cy = (n // COLS) * CELL_H + (CELL_H - (asc + desc)) // 2 + asc
    d.text((cx, cy), ch, fill=255, font=font, anchor='ms')

img.save(OUT)
print('wrote %s' % OUT)
# Report coverage so an empty cell (a glyph the face has no room for) is visible here rather than
# showing up as a blank on the desktop.
import numpy as np
a = np.asarray(img)
weak = [(chr(codes[i]), int(a[i // COLS * CELL_H:(i // COLS + 1) * CELL_H,
                          (i % COLS) * CELL_W:(i % COLS + 1) * CELL_W].max()))
        for i in range(len(codes))]
bad = [w for w in weak if w[1] < 200]
print('cells whose peak coverage is under 200/255:', bad or 'none')
print('grid: index = (code - 32); col = index %% %d, row = index // %d; uv = ((col+qx)/%d, (row+qy)/%d)'
      % (COLS, COLS, COLS, rows))
