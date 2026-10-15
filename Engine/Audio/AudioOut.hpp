#pragma once
// Engine/Audio/AudioOut.hpp - the engine's side of the audio seam.
//
// Loads a backend DLL by absolute path from the engine's own directory and hands out its function
// table. Every way this can fail - no file, no symbol, another ABI, no output device, the device
// unplugged between two launches - means the same thing to the rest of the engine: no sound. None of
// them is a reason for a wallpaper not to run, so none of them is logged as an error either.
//
// Shape borrowed from Engine/App/GpuMonitor.cpp, which has done this with nvml.dll since the GPU
// metrics landed.
#include "Engine/Audio/IAudioPlugin.h"

#include <string>

namespace sw {

class AudioOut {
public:
    AudioOut() = default;
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Loads and opens the backend. `note` always says what happened, in the same string the log and
    // the status block show, because "why is it silent" has to be answerable without a debugger.
    bool Open(std::string& note);
    void Close();

    bool ready() const { return p_ && p_->write; }
    const char* backend() const { return p_ && p_->name ? p_->name : ""; }
    unsigned rate() const { return rate_; }
    unsigned channels() const { return ch_; }

    // Frames taken, fewer than asked means the ring is full and the caller must keep the rest.
    int write(const float* pcm, unsigned frames) {
        return ready() ? p_->write(pcm, frames) : 0;
    }
    void setVolume(float v) { if (p_ && p_->setVolume) p_->setVolume(v); }
    void setPaused(bool paused) { if (p_ && p_->setPaused) p_->setPaused(paused ? 1 : 0); }
    unsigned underruns() const { return p_ && p_->underruns ? p_->underruns() : 0u; }
    // Permille of full scale, post-gain, over the last ~0.3 s. 0 while "playing" means we are writing
    // silence and the device is happily eating it.
    unsigned peak() const { return p_ && p_->peak ? p_->peak() : 0u; }
    unsigned latencyMs() const { return p_ && p_->latencyMs ? p_->latencyMs() : 0u; }
    // Which endpoint the device is on right now, and at what level. Empty when the backend has no
    // answer for it - that is a missing reading, not a reason to stop the wallpaper.
    std::string route() {
        if (!p_ || !p_->route) return {};
        char buf[256] = {};
        p_->route(buf, sizeof(buf));
        return buf;
    }

private:
    void* lib_ = nullptr;
    const SwAudioPlugin* p_ = nullptr;
    unsigned rate_ = 0, ch_ = 0;
};

} // namespace sw
