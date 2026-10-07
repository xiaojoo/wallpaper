#pragma once
// Engine/Graphics/VideoPlayer.hpp - a video file playing as a wallpaper.
//
// Why this does not use the source reader's own format conversion: measured on this machine, every
// SetCurrentMediaType is refused (E_INVALIDARG, pdwResult=0) - RGB32 or NV12, with or without a DXGI
// device manager, minimal type or a clone of the native one - while the reader happily hands back
// compressed H.264 samples. MFTEnumEx shows why the convenience layer cannot be used here and also
// what can: MF discovers `Microsoft H264 Video Decoder MFT` (software) and NVIDIA registers no
// hardware H.264 decoder with MF at all (only NVENC plus an MJPEG decoder). So the reader is used as
// a demuxer and the decoder transform is driven by hand. Enumerating hardware first means the same
// code gets GPU decode on a machine whose vendor registers it.
//
// Two measured facts from tools/mkdecode.cpp shaped this: the decoder's NV12 plane is stored
// bottom-up (row 0 is the picture's last row), and after the first keyframes the transform reports
// MF_E_TRANSFORM_STREAM_CHANGE with a coded height larger than the container's (1080 -> 1088),
// whose real size is only readable from GetOutputStreamInfo().cbSize.
#include "Engine/Core/Platform.hpp"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace sw {

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Decodes <file> at its own frame rate on a worker thread. loop restarts it at the end.
    bool Open(ID3D11Device* dev, const std::wstring& file, bool loop, std::string& error);

    // The current picture as two planes: t0 = luma (R8), t1 = chroma (R8G8). FrameY uploads the newest
    // decoded frame and returns the luma view, so it must be called first; null before the first frame.
    ID3D11ShaderResourceView* FrameY(ID3D11DeviceContext* ctx);
    ID3D11ShaderResourceView* FrameUV();

    void SetActive(bool active);
    void Pause();
    void Resume();
    bool paused() const { return paused_.load(); }
    void Seek(double seconds);
    // Park on the frame at this time and return it: the thumbnails need named moments.
    bool ShowFrameAt(double seconds, ID3D11DeviceContext* ctx);

    double position_s() const { return position_.load(); }
    double duration_s() const { return duration_.load(); }
    double fps() const { return fps_.load(); }
    UINT width() const { return w_.load(); }
    UINT height() const { return h_.load(); }
    unsigned long long delivered() const { return delivered_.load(); }
    bool ended() const { return ended_.load(); }
    // Which decoder was chosen, for status: "sw" or "hw", plus the friendly name.
    const std::string& note() const { return note_; }

private:
    void Thread();
    bool OpenDemuxer();
    bool OpenDecoder();
    bool NegotiateOutput();                       // after MF_E_TRANSFORM_STREAM_CHANGE
    bool HandleStreamChange();
    bool PumpOne(std::vector<BYTE>& out, long long& outTs);
    bool EnsurePlanes();
    void UploadPlanes(ID3D11DeviceContext* ctx, const BYTE* nv12, size_t have);
    void RequestSeek(double seconds, bool hold);

    Com<ID3D11Device> dev_;
    Com<IMFSourceReader> reader_;
    Com<IMFMediaType> nativeType_;
    Com<IMFMediaType> outType_;
    Com<IMFTransform> xform_;
    Com<ID3D11Texture2D> yTex_, uvTex_;
    Com<ID3D11ShaderResourceView> ySrv_, uvSrv_;

    std::wstring file_;
    std::string note_ = "none";
    bool loop_ = true;
    UINT codedW_ = 0, codedH_ = 0, dispH_ = 0;
    bool lastStepHard_ = false;  // did the last PumpOne fail on a hard error, or is the transform just
                                 // asking for more input (the normal state for the first ~35 samples)
    MFT_INPUT_STREAM_INFO isi_{};
    MFT_OUTPUT_STREAM_INFO osi_{};

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool active_ = true;
    bool drained_ = false;
    bool holding_ = false;
    bool holdAfterFrame_ = false;  // a park-and-grab seek, armed
    bool holdNext_ = false;        // park after the next frame is published
    double holdTarget_ = 0.0;      // ... but only once the picture has reached this moment
    bool seekPending_ = false;
    double seekTo_ = 0.0;
    std::vector<BYTE> pending_;      // newest decoded NV12 frame, waiting for the render thread
    std::vector<BYTE> rows_, chroma_; // staging for the top-down copy, reused between frames: at 4K60
                                      // allocating these per frame is 12 MB of malloc and page faults
                                      // a second on the render thread
    std::atomic<unsigned> seekApplied_{0};

    std::atomic<bool> paused_{false};
    std::atomic<bool> ended_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<double> fps_{0.0};
    std::atomic<UINT> w_{0}, h_{0};
    std::atomic<unsigned long long> delivered_{0};

    // Timing counters for the once-a-second `stats` log line. Split deliberately: the decode step runs
    // on this thread, the transpose and the upload run on the render thread, so "the video is slow"
    // has three different places it could mean.
    struct Stats {
        std::atomic<unsigned long long> decodeUsSum{0}, decodeUsMax{0};
        std::atomic<unsigned long long> transUsSum{0}, transUsMax{0};   // bottom-up -> top-down copy
        std::atomic<unsigned long long> copyUsSum{0}, copyUsMax{0};     // UpdateSubresource itself
        std::atomic<unsigned long long> frameUsSum{0}, frameUsMax{0};   // whole FrameY, render thread
        std::atomic<unsigned long long> steps{0}, empty{0}, dropped{0}, taken{0};
        std::atomic<unsigned> lumaMean{0}, chromaMean{0}, haveBytes{0}, needBytes{0};
    } st_;

    void LogStats();
    std::chrono::steady_clock::time_point lastStats_;
    double wsAtOpenMb_ = 0, privAtOpenMb_ = 0;
};

} // namespace sw
