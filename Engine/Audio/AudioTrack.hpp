#pragma once
// Engine/Audio/AudioTrack.hpp - one decoded audio file, played through the audio backend.
//
// The decode side of the seam: libavformat/libavcodec in, float32 PCM out, on its own thread, into
// whatever AudioOut resolved to. Only AudioTrack.cpp sees libav* - the same containment rule
// Engine/Graphics/FFmpegPlayer.cpp follows, so no FFmpeg type escapes into the engine proper.
//
// What this deliberately is not: a clock. Between the jumps it is told about it has no idea what the
// picture is doing. That is the half that matters for a movie: the track used to free-run and rewind
// only at its own end of file, so dragging the picture left the dialogue where it was, and the two
// laps (picture 7351.712 s, audio whatever its own stream length is) did not have to agree. `Seek` is
// the cheap fix - whoever owns the picture tells this track where that picture went. Timing the
// picture to THIS track is the other half, and it is not here yet.
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
    // Move this track to `seconds` of its own file. Called by whoever owns the picture when that
    // picture jumped - a seek, or the loop seam - because a track that only knows its own clock puts
    // a movie's dialogue seconds away from its lips on the first drag of the bar. Best-effort: the
    // samples already handed to the backend (up to the plugin's ring, ~85 ms) still play out first.
    void Seek(double seconds) { seekUs_.store(static_cast<long long>(seconds * 1e6),
                                              std::memory_order_release); }

    // Whether the track is actually feeding the backend. `Start()` only hands the file to a thread,
    // and that thread can still fail on its first avformat call - a file that is not a media file, a
    // container with no audio stream, a codec with no decoder. "playing" in the status block has to
    // mean the second thing, not the first, or a silent wallpaper reports itself as sounding.
    bool running() const { return state_.load() == 1; }    // Spawned, still inside avformat_open_input. `Start` returns true here, so a caller asking
    // "is it dead" from `running` alone would call every fresh track dead for its first 20 ms.
    bool starting() const { return state_.load() == 0; }
    // Why the thread stopped, empty while it is running or when the file simply reached its end.
    std::string error() const;
    // Frames handed to the backend, and how many times the track rewound. Together they say whether
    // the decoder is keeping up (frames/s should sit at the device rate) and whether the loop point
    // is being reached at all.
    unsigned long long framesWritten() const { return written_.load(); }
    unsigned long long loops() const { return loops_.load(); }
    // Where the demuxer of this track itself is, in seconds of its own file, from the pts of the last
    // packet it took. -1 before the first one. This is the number that says whether a `Seek` landed:
    // the picture's position and this one have to agree, and nothing else in the status block shows
    // the audio side's place at all.
    double mediaPos_s() const { const long long u = mediaUs_.load(); return u < 0 ? -1.0 : double(u) / 1e6; }

private:
    void Run();

    AudioOut* out_ = nullptr;
    std::wstring file_;
    bool loop_ = true;
    std::thread thread_;
    std::atomic<long> stop_{0};
    std::atomic<unsigned long long> written_{0};
    std::atomic<unsigned long long> loops_{0};
    // microseconds into this file, -1 when nothing is owed. One producer (the frame loop), one
    // consumer (the track thread), and the consumer takes it with exchange() so a jump is acted on
    // exactly once.
    std::atomic<long long> seekUs_{-1};
    // The demuxer's own place, microseconds in the file, kept from each packet's pts. Written by the
    // track thread, read by whoever asks the engine what is audible.
    std::atomic<long long> mediaUs_{-1};
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
