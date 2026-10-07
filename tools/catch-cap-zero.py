#!/usr/bin/env python3
"""tools/catch-cap-zero.py - passively wait for the power classifier to zero the frame budget.

The freeze we are chasing lasts a couple of seconds and only shows up in ~1% of them, so reading the
log afterwards is not enough: the log's per-second `loop:` line carries the budget but not the
*reason*, and the reason only lives in the status snapshot of that instant. This polls the pipe and
prints every sample whose cap differs from the last one, together with state_reason and the
foreground window - which is the field that explains "why did it decide to stop drawing".

It does not restart, reconfigure or touch the renderer: it is a reader.

usage: catch-cap-zero.py [seconds]
"""
import json
import subprocess
import sys
import time

EXE = r'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe'


def snap():
    r = subprocess.run([EXE, '--ctl', 'status'], capture_output=True, timeout=20)
    out = r.stdout.decode('utf-8', 'replace')
    i = out.find('{')
    return json.loads(out[i:]) if i >= 0 else None


def main() -> int:
    limit = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    end = time.time() + limit
    last = None
    zero = 0
    suppressed = 0
    trans = 0
    print(f'轮询 {limit:.0f} 秒（每次约 0.7 秒，快照本身每秒才刷新一次）')
    while time.time() < end:
        t0 = time.time()
        try:
            s = snap()
        except Exception as exc:  # noqa: BLE001 - a lost pipe is itself a signal (renderer restarting)
            print(f'{time.strftime("%H:%M:%S")} 读不到管道: {exc}')
            time.sleep(1)
            continue
        if not s:
            print(f'{time.strftime("%H:%M:%S")} 快照为空')
            time.sleep(1)
            continue
        m = s['monitors'][0] if s.get('monitors') else {}
        key = (s.get('pid'), m.get('state'), m.get('cap_fps'), m.get('wallpaper'), s.get('paused'))
        if key != last:
            print(f'{time.strftime("%H:%M:%S")} pid={s.get("pid")} state={m.get("state")} cap={m.get("cap_fps")} '
                  f'eff={m.get("effective_fps")} 交付={m.get("measured_fps")} paused={s.get("paused")} '
                  f'| 理由: {m.get("state_reason")} | power={s.get("power", "")[:70]}')
            if m.get('cap_fps') == 0:
                zero += 1
            trans += 1
            last = key
        r = (m.get('state_reason') or '')
        if '不改预算' in r:
            suppressed += 1
        time.sleep(max(0.0, 0.7 - (time.time() - t0)))
    print(f'结束：状态翻转(真正改预算) {trans} 次 | 被迟滞压住的原始翻转样本 {suppressed} 个 | cap=0 样本 {zero} 个')
    return 0


if __name__ == '__main__':
    sys.exit(main())
