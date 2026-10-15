#pragma once
// Engine/Audio/IAudioPlugin.h - the only contract between the engine and an audio backend.
//
// The engine never links an audio library. It loads one DLL at run time (same shape as
// Engine/App/GpuMonitor.cpp, which does `LoadLibraryExA("nvml.dll")` and treats a missing library as
// "no GPU metrics" rather than as a failure): if the file is absent, unreadable, built against another
// ABI, or the machine has no output device, the wallpaper runs exactly as it does today and simply
// makes no sound. That is what makes this pluggable rather than merely separate - a backend is a file
// you can take away.
//
// Sample format across the seam: interleaved 32-bit float at the rate and channel count `open`
// reports. The engine resamples to that pair with libswresample (it is already in the tree), so the
// backend only ever has to convert float to whatever the device actually wants, if anything.
//
// Keep this header free of C++ and of any include beyond the essentials: a third-party backend is
// supposed to be writable against this file alone.

#include <stdint.h>

#define SWAUDIO_ABI 3u

// The exported entry point. A DLL that does not export exactly this name is refused.
#if defined(_WIN32)
  #define SWAUDIO_EXPORT extern "C" __declspec(dllexport)
#else
  #define SWAUDIO_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct SwAudioPlugin {
    // SWAUDIO_ABI the backend was built against. The engine refuses a mismatch rather than
    // guessing at a struct layout.
    unsigned abi;
    // A short name for the log ("wasapi-shared"), so a report can say which backend produced sound.
    const char* name;

    // Open the default render endpoint. On success fills the format the engine must feed and
    // returns 0; any other value means "no audio", and `error` receives a sentence for the log.
    // Must be safe to call once per process; the engine does not retry behind a failure.
    int (*open)(unsigned* sampleRate, unsigned* channels, char* error, unsigned errorBytes);

    // Feed interleaved float32. Returns the number of frames actually taken - fewer than asked
    // means the buffer is full, and the caller must keep the rest (the decoder thread paces itself
    // on this, which is what keeps memory bounded when the device stalls).
    int (*write)(const float* interleaved, unsigned frames);

    // Linear 0..1 applied to everything after the ring, so a volume change cannot click the ring.
    void (*setVolume)(float linear01);
    // 1 = hold: the device stream stays open and is fed silence, so resuming has no first-frame gap.
    void (*setPaused)(int paused);

    // Frames the ring was short by since open, i.e. how often the backend had to fill silence.
    // The one number that says whether this backend is being starved.
    unsigned (*underruns)(void);
    // Milliseconds of buffering the device asked for; reported so the log can say what latency the
    // engine agreed to rather than what it hoped for.
    unsigned (*latencyMs)(void);

    // ABI 2: the route as it is *right now* - which endpoint, its master volume, whether it is muted.
    // Read on demand rather than only at open, because "我调了音量之后又没有了" is a question about
    // the moment after the open, and the answer has to be a number taken then. A backend that cannot
    // say writes an empty string; that is silence about the route, not silence of the wallpaper.
    void (*route)(char* out, unsigned bytes);

    // ABI 3: the loudest sample handed to the device in the last ~0.3 s, in permille of full scale,
    // measured after the gain. "ready / playing / underruns 0 / 48 000 frames a second" can all be
    // true while the stream is pure zeros; this is the one number that cannot.
    unsigned (*peak)(void);

    void (*close)(void);
};
