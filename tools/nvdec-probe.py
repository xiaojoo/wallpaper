#!/usr/bin/env python3
"""tools/nvdec-probe.py - what does decoding this clip actually cost, per decoder?

Runs ffmpeg over a clip for a fixed number of loops while sampling the decoder process (RSS, private
bytes, CPU) and the GPU's decode engine (`nvidia-smi dmon -s u`, the `dec` column), for two configs:
NVDEC (`-hwaccel cuda -c:v h264_cuvid`) and plain software decode. The point is attribution: our
Media Foundation path measured +929 MB of committed memory per 4K player, of which only ~47 MB was
ours, so the question is how much of that is "CPU-decoding 4K" and how much is "this particular
decoder".

usage: tools/nvdec-probe.py <ffmpeg.exe> <clip> [loops]
"""
import ctypes
import re
import subprocess
import sys
import threading
import time
from ctypes import wintypes

k32 = ctypes.windll.kernel32


class PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PagepeakUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t),
                ("SharedWriteWatchedUsage", ctypes.c_size_t)]


def mem_mb(pid):
    """(working set, private) in MB for a process id, or (0, 0) if it is gone."""
    h = k32.OpenProcess(0x1010, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION | VM_READ
    if not h:
        return 0.0, 0.0
    try:
        p = PMC()
        p.cb = ctypes.sizeof(PMC)
        if not k32.K32GetProcessMemoryInfo(h, ctypes.byref(p), p.cb):
            return 0.0, 0.0
        return p.WorkingSetSize / 1048576.0, p.PrivateUsage / 1048576.0
    finally:
        k32.CloseHandle(h)


def cpu_percent(pid, stop, out):
    """Total CPU seconds consumed, sampled twice a second; the deltas are the rate."""
    prev = None
    samples = []
    while not stop.is_set():
        h = k32.OpenProcess(0x1000 | 0x0400, False, pid)   # QUERY_INFORMATION | QUERY_LIMITED
        if not h:
            break
        try:
            ct = ctypes.wintypes.FILETIME()
            et = ctypes.wintypes.FILETIME()
            kt = ctypes.wintypes.FILETIME()
            ut = ctypes.wintypes.FILETIME()
            if not k32.GetProcessTimes(h, ctypes.byref(ct), ctypes.byref(et), ctypes.byref(kt),
                                       ctypes.byref(ut)):
                break
            now = ((kt.dwHighDateTime << 32 | kt.dwLowDateTime) +
                   (ut.dwHighDateTime << 32 | ut.dwLowDateTime)) / 1e7
        finally:
            k32.CloseHandle(h)
        if prev is not None:
            samples.append((now - prev[0]) / 0.5 * 100.0)   # 100 % = one core
        prev = (now, None)
        stop.wait(0.5)
    out.extend(samples)


def dmon(path, seconds):
    with open(path, "w", encoding="utf-8", errors="replace") as f:
        subprocess.run(["nvidia-smi", "dmon", "-s", "u", "-d", "1", "-c", str(seconds)],
                       stdout=f, stderr=subprocess.STDOUT, check=False)


def run(ffmpeg, clip, loops, label, extra):
    args = [ffmpeg, "-hide_banner", "-loglevel", "info", "-stream_loop", str(loops)]
    args += extra + ["-i", clip, "-map", "0:v:0", "-f", "null", "-"]
    t0 = time.time()
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                         errors="replace")
    stop, cpus = threading.Event(), []
    th = threading.Thread(target=cpu_percent, args=(p.pid, stop, cpus))
    th.start()
    ws, priv = [], []
    while p.poll() is None:
        w, v = mem_mb(p.pid)
        if w:
            ws.append(w)
            priv.append(v)
        time.sleep(0.25)
    err = p.stderr.read()
    stop.set()
    th.join()
    secs = time.time() - t0
    # The progress lines are the only frame count available; take the last one. ffmpeg's own "fps="
    # field is parsed from a *different* line than the one that carries `frame=`, so trusting the first
    # "fps" match in the whole log gave two runs the same number and looked like a hardware failure
    # when nothing had failed.
    frames = [int(x) for x in re.findall(r"frame=\s*(\d+)", err)]
    fps = frames[-1] / secs if frames and secs > 0 else 0.0
    hw = "cuda" in err or "cuvid" in " ".join(extra)
    print(f"[{label}] {secs:.1f} s wall, {frames[-1] if frames else 0} frames decoded = {fps:.0f} fps")
    if ws:
        print(f"          working set peak {max(ws):.0f} MB (mean {sum(ws)/len(ws):.0f}), "
              f"private peak {max(priv):.0f} MB (mean {sum(priv)/len(priv):.0f})")
    if cpus:
        print(f"          process CPU mean {sum(cpus)/len(cpus):.0f} % of one core, max {max(cpus):.0f} %")
    if fps:
        print(f"          normalised to 60 fps: {sum(cpus)/len(cpus) * 60.0 / fps:.0f} % of one core")
    return fps, (max(ws) if ws else 0), (max(priv) if priv else 0), (sum(cpus) / len(cpus) if cpus else 0)


def main():
    ffmpeg, clip = sys.argv[1], sys.argv[2]
    loops = sys.argv[3] if len(sys.argv) > 3 else "6"
    dm = threading.Thread(target=dmon, args=("build/dmon-probe.txt", 60))
    dm.start()
    time.sleep(1)
    sw = run(ffmpeg, clip, loops, "software h264 (libavcodec)", ["-c:v", "h264"])
    time.sleep(1)
    rb = run(ffmpeg, clip, loops, "NVDEC + copy back to RAM", ["-hwaccel", "cuda", "-c:v", "h264_cuvid"])
    time.sleep(1)
    hw = run(ffmpeg, clip, loops, "NVDEC, frames kept on the GPU",
             ["-hwaccel", "cuda", "-hwaccel_output_format", "cuda", "-c:v", "h264_cuvid"])
    dm.join()
    print("\nGPU decode-engine utilisation (dec column, % of the NVDEC block):")
    try:
        rows = []
        for line in open("build/dmon-probe.txt", encoding="utf-8", errors="replace"):
            c = line.split()
            if len(c) == 7 and c[0] == "0":
                try:
                    rows.append(int(c[5]))
                except ValueError:
                    pass
        if rows:
            print("   samples:", rows)
            print(f"   mean {sum(rows)/len(rows):.1f} %  max {max(rows)} %")
    except OSError:
        print("   (no dmon output)")
    print(f"\nsummary (this clip at 60 fps would cost, normalised): software {sw[3]*60/sw[0]:.0f} % cpu / "
          f"{sw[1]:.0f} MB ws   NVDEC+readback {rb[3]*60/rb[0]:.0f} % / {rb[1]:.0f} MB   "
          f"NVDEC on-GPU {hw[3]*60/hw[0]:.0f} % / {hw[1]:.0f} MB")


if __name__ == "__main__":
    main()
