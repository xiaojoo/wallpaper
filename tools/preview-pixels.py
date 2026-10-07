#!/usr/bin/env python3
"""tools/preview-pixels.py - measure the pixels the renderer actually composited for a video.

Why not a screenshot: the wallpaper surface sits under every app window, so a screenshot taken while
the desktop is covered measures that window, not the wallpaper. The live preview's shared-memory
section (Engine/App/PreviewStream.hpp) is the same shader output on the same device, and its layout
is fixed by that header, so it is a faithful ruler for "what did Video.hlsl put on screen".

Fixture geometry (tools/mkfixture.cpp), in video pixels of a 16:9 clip shown into a 16:9 preview box:
  top third    8 saturated colour bars - the matrix choice moves these by tens
  middle third 11 step studio-swing luma ramp, 16..235, grey, so both matrices agree
  bottom third grey 128 with one 40 px white block that walks left to right

usage: tools/preview-pixels.py <renderer exe> <wallpaper id> [--save out.png]
"""
import ctypes
import json
import struct
import subprocess
import sys
import time
from ctypes import wintypes

MAP = "Local\\SmartWallpaper.Preview"
MAGIC, VERSION = 0x56535057, 2
HDR_BYTES = 64
# PreviewHeader: magic 0, version 4, w 8, h 12, stride 16, seq 20, stampMs 24, frames 32, idHash 40
SEQ_OFF = 20
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (16, 16, 16)]
RAMP = [16 + i * (235 - 16) // 10 for i in range(11)]
# What BT.601 instead of BT.709 does to the saturated bars, measured on this fixture. The checker has
# to be able to tell the two apart, otherwise "the colours look fine" means nothing.
DELTA_601_ON_RED = 54


def open_section():
    k32 = ctypes.windll.kernel32
    k32.OpenFileMappingW.restype = wintypes.HANDLE
    k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    k32.MapViewOfFile.restype = wintypes.LPVOID
    k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.DWORD, ctypes.c_size_t]
    h = k32.OpenFileMappingW(0x0004, False, MAP)
    if not h:
        return None, None, k32.GetLastError()
    v = k32.MapViewOfFile(h, 0x0004, 0, 0, HDR_BYTES + 1280 * 720 * 4)
    if not v:
        return h, None, k32.GetLastError()
    return h, v, 0


def read_frame(view):
    head = ctypes.string_at(view, HDR_BYTES)
    magic, ver, w, h, stride = struct.unpack_from("<IIIII", head, 0)
    if magic != MAGIC or ver != VERSION or not w or not h:
        return None
    seq = struct.unpack_from("<i", head, SEQ_OFF)[0]
    if seq & 1:
        return None
    buf = ctypes.string_at(view + HDR_BYTES, h * stride)
    if struct.unpack_from("<i", ctypes.string_at(view + SEQ_OFF, 4), 0)[0] != seq:
        return None
    return w, h, buf


def sample(w, h, buf, vx, vy, vw, vh):
    x = min(w - 1, max(0, int(vx * w / vw)))
    y = min(h - 1, max(0, int(vy * h / vh)))
    o = y * w * 4 + x * 4
    return buf[o + 2], buf[o + 1], buf[o]


def block_centroid(w, h, buf):
    """The white block lives in the bottom third; the rest of that band is grey 128 or black."""
    tot = n = 0
    for y in range(h * 2 // 3, h, 2):
        for x in range(0, w, 2):
            o = y * w * 4 + x * 4
            if buf[o] > 235 and buf[o + 1] > 235 and buf[o + 2] > 235:
                tot += x
                n += 1
    return (tot / n if n else None), n


def ctl(exe, *args):
    r = subprocess.run([exe, "--ctl", *args], capture_output=True, check=False)
    # The snapshot carries Chinese reason strings as UTF-8; on this machine the locale codec is GBK
    # and text=True would die on them.
    return r.stdout.decode("utf-8", "replace")


def main():
    exe, ident = sys.argv[1], sys.argv[2]
    save = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else None

    out = ctl(exe, "status")
    st = json.loads(out[out.find("{"):])
    mon = next((m for m in st["monitors"] if m.get("wallpaper") == ident), st["monitors"][0])
    vw, vh = (int(v) for v in mon.get("video_size", "1920x1080").split("x"))
    print(f"clip {vw}x{vh} {mon.get('video_fps')} fps, engine paused={st.get('paused')}, "
          f"delivering={mon.get('video_frames_delivered')}")

    subprocess.run([exe, "--ctl", "preview", ident], capture_output=True, check=False)
    hsec, view, gle = open_section()
    if not view:
        print(f"no preview section (gle {gle})")
        return 1

    frames = []
    owner = None
    deadline = time.time() + 2.6  # a pipe-driven preview is closed by its own watchdog after ~3 s
    while time.time() < deadline and len(frames) < 2:
        f = read_frame(view)
        if f and (not frames or f[2] != frames[-1][2]):
            frames.append(f)
            owner = ctl(exe, "status")
        time.sleep(0.04)
    if len(frames) < 2:
        print(f"got {len(frames)} distinct preview frame(s) in 2.6 s - cannot prove motion")
        return 1
    # The settings window re-claims the preview pass for whatever it has selected, so the frame in the
    # section is only ours while status says so. Reading it without this check measures someone else's
    # wallpaper and reports it as a colour defect.
    if owner:
        o = json.loads(owner[owner.find("{"):])
        if not o["preview"].get("on") or o["preview"].get("id") != ident:
            print(f"ABORT: the preview pass belongs to {o['preview'].get('id') if o['preview'].get('on') else 'nobody'}, "
                  f"not {ident} - these pixels are not the clip under test")
            return 2

    w, h, buf = frames[0]
    print(f"preview surface {w}x{h}, 2 distinct frames collected")

    worst = 0
    for i, exp in enumerate(BARS):
        got = sample(w, h, buf, (i + 0.5) * vw / 8, vh * 0.20, vw, vh)
        d = max(abs(a - b) for a, b in zip(got, exp))
        worst = max(worst, d)
        print(f"  bar {i}: expected {exp} got {got} max|delta| {d}")
    print(f"  worst bar delta {worst}; using the wrong (601) matrix would put the red bar about "
          f"{DELTA_601_ON_RED} off")

    errs, notgrey = [], []
    for j, exp in enumerate(RAMP):
        got = sample(w, h, buf, (j + 0.5) * vw / 11, vh * 0.50, vw, vh)
        errs.append(got[0] - exp)
        # The clip is H.264 4:2:0, so a neutral grey can come back with R and B a couple of codes apart
        # from G. Only a channel gap over 2 is a plane-order or chroma-geometry fault.
        if max(abs(got[0] - got[1]), abs(got[0] - got[2]), abs(got[1] - got[2])) > 2:
            notgrey.append((j, exp, got))
    print(f"  luma ramp grey error min {min(errs)} max {max(errs)}, cells with channel gap > 2: "
          f"{len(notgrey)}")
    for row in notgrey[:4]:
        print(f"    cell {row}")

    c0, n0 = block_centroid(w, h, frames[0][2])
    c1, n1 = block_centroid(w, h, frames[1][2])
    shift = None if c0 is None or c1 is None else round(c1 - c0, 1)
    print(f"  white block: frame1 x={c0} over {n0} px, frame2 x={c1} over {n1} px, shift {shift} px")

    if save:
        from PIL import Image
        rgba = bytearray(len(buf))
        rgba[0::4] = buf[2::4]
        rgba[1::4] = buf[1::4]
        rgba[2::4] = buf[0::4]
        rgba[3::4] = b"\xff" * (len(buf) // 4)
        Image.frombytes("RGBA", (w, h), bytes(rgba)).save(save)
        print("saved", save)
    return 0


if __name__ == "__main__":
    sys.exit(main())
