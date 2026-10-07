#!/usr/bin/env python3
"""tools/find-stalls.py - find the seconds where the frame loop delivered less than it should have.

Only input is the renderer's own log: the once-a-second `loop:` line carries the cumulative frame
counter (`drew=`), so the per-second delta *is* the delivered frame rate. Comparing each second with
the median of the run makes a stall show up as a ratio (0.4x = 60% of that second was spent not
drawing) without needing to know which power cap was in force at the time.

Why not poll `--ctl status` faster: the snapshot is rebuilt once a second (lastSnapshotMs_ > 1000),
so a 200 ms sampler reads the same `frames` value over and over and reports a resolution it does not
have. Sub-second stalls have to come from the log's own per-second counter.

usage: find-stalls.py [log-path]   (default: the newest renderer-*.log)
"""
import glob
import os
import re
import statistics
import sys

LOOP = re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\.\d+ \w+\s+\[app\] loop: (\d+) iters/s drew=(\d+)')
SLOT = re.compile(r'^\S+ \S+ \w+\s+\[app\]\s+(\S+) (\S+) (\w+) target=(\d+) eff=([\d.]+)')
PREV = re.compile(r'^\S+ (\d\d:\d\d:\d\d)\.\d+ \w+\s+\[app\] command previewbeat')


def newest_log() -> str:
    return max(glob.glob(r'H:\wallpaper\bld\bin\RelWithDebInfo\logs\renderer-*.log'), key=os.path.getmtime)


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else newest_log()
    secs, caps, prev_secs = [], [], set()
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LOOP.match(line)
        if m:
            d, t, iters, drew = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
            secs.append((t, iters, drew))
            continue
        s = SLOT.match(line)
        if s:
            caps.append((s.group(2)[0:8], int(s.group(4))))
        p = PREV.match(line)
        if p:
            prev_secs.add(p.group(1))

    if len(secs) < 20:
        print(f'{os.path.basename(path)}: only {len(secs)} loop lines, not enough to judge')
        return 1

    deltas = [secs[i][2] - secs[i - 1][2] for i in range(1, len(secs)) if secs[i][2] >= secs[i - 1][2]]
    med = statistics.median(deltas)
    if med <= 0:
        print('median delivered frames/s is 0 - nothing was drawing in this window')
        return 1
    print(f'{os.path.basename(path)}  {len(secs)} 个整秒  中位交付 {med:.0f} 帧/秒  '
          f'区间 {min(deltas)}~{max(deltas)}')

    stalls = []
    for i in range(1, len(secs)):
        d = secs[i][2] - secs[i - 1][2]
        if 0 <= d < med * 0.8:
            stalls.append((secs[i][0], d, d / med, secs[i][1], secs[i][0] in prev_secs))
    print(f'掉速秒 {len(stalls)} / {len(deltas)} ({100.0 * len(stalls) / len(deltas):.1f}%)'
          '   (阈值: 低于中位数 80%)')
    print('   时刻        交付  相对   循环it/s  这一秒预览在跳心跳?')
    for t, d, ratio, iters, pv in stalls[:25]:
        print(f'   {t}  {d:4}  {ratio:5.2f}x   {iters:5}     {"是" if pv else "否"}')
    if len(stalls) > 25:
        print(f'   ... 另有 {len(stalls) - 25} 秒')
    if len(stalls) > 1:
        gaps = []
        last = None
        for t, *_ in stalls:
            h, mnt, sc = (int(x) for x in t.split(':'))
            s = h * 3600 + mnt * 60 + sc
            if last is not None:
                gaps.append(s - last)
            last = s
        print(f'掉速秒之间的间隔: {sorted(gaps)[:12]} 秒 (若集中在某个数, 就是周期性事件)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
