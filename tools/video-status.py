#!/usr/bin/env python3
"""tools/video-status.py - read `WallpaperRenderer.exe --ctl status` output and print one line.

The snapshot is pretty-printed JSON, so a shell pipeline over it is fragile; the fields worth
watching during a video test are named here instead. Reads the JSON on stdin, writes key=value.
"""
import json
import sys


def main() -> int:
    text = sys.stdin.read()
    start = text.find("{")
    if start < 0:
        print("error=no json from the pipe; first 120 chars: " + text[:120].replace("\n", " "))
        return 1
    try:
        s = json.loads(text[start:])
    except Exception as exc:  # noqa: BLE001 - a broken read has to be reported, not raised
        print(f"error=cannot parse status json: {exc}")
        return 1
    print(f"pid={s.get('pid')} paused={s.get('paused')} fit={s.get('image_fit')} "
          f"cpu={s.get('process', {}).get('cpu_percent')} "
          f"ws_mb={s.get('process', {}).get('working_set_mb')}")
    for c in s.get("catalog", []):
        if c.get("type") == "video":
            print(f"catalog id={c.get('id')} name={c.get('name')} res={c.get('resolution')} "
                  f"dur={c.get('duration_s')} fps={c.get('fps')} thumb={bool(c.get('thumb'))}")
    for m in s.get("monitors", []):
        line = (f"mon {m.get('tag')} wallpaper={m.get('wallpaper')} measured={m.get('measured_fps')} "
                f"avg={m.get('avg_frame_ms')} changed={m.get('corner_samples_changed')} "
                f"static={m.get('corner_samples_static')} err={m.get('error')}")
        if "video_pos" in m:
            line += (f" | video pos={m.get('video_pos'):.2f}/{m.get('video_duration'):.2f} "
                     f"src_fps={m.get('video_fps')} size={m.get('video_size')} "
                     f"paused={m.get('video_paused')} ended={m.get('video_ended')} "
                     f"delivered={m.get('video_frames_delivered')} output={m.get('video_output')}")
        print(line)
    p = s.get("preview", {})
    if p.get("on"):
        print(f"preview id={p.get('id')} fps={p.get('fps'):.2f} drew={p.get('drew')} dropped={p.get('dropped')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
