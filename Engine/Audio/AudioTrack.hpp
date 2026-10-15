#pragma once
// Engine/Audio/AudioTrack.hpp - one decoded audio file, played through the audio backend.
//
// The decode side of the seam: libavformat/libavcodec in, float32 PCM out, on its own thread, into
// whatever AudioOut resolved to. Only AudioTrack.cpp sees libav* - the same containment rule
// Engine/Graphics/FFmpegPlayer.cpp follows, so no FFmpeg type escapes into the engine proper.
//
// What this deliberately is not: a clock. The track free-runs and loops by rewinding the demuxer.
// Frame-accurate audio against a video wallpaper would have to ride the same seek/loop seam as the
// picture, and that seam already costs 7.7-22 ms per lap - an ambient bed (rain under a snow scene)
// does not care, and paying for sync it does not need is how the sound ends up drifting anyway.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace sw {

class AudioOut;

class AudioTrack {
public:
    AudioTrack() = default;
    ~AudioTrack();
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // Starts playing `file` (an absolute path) through `out`. `note` always says what happened.
    // Returns false when the file has no audio stream, no decoder, or cannot be opened at all.
    bool Start(AudioOut& out, const std::wstring& file, bool loop, std::string& note);
    void Stop();

    // Whether the track is actually feeding the backend. `Start()` only hands the file to a thread,
    // and that thread can still fail on its first avformat call - a file that is not a media file, a
    // container with no audio stream, a codec with no decoder. "playing" in the status block has to
    // mean the second thing, not the first, or a silent wallpaper reports itself as sounding.
    bool running() const { return state_.load() == 1; }
    // Why the thread stopped, empty while it is running or when the file simply reached its end.
    std::string error() const;
    // Frames handed to the backend, and how many times the track rewound. Together they say whether
    // the decoder is keeping up (frames/s should sit at the device rate) and whether the loop point
    // is being reached at all.
    unsigned long long framesWritten() const { return written_.load(); }
    unsigned long long loops() const { return loops_.load(); }

private:
    void Run();

    AudioOut* out_ = nullptr;
    std::wstring file_;
    bool loop_ = true;
    std::thread thread_;
    std::atomic<long> stop_{0};
    std::atomic<unsigned long long> written_{0};
    std::atomic<unsigned long long> loops_{0};
    // 0 = handed off, 1 = feeding the backend, 2 = the thread is done (finished, or gave up).
    std::atomic<int> state_{0};
    // Written by the track thread on its way out, read by whoever asks the engine what is playing.
    // Off the audio path: one string, guarded because two threads touch it, never held while samples
    // move.
    mutable std::mutex errMtx_;
    std::string err_;
    void Fail(const std::string& why);
};

} // namespace sw
