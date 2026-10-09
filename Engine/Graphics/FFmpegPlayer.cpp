// Engine/Graphics/FFmpegPlayer.cpp - the video backend: libavformat + libavcodec.
//
// This is the only one since 2026-10-07; the Media Foundation decoder that shipped before it is gone
// (see BACKLOG for the reasons and the measurements). Demux and decode are driven here, the picture
// comes out as one packed NV12 buffer, and `Nv12Uploader` turns that into the two shader resources
// Video.hlsl reads. The colour ruler is the standing judge of "did the pipeline survive a change":
// saturated bars within 2/255 of the fixture's own values and the 11-step grey ramp coming back +1,
// which is the pair that catches a matrix error, a row-order error and a plane-geometry error.
//
// Two numbers this file owns rather than infers: the coded height and the display height arrive
// separately from the codec context, which is why the crop is a choice here and not a guess from a
// buffer size. The row order is not a variable at all - libavcodec hands back top-down planes, which
// is what Nv12Uploader assumes outright; the flip that used to sit in that uploader was inferred from
// the fixture's own stored inversion rather than from a decoder, and it inverted every video. See
// Nv12Uploader.hpp and VideoPlayer's replacement, ProbeRowOrder, in the git history.
#include "Engine/Graphics/FFmpegPlayer.hpp"

#include "Engine/Graphics/Nv12Uploader.hpp"

#include "Engine/Core/Json.hpp"
#include "Engine/Core/Log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
}
// hwcontext_d3d11va.h pulls in the D3D11 headers, and those define C++ operators - so it must not sit
// inside extern "C" (the same trap tools/dxvaspike.cpp hit on its first compile).
#include <libavutil/hwcontext_d3d11va.h>

#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace sw {

namespace {
constexpr const char* MOD = "video";

// How many players this process is holding, and the same line when one goes away. This is the
// instrument that found the parked-decoder bug (browsing through a dozen clips left a decoder thread
// group and two plane textures alive per clip). It lived in the Media Foundation player and was
// deleted with it on 2026-10-07, which left the only backend with no way to count its own players -
// "opened 3, closed 0" in the log was uninterpretable until this came back.
std::atomic<int> gLivePlayers{0};

struct Clock {
    LARGE_INTEGER t{};
    Clock() { QueryPerformanceCounter(&t); }
    unsigned long long us() const {
        static const LARGE_INTEGER f = [] { LARGE_INTEGER x{}; QueryPerformanceFrequency(&x); return x; }();
        LARGE_INTEGER n{};
        QueryPerformanceCounter(&n);
        return (unsigned long long)((double)(n.QuadPart - t.QuadPart) * 1e6 / (double)f.QuadPart);
    }
};

void AddMax(std::atomic<unsigned long long>& slot, unsigned long long v) {
    unsigned long long cur = slot.load();
    while (v > cur && !slot.compare_exchange_weak(cur, v)) {
    }
}

// av_get_pix_fmt_name returns NULL when the format is unset, and std::format("%s", nullptr) is
// undefined behaviour that shows up as a BEX64 fastfail inside ucrtbase - which is what killed the
// first NVDEC run, far from the line that did it.
const char* FmtName(AVPixelFormat f) {
    const char* n = av_get_pix_fmt_name(f);
    return n ? n : "(none)";
}

// The preference list is terminated by AV_PIX_FMT_NONE (this avcodec's get_format takes two
// arguments, not three); NV12 is what the uploader wants, because chroma then costs one interleaved
// plane instead of two and the MF path already feeds the shader that layout.
AVPixelFormat PickFormat(AVCodecContext*, const AVPixelFormat* fmts) {
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    }
    return fmts[0];   // usually YUV420P; Pack() interleaves it
}

// The D3D11VA request: take the decoder's GPU surfaces when the codec offers them, otherwise behave
// exactly like PickFormat. Only installed on a context that was opened against our own device.
AVPixelFormat PickFormatD3D11(AVCodecContext*, const AVPixelFormat* fmts) {
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_D3D11) return AV_PIX_FMT_D3D11;
    }
    return PickFormat(nullptr, fmts);
}

// The D3D11VA device context locks our immediate context from its own thread. These are the callbacks
// libavcodec calls around every device use; the engine's device is created multithread-protected as
// well (D3D11Device.cpp), which is the part that makes sharing the immediate context legal.
inline std::string errcode(int e) {
    char b[128] = {};
    av_strerror(e, b, sizeof(b));
    return std::string("av error ") + std::to_string(e) + " (" + b + ")";
}

void DxLock(void* ctx) { AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(ctx)); }
void DxUnlock(void* ctx) { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(ctx)); }

// The NVDEC-backed twins of libavcodec's native decoders. Naming them is all it takes to ask for
// hardware decode; whether this machine's driver actually gives one is answered by avcodec_open2 and
// reported through note(), never assumed.
const char* CuvidName(const char* native) {
    if (!strcmp(native, "h264")) return "h264_cuvid";
    if (!strcmp(native, "hevc")) return "hevc_cuvid";
    if (!strcmp(native, "av1")) return "av1_cuvid";
    if (!strcmp(native, "vp9")) return "vp9_cuvid";
    return nullptr;
}

class FFmpegPlayer final : public VideoSource {
public:
    // GPU surfaces are the default since 2026-10-09, when the path passed the colour gate against the
    // CPU copy (identical per-bar deltas, 99.07% of the fixture's static region bit-identical - which is
    // less than the same clip differs between two of its own frames) and measured 14.38% -> 1.51% of a
    // core on the 4K60 desktop with the working set down from 348 MB to 142 MB. `SW_D3D11VA=0` next to
    // the exe drives the CPU path instead: that is how both sides are measured without a rebuild per
    // side, and it is the escape hatch for a driver elsewhere that paints something we cannot see.
    explicit FFmpegPlayer(bool preferNvdec) : preferNvdec_(preferNvdec) {
        char v[8] = {};
        gpuAllowed_ = GetEnvironmentVariableA("SW_D3D11VA", v, static_cast<DWORD>(sizeof(v))) == 0 ||
                      v[0] != '0';
    }

    ~FFmpegPlayer() override {
        {
            std::lock_guard lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        Free();
        const int live = --gLivePlayers;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        const BOOL got = K32GetProcessMemoryInfo(GetCurrentProcess(),
                                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                                 sizeof(pmc));
        Info(MOD, "closed a player: {} still live, process ws {:.0f} MB",
             live, got ? double(pmc.WorkingSetSize) / 1048576.0 : 0.0);
    }

    // Everything that does not touch our D3D11 device: the container, the streams, the decoder and the
    // hardware session. This is the half a worker thread is allowed to run.
    bool OpenFile(const std::wstring& file, bool loop, std::string& error) override {
        // Once per process, and from the loaded module: which build is running is a compliance answer
        // as much as a debugging one, and avcodec_configuration() is the only place that cannot drift
        // from the DLLs sitting next to the exe.
        static const bool buildLogged = [] {
            Info(MOD, "FFmpeg backend: {}", FfmpegBuildNote());
            Info(MOD, "FFmpeg was configured: {}",
                 avcodec_configuration() ? avcodec_configuration() : "(no configuration string)");
            return true;
        }();
        (void)buildLogged;
        file_ = file;
        loop_ = loop;
        const auto tStart = std::chrono::steady_clock::now();
        const auto msSince = [](std::chrono::steady_clock::time_point a) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        };
        const std::string utf8 = ToUtf8(file);
        if (avformat_open_input(&fmt_, utf8.c_str(), nullptr, nullptr) < 0) {
            error = "avformat_open_input failed: " + utf8;
            return false;
        }
        if (avformat_find_stream_info(fmt_, nullptr) < 0) {
            error = "avformat_find_stream_info failed";
            return false;
        }
        vIdx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &dec_, 0);
        if (vIdx_ < 0 || !dec_) {
            error = "no video stream";
            return false;
        }
        AVStream* st = fmt_->streams[vIdx_];
        tb_ = st->time_base;
        if (fmt_->duration > 0) duration_ = double(fmt_->duration) / double(AV_TIME_BASE);
        else if (st->duration > 0) duration_ = double(st->duration) * av_q2d(tb_);
        const AVRational r = av_guess_frame_rate(fmt_, st, nullptr);
        fps_ = r.den ? double(r.num) / double(r.den) : 0.0;
        const double demuxMs = msSince(tStart);

        // One helper so "which decoder did we actually get" cannot drift from "which one did we
        // manage to open": nvdec is tried first when asked for, plain and then with a CUDA device
        // handed to it, and the native decoder is the fallback. note() reports the winner.
        auto openWith = [&](const AVCodec* c, bool withCudaDevice) -> bool {
            avcodec_free_context(&ctx_);
            ctx_ = avcodec_alloc_context3(c);
            if (!ctx_ || avcodec_parameters_to_context(ctx_, st->codecpar) < 0) return false;
            ctx_->get_format = PickFormat;
            // libavcodec defaults to ONE thread, which measured 33 ms per 4K frame - half the frame
            // rate this clip needs. 0 means "choose by core count".
            ctx_->thread_count = 0;
            ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            if (withCudaDevice) {
                AVBufferRef* dev = nullptr;
                if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0)
                    return false;
                ctx_->hw_device_ctx = dev;   // the codec context owns the reference from here
            }
            if (avcodec_open2(ctx_, c, nullptr) < 0) return false;
            dec_ = c;
            codedW_ = ctx_->coded_width;
            codedH_ = ctx_->coded_height;
            w_.store(unsigned(ctx_->width ? ctx_->width : codedW_));
            h_.store(unsigned(ctx_->height ? ctx_->height : codedH_));
            return true;
        };
        // Don't open the NVDEC wrapper for the codecs the GPU-surface path is going to take over, or
        // for anything else where it is still the fastest way to decode: `h264_cuvid` copies its frame
        // back to system memory, which is the copy that path exists to remove, while `vp9_cuvid` has no
        // surface path of its own and is pure gain. The first version opened cuvid and then replaced it,
        // which put a CUDA device context and a D3D11 device context inside one player's lifetime with a
        // free in between - and the run died of heap corruption (c0000374) inside avcodec_free_context on
        // the throwaway instance a thumbnail build uses. Located with cdb (build/tmp/cdb-gpu.bat), not
        // guessed. The codec test is TryD3D11's own: a stream it will not touch keeps cuvid.
        const bool surfaces = gpuAllowed_ &&
                              (!strcmp(dec_->name, "h264") || !strcmp(dec_->name, "hevc"));
        const char* cu = (preferNvdec_ && !surfaces) ? CuvidName(dec_->name) : nullptr;
        const AVCodec* hw = cu ? avcodec_find_decoder_by_name(cu) : nullptr;
        bool opened = hw && (openWith(hw, false) || openWith(hw, true));
        if (opened) {
            note_ = std::string("nvdec libavcodec ") + hw->name;
        } else {
            if (hw) Info(MOD, "{} would not open on this driver; using the native decoder", cu);
            opened = openWith(dec_, false);
            note_ = std::string("sw libavcodec ") + dec_->name;
        }
        if (!opened) {
            error = std::string("avcodec_open2 failed for ") + dec_->name;
            return false;
        }
        const double codecMs = msSince(tStart) - demuxMs;
        pkt_ = av_packet_alloc();
        frm_ = av_frame_alloc();
        if (!pkt_ || !frm_) {
            error = "packet/frame alloc";
            return false;
        }
        Info(MOD, "opened {} via FFmpeg ({}x{}, {:.2f} fps, {:.1f} s, coded {} rows, pix fmt {})",
             ToUtf8(file_), w_.load(), h_.load(), fps_.load(), duration_.load(), codedH_,
             FmtName(ctx_->pix_fmt));
        Info(MOD, "{}: file open cost {:.1f} ms off the presenting thread - container/streams {:.1f}, "
                  "decoder {} {:.1f}",
             ToUtf8(file_), msSince(tStart), demuxMs, note_, codecMs);
        return true;
    }

    // The device half: the two plane textures and the thread that fills them. This is the part a switch
    // to a video still has to pay on the presenting thread, and it is a few milliseconds.
    bool Attach(ID3D11Device* dev, std::string& error) override {
        dev_ = dev;
        const auto t0 = std::chrono::steady_clock::now();
        // Ask for GPU surfaces before the plane textures are built: when this succeeds the textures are
        // still created (they are what a mid-stream switch to a software frame falls back to), but no
        // frame is ever copied through the CPU.
        const bool gpu = gpuAllowed_ && TryD3D11();
        if (!gpu && gpuAllowed_)
            Info(MOD, "{}: D3D11VA not available, decoding to system memory and copying the planes",
                 ToUtf8(file_));
        // Build the plane textures here, not on the first draw. `WallpaperInstance::MakeArgs` only
        // binds the pair once FrameY() hands one back, so a player whose first frame has not arrived
        // leaves the shader sampling empty slots - and empty slots read as zero, which the YUV->RGB
        // step turns into R0 G76 B0. That is the green: it appeared on a card built at startup and on
        // any desktop that had not received its first frame yet.
        if (!up_.Ensure(dev_.Get(), codedW_, codedH_, h_.load())) {
            error = "cannot build plane textures for " + std::to_string(codedW_) + "x"
                    + std::to_string(codedH_);
            Warn(MOD, "{}: {}", ToUtf8(file_), error);
            return false;
        }
        Info(MOD, "live players in this process: {}", ++gLivePlayers);
        thread_ = std::thread(&FFmpegPlayer::Thread, this);
        Info(MOD, "{}: attached to the device in {:.1f} ms (plane textures + decode thread started)",
             ToUtf8(file_),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }

    ID3D11ShaderResourceView* FrameY(ID3D11DeviceContext* ctx) override {
        const Clock whole;
        // Ensure BEFORE taking the frame: the first version of this checked it after the swap, so a
        // failure there consumed the picture and returned null without a word - which is how
        // "render took 0 frame(s)" with a decoder producing 31 steps/s looked like a decode problem.
        if (!up_.Ensure(dev_.Get(), codedW_, codedH_, h_.load())) {
            if (!ensureWarned_) {
                ensureWarned_ = true;
                Warn(MOD, "cannot build plane textures for {}x{} coded {} rows (display {} rows)",
                     codedW_, codedH_, codedW_, h_.load());
            }
            return nullptr;
        }
        std::vector<BYTE> frame;
        AVFrame* slot = nullptr;
        {
            std::lock_guard lk(mtx_);
            if (gpuOut_ && pendingAv_ && pendingAv_->data[0]) {
                // The reference is taken into the ring INSIDE the lock: the slot we hand to the shader
                // must survive however many swaps the decode thread does while our draw is in flight.
                slot = shownRing_[shownNext_];
                shownNext_ = (shownNext_ + 1) % 4;
                av_frame_unref(slot);
                if (av_frame_ref(slot, pendingAv_) < 0) slot = nullptr;
                av_frame_unref(pendingAv_);
            } else if (!pending_.empty()) {
                frame.swap(pending_);
            } else {
                // The ruler for "did skipping the copy cost the desktop a picture": a present that finds
                // nothing new shows the same frame as the one before it. Meaningful with one consumer -
                // when the second pass shares this player, its extra drains repeat legitimately.
                ++st_.tookNothing;
            }
        }
        if (slot && slot->data[0]) {
            const bool bound = up_.AdoptDecoded(
                reinterpret_cast<ID3D11Texture2D *>(slot->data[0]),
                static_cast<UINT>(reinterpret_cast<uintptr_t>(slot->data[1])),
                codedW_, codedH_, h_.load());
            if (bound) {
                ++st_.gpuTaken;
                st_.frameUsSum += whole.us();
                AddMax(st_.frameUsMax, whole.us());
                ++st_.taken;
                return up_.Y();
            }
            if (!adoptWarned_) {
                adoptWarned_ = true;
                Warn(MOD, "{}: decoder surface would not bind; drawing the CPU planes from here on",
                     ToUtf8(file_));
                std::lock_guard lk(mtx_);
                gpuOut_ = false;
                // Drop every surface reference we hold, or the pool keeps 24 decoder surfaces alive for
                // a path that will never read them again.
                if (pendingAv_) av_frame_unref(pendingAv_);
                for (auto& f : shownRing_) if (f) av_frame_unref(f);
            }
        }
        if (!frame.empty()) {
            Nv12Uploader::Out o;
            if (up_.Upload(ctx, frame.data(), frame.size(), o)) {
                st_.copyUsSum += o.copyUs;
                AddMax(st_.copyUsMax, o.copyUs);
                st_.lumaMean = o.lumaMean;
                st_.chromaMean = o.chromaMean;
                st_.haveBytes = o.haveBytes;
                st_.needBytes = o.needBytes;
                st_.frameUsSum += whole.us();
                AddMax(st_.frameUsMax, whole.us());
                ++st_.taken;
            }
        }
        PutBuffer(std::move(frame));   // the draw side was the other 12.4 MB allocation per frame
        return up_.Y();
    }
    ID3D11ShaderResourceView* FrameUV() override { return up_.UV(); }

    void SetActive(bool active) override {
        { std::lock_guard lk(mtx_); active_ = active; }
        cv_.notify_all();
    }
    void Pause() override {
        { std::lock_guard lk(mtx_); paused_ = true; }
        cv_.notify_all();
    }
    void Resume() override {
        { std::lock_guard lk(mtx_); paused_ = false; }
        cv_.notify_all();
    }
    bool paused() const override { return paused_.load(); }
    void Seek(double seconds) override { RequestSeek(seconds, false); }
    bool ShowFrameAt(double seconds, ID3D11DeviceContext* ctx,
                     const std::function<void()>& idle) override {
        // Already parked on this moment? Then the planes hold its picture and a second seek would decode
        // the whole GOP again for the same bytes. The card is built twice (small and large), so that
        // repeat cost was measured per clip: 121 frames to the first grab, 118 more to the second.
        {
            std::lock_guard lk(mtx_);
            if (holding_ && !holdNext_ && std::abs(position_.load() - seconds) < 0.25) return true;
        }
        unsigned long long s0 = 0;
        { std::lock_guard lk(mtx_); s0 = seekSeq_; }
        RequestSeek(seconds, true);
        // Wait for *this* seek to be taken, and read the frame count inside that same critical section:
        // only frames delivered after it can be the picture at `seconds`. The first version watched
        // `delivered_ > want && position_ >= seconds`, and the seek branch stamps position_ with the
        // requested time before a single frame has been decoded, while its flush empties the queue - so
        // this returned "found a frame" with the planes still holding their video-black clear. Measured
        // 2026-10-07: two of the three video packages lost their card that way (54 ms after open,
        // delivered stuck at 1, luma 0..0 over the whole 480x270 picture).
        const ULONGLONG until = GetTickCount64() + 700;
        unsigned long long base = 0;
        bool seeked = false;
        while (GetTickCount64() < until) {
            {
                std::lock_guard lk(mtx_);
                if (!seeked && seekSeq_ != s0) { base = delivered_.load(); seeked = true; }
            }
            if (seeked && delivered_.load() > base && position_.load() >= seconds - 0.05) break;
            if (idle) idle();
            Sleep(5);
        }
        FrameY(ctx);
        return seeked && delivered_.load() > base && position_.load() >= seconds - 0.2;
    }

    double position_s() const override { return position_.load(); }
    double duration_s() const override { return duration_.load(); }
    double fps() const override { return fps_.load(); }
    unsigned width() const override { return w_.load(); }
    unsigned height() const override { return h_.load(); }
    unsigned long long delivered() const override { return delivered_.load(); }
    bool ended() const override { return ended_.load(); }
    const std::string& note() const override { return note_; }

private:
    void RequestSeek(double seconds, bool hold) {
        {
            std::lock_guard lk(mtx_);
            seekPending_ = true;
            seekTo_ = seconds;
            holdAfterFrame_ = hold;
        }
        cv_.notify_all();
    }

    void Free() {
        if (frm_) av_frame_free(&frm_);
        if (pkt_) av_packet_free(&pkt_);
        if (pendingAv_) av_frame_free(&pendingAv_);
        for (auto& f : shownRing_) if (f) av_frame_free(&f);
        if (ctx_) avcodec_free_context(&ctx_);
        if (fmt_) avformat_close_input(&fmt_);
        // Order matters: the frames context holds a reference to the device context.
        if (hwFrames_) av_buffer_unref(&hwFrames_);
        if (hwDev_) av_buffer_unref(&hwDev_);
    }

    // AVFrame planes -> one packed NV12 buffer of codedW*codedH*3/2 bytes, honouring linesize.
    bool Pack(AVFrame* f, std::vector<BYTE>& out) {
        const int w = int(codedW_), h = int(codedH_);
        if (f->format == AV_PIX_FMT_NV12) {
            out.resize(size_t(w) * h * 3 / 2);
            for (int r = 0; r < h; ++r)
                memcpy(out.data() + size_t(r) * w, f->data[0] + size_t(r) * f->linesize[0], size_t(w));
            for (int r = 0; r < h / 2; ++r)
                memcpy(out.data() + size_t(w) * h + size_t(r) * w,
                       f->data[1] + size_t(r) * f->linesize[1], size_t(w));
            return true;
        }
        if (f->format == AV_PIX_FMT_YUV420P) {
            out.resize(size_t(w) * h * 3 / 2);
            for (int r = 0; r < h; ++r)
                memcpy(out.data() + size_t(r) * w, f->data[0] + size_t(r) * f->linesize[0], size_t(w));
            BYTE* uv = out.data() + size_t(w) * h;
            for (int r = 0; r < h / 2; ++r) {
                const BYTE* u = f->data[1] + size_t(r) * f->linesize[1];
                const BYTE* v = f->data[2] + size_t(r) * f->linesize[2];
                BYTE* d = uv + size_t(r) * w;
                for (int c = 0; c < w / 2; ++c) { d[2 * c] = u[c]; d[2 * c + 1] = v[c]; }
            }
            return true;
        }
        Warn(MOD, "decoder produced pix fmt {} - only NV12 and YUV420P are packed here",
             FmtName(AVPixelFormat(f->format)));   // AVFrame::format is an int
        return false;
    }

    void Thread();
    void LogStats();

    Com<ID3D11Device> dev_;
    std::wstring file_;
    std::string note_ = "none";
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    const AVCodec* dec_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;
    int vIdx_ = -1;
    AVRational tb_{};
    UINT codedW_ = 0, codedH_ = 0;
    bool loop_ = true;
    bool preferNvdec_ = false;
    bool gpuAllowed_ = false;            // SW_D3D11VA=1; see the constructor
    Nv12Uploader up_;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool active_ = true;
    bool seekPending_ = false;
    unsigned long long seekSeq_ = 0;   // bumped when a seek is taken; lets ShowFrameAt tell its own frames apart
    bool holding_ = false;
    bool holdNext_ = false;
    bool holdAfterFrame_ = false;
    bool ensureWarned_ = false;
    bool adoptWarned_ = false;           // the decoder surface failed to bind, once per player
    int noFrameSeconds_ = 0;    // consecutive stats lines with no step and no frame at all
    double holdTarget_ = 0.0;
    double seekTo_ = 0.0;
    std::vector<BYTE> pending_;

    // A 4K NV12 frame is 12.4 MB, and until 2026-10-08 both ends of this queue allocated one per
    // frame: the decode thread declared its `frame` inside the loop, and FrameY let the buffer it took
    // fall out of scope after the upload. That is 30 + 24 allocate/free pairs a second of a 12.4 MB
    // block (the decode step measured 4.50 ms per frame with the copy inside it). The pool keeps them
    // alive instead: Take hands back a buffer whose capacity is already there, Put returns one, and
    // after the first two frames the steady state allocates nothing.
    //
    // Three slots is the most that can be in flight at once: pending_, the buffer the draw is
    // uploading, and the one the decode thread is filling.
    std::vector<std::vector<BYTE>> pool_;

    std::vector<BYTE> TakeBuffer() {
        std::lock_guard lk(mtx_);
        int pick = -1;
        for (size_t i = 0; i < pool_.size(); ++i) {
            if (!pool_[i].empty()) continue;                 // somebody is still holding that slot
            if (pool_[i].capacity()) { pick = int(i); break; }  // a real buffer: take it
            pick = int(i);                                   // usable, just without capacity yet
        }
        if (pick < 0) {
            if (pool_.size() >= 3) return {};
            pool_.emplace_back();
            pick = int(pool_.size()) - 1;
        }
        std::vector<BYTE> out;
        out.swap(pool_[pick]);        // the slot keeps whatever the caller had in its place
        return out;
    }
    void PutBuffer(std::vector<BYTE>&& b) {
        if (b.capacity() == 0) return;
        std::vector<BYTE> kept(std::move(b));
        kept.clear();               // the bytes stay, the size becomes zero: an empty slot
        std::lock_guard lk(mtx_);
        for (auto& s : pool_) {
            if (s.capacity() && s.empty()) return;      // already holding an equal-sized buffer
            if (s.empty() && s.capacity() < kept.capacity()) { s.swap(kept); return; }
        }
        if (pool_.size() < 3) pool_.push_back(std::move(kept));
    }

    // ---- D3D11VA: the decoder writes GPU surfaces and the shader reads them (no 12.4 MB trip) ----
    //
    // `pending_` stays the CPU path's slot; `pendingAv_` is the same idea for a surface. The ring below
    // is the part that is easy to get wrong: returning the AVFrame reference hands the surface back to
    // the decoder's pool, and the decoder may overwrite it while our draw that bound it is still queued
    // on the GPU. Holding the reference for a few more takes is a guarantee rather than a heuristic -
    // a referenced surface is not handed out - and the pool is ~20 surfaces deep, so four is free.
    bool TryD3D11() {
        if (!dev_ || !fmt_) return false;
        // The NATIVE decoder is the one with the D3D11VA hwaccel attached. Asking by the currently open
        // codec's name was the first attempt and it never engaged, because OpenFile prefers the NVDEC
        // wrapper - `h264_cuvid` copies the frame to system memory, which is the copy this path exists
        // to remove. Look the native one up by codec id instead.
        const AVCodec* native = avcodec_find_decoder(fmt_->streams[vIdx_]->codecpar->codec_id);
        if (!native) { Info(MOD, "d3d11va: no native decoder for this stream"); return false; }
        if (strcmp(native->name, "h264") && strcmp(native->name, "hevc")) {
            Info(MOD, "d3d11va: {} decodes to system memory by nature, staying on the CPU path",
                 native->name);
            return false;
        }
        static SRWLOCK d3dLock = SRWLOCK_INIT;
        bool ok = false;
        std::string why = "?";
        AVBufferRef* dref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        AVBufferRef* fref = nullptr;
        AVCodecContext* c = nullptr;
        ID3D11DeviceContext* imm = nullptr;
        do {
            if (!dref) { why = "device ctx alloc"; break; }
            AVD3D11VADeviceContext* d3d =
                (AVD3D11VADeviceContext *)((AVHWDeviceContext *)dref->data)->hwctx;
            dev_->GetImmediateContext(&imm);
            if (!imm) { why = "no immediate context"; break; }
            d3d->device = dev_.Get();
            d3d->device->AddRef();
            d3d->device_context = imm;
            d3d->lock = DxLock;
            d3d->unlock = DxUnlock;
            d3d->lock_ctx = &d3dLock;
            // DEVICE level, not frames level: the hwaccel builds the array itself and reads the flags
            // from here. Without SHADER_RESOURCE every CreateShaderResourceView fails with
            // E_INVALIDARG, which is what the first spike run measured (bind 0x200, seven views, zero
            // successes) and what makes the whole approach work or not.
            d3d->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
            if (const int r = av_hwdevice_ctx_init(dref); r < 0) { why = errcode(r); break; }
            fref = av_hwframe_ctx_alloc(dref);
            if (!fref) { why = "frames ctx alloc"; break; }
            AVHWFramesContext* fc = (AVHWFramesContext *)fref->data;
            fc->format = AV_PIX_FMT_D3D11;
            fc->sw_format = AV_PIX_FMT_NV12;
            fc->width = codedW_;
            fc->height = codedH_;
            fc->initial_pool_size = 24;
            ((AVD3D11VAFramesContext *)fc->hwctx)->BindFlags = d3d->BindFlags;
            if (const int r = av_hwframe_ctx_init(fref); r < 0) { why = errcode(r); break; }
            c = avcodec_alloc_context3(native);
            if (!c || avcodec_parameters_to_context(c, fmt_->streams[vIdx_]->codecpar) < 0) { why = "codec ctx"; break; }
            c->get_format = PickFormatD3D11;
            // One decoder thread, because the software frame threads were the crash: with 0 (= auto)
            // the run died of heap corruption (c0000374) reported from a free of an AVBufferRef - inside
            // avcodec_free_context while the NVDEC wrapper was also being opened, and inside
            // avcodec_send_packet once it was not. Same build, this one line different. Application
            // Verifier's page heap still stopped it at the free rather than at a write, so it is a
            // dangling reference in the decoder's own bookkeeping; which part of that bookkeeping frame
            // threading breaks is not established. What costs nothing to lose: the GPU is doing the
            // decoding here, and the step measured 0.6 ms at 4K60 against 4.3 ms on the CPU path.
            c->thread_count = 1;
            c->hw_device_ctx = av_buffer_ref(dref);
            c->hw_frames_ctx = av_buffer_ref(fref);
            if (const int r = avcodec_open2(c, native, nullptr); r < 0) { why = errcode(r); break; }
            avcodec_free_context(&ctx_);
            ctx_ = c;
            c = nullptr;
            hwDev_ = dref;
            dref = nullptr;
            hwFrames_ = fref;
            fref = nullptr;
            pendingAv_ = av_frame_alloc();
            for (auto& f : shownRing_) f = av_frame_alloc();
            if (!pendingAv_ || !shownRing_[0]) break;
            gpuOut_ = true;
            dec_ = native;
            note_ = std::string("d3d11va libavcodec ") + native->name;
            ok = true;
        } while (false);
        if (!ok) Info(MOD, "{}: D3D11VA unavailable ({})", ToUtf8(file_), why);
        // No imm->Release() here on purpose. GetImmediateContext handed us one reference, and that
        // reference is what lives in AVD3D11VADeviceContext::device_context now - libavcodec's device
        // uninit releases both pointers it was given, on the success path and on the failure one.
        // Releasing it here as well dropped the context to zero refs while the engine was still
        // drawing with it, which is the crash the first GPU-on run died on (the log stopped at
        // "aurora prepared", no process left, a dump under %LOCALAPPDATA%\CrashDumps).
        if (c) avcodec_free_context(&c);
        if (fref) av_buffer_unref(&fref);
        if (dref) av_buffer_unref(&dref);
        return ok;
    }

    // True when the slot already holds a picture nobody has drawn yet - the backpressure test, which
    // has to look at a different place depending on which kind of picture this is.
    bool SlotFull() const { return gpuOut_ ? (pendingAv_ && pendingAv_->data[0]) : !pending_.empty(); }

    AVFrame* pendingAv_ = nullptr;
    AVFrame* shownRing_[4] = {};
    unsigned shownNext_ = 0;
    bool gpuOut_ = false;
    AVBufferRef* hwDev_ = nullptr;
    AVBufferRef* hwFrames_ = nullptr;

    // The loop seam, timed on the decode thread only: when the clip ran out, and what the last frame
    // shown before the cut was. The queue the decoder holds at that moment is the clip's own tail, so
    // it is played out, and the number that matters is the gap between the last tail frame and the
    // first frame of the next cycle - that is the freeze a viewer would call a hitch.
    bool seamOpen_ = false;
    std::chrono::steady_clock::time_point lastShownAt_{};
    double lastShownPos_ = -1.0;

    std::atomic<bool> paused_{false};
    std::atomic<bool> ended_{false};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<double> fps_{0.0};
    std::atomic<unsigned> w_{0}, h_{0};
    std::atomic<unsigned long long> delivered_{0};

    struct Stats {
        std::atomic<unsigned long long> decodeUsSum{0}, decodeUsMax{0};
        std::atomic<unsigned long long> packUsSum{0}, packUsMax{0};       // the 12.4 MB copy, timed on its own
        std::atomic<unsigned long long> copyUsSum{0}, copyUsMax{0};   // the two UpdateSubresource calls
        std::atomic<unsigned long long> frameUsSum{0}, frameUsMax{0};
        std::atomic<unsigned> steps{0}, empty{0}, dropped{0}, taken{0}, notPacked{0}, packSteps{0},
                                    tookNothing{0}, gpuTaken{0};
        std::atomic<unsigned> lumaMean{0}, chromaMean{0}, haveBytes{0}, needBytes{0};
    } st_;
    std::chrono::steady_clock::time_point lastStats_;
};

void FFmpegPlayer::LogStats() {
    lastStats_ = std::chrono::steady_clock::now();
    const auto steps = st_.steps.exchange(0), empty = st_.empty.exchange(0);
    const auto taken = st_.taken.exchange(0), dropped = st_.dropped.exchange(0);
    const auto notPacked = st_.notPacked.exchange(0);
    const auto packSteps = st_.packSteps.exchange(0);
    const auto gpuTaken = st_.gpuTaken.exchange(0);
    const auto tookNothing = st_.tookNothing.exchange(0);
    const auto pSum = st_.packUsSum.exchange(0), pMax = st_.packUsMax.exchange(0);
    const auto dSum = st_.decodeUsSum.exchange(0), dMax = st_.decodeUsMax.exchange(0);
    const auto cSum = st_.copyUsSum.exchange(0), cMax = st_.copyUsMax.exchange(0);
    const auto fSum = st_.frameUsSum.exchange(0), fMax = st_.frameUsMax.exchange(0);
    const double k = 1.0 / 1000.0;
    Info(MOD, "{}: FFmpeg decode {} steps in 1 s ({} empty), step mean {:.1f} max {:.1f} ms | "
              "render took {} frame(s), FrameY mean {:.2f} max {:.2f} ms (upload {:.2f}/{:.2f}) | "
              "{} produced but never displayed, {} decoded and not copied, packed {} frame(s) mean {:.2f} "
              "max {:.2f} ms, {} presents took nothing, {} drawn straight off the decoder surface",
         ToUtf8(file_), steps, empty, steps ? dSum * k / steps : 0.0, dMax * k, taken,
         taken ? fSum * k / taken : 0.0, fMax * k,
         taken ? cSum * k / taken : 0.0, cMax * k, dropped, notPacked, packSteps,
         packSteps ? pSum * k / packSteps : 0.0, pMax * k, tookNothing, gpuTaken);
    if (gpuOut_) {
        // Not a measurement of the picture. This path never calls Upload(), so the two means would keep
        // reporting the video-black clear of plane textures nobody writes - a green-looking number with
        // nothing wrong. The colour of a frame drawn off the decoder surface is measured where it lands:
        // composited pixels (tools/preview-pixels.py).
        Info(MOD, "{}: {} decoder surface(s) bound this second, the CPU planes are not written by this "
                  "path", ToUtf8(file_), gpuTaken);
    } else {
        Info(MOD, "{}: planes hold luma mean {} chroma mean {} (neutral chroma is 128), frame {} B "
                  "against a coded plane of {} B",
             ToUtf8(file_), st_.lumaMean.load(), st_.chromaMean.load(), st_.haveBytes.load(),
             st_.needBytes.load());
    }
    // The state worth shouting about is "the file opened, the thread is working, and not one frame has
    // ever come out" - steps above zero with every step empty. The first version of this checked
    // `steps == 0`, which is the opposite: zero steps just means nobody is asking right now (parked for
    // a thumbnail grab, paused, or hidden), and that is normal, not a fault. With the import probe in
    // place a file that cannot be decoded never reaches a player at all, so this line is the last
    // witness rather than the first.
    const bool idle = [&] { std::lock_guard lk(mtx_); return !active_ || paused_ || holding_; }();
    if (delivered_.load() == 0 && !idle) {
        if (++noFrameSeconds_ >= 3)
            Warn(MOD, "{}: open for {} s, {} steps and {} of them empty, not one frame produced - "
                      "anything drawing this wallpaper shows video black, not the clip",
                 ToUtf8(file_), noFrameSeconds_, steps, empty);
    } else {
        noFrameSeconds_ = 0;
    }
}

void FFmpegPlayer::Thread() {
    double fps = fps_.load();
    if (fps < 1.0) fps = 30.0;
    const auto period = std::chrono::microseconds(static_cast<long long>(1e6 / fps + 0.5));
    auto nextDue = std::chrono::steady_clock::now();
    lastStats_ = nextDue;
    int idleStreak = 0;

    while (true) {
        double seekSeconds = -1.0;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] {
                return stop_ || seekPending_ || (active_ && !paused_ && !holding_);
            });
            if (stop_) return;
            if (seekPending_) {
                seekPending_ = false;
                seekSeconds = seekTo_;
            }
        }
        if (std::chrono::steady_clock::now() - lastStats_ >= std::chrono::seconds(1)) LogStats();
        if (seekSeconds >= 0.0) {
            // A loop rewind is not a seek: it lands on the clip's first IDR, and an IDR refreshes every
            // reference frame by itself, so the decoder is kept warm. Measured 2026-10-07 on the 4K
            // clip: flushing it discarded the 15 frames still queued for display and put 105-118 ms in
            // front of the next cycle's first frame (against a 5.3 ms steady-state step); keeping them
            // costs one pacing interval. The Media Foundation path follows the same lesson.
            const bool rewinding = seamOpen_;
            const int64_t target = int64_t(seekSeconds / av_q2d(tb_));
            av_seek_frame(fmt_, vIdx_, target, AVSEEK_FLAG_BACKWARD);
            std::vector<BYTE> retired;
            {
                std::lock_guard lk(mtx_);
                ended_ = false;
                // A user seek throws the picture it jumped away from and starts clean: it can land in the
                // middle of a GOP, where only a flush gets decodable output. A loop rewind does neither -
                // the reader ran out while this cycle's last frames were still queued undisplayed, so
                // they play out, and stamping 0 s over them would jump the position back for a quarter
                // of a second before running forward again.
                if (!rewinding) {
                    avcodec_flush_buffers(ctx_);
                    position_ = seekSeconds;
                    retired.swap(pending_);   // emptied, and the 12.4 MB block goes back to the pool
                    holding_ = false;
                    holdNext_ = holdAfterFrame_;
                    holdTarget_ = seekSeconds;
                    holdAfterFrame_ = false;
                    // Last, so anyone watching this counter from the other side of the lock sees a queue
                    // that is already empty and a position that is already stamped - nothing can look like
                    // a frame delivered by this seek before one exists.
                    ++seekSeq_;
                }
            }
            PutBuffer(std::move(retired));
            nextDue = std::chrono::steady_clock::now();
            continue;
        }

        const Clock stepClock;
        std::vector<BYTE> frame = TakeBuffer();
        long long outTs = 0;
        bool gotFrame = false;
        bool skipped = false;
        for (;;) {
            const int rr = av_read_frame(fmt_, pkt_);
            if (rr < 0) {
                if (rr == AVERROR_EOF) {
                    ended_ = true;
                    if (loop_) seamOpen_ = true;
                    {
                        std::lock_guard lk(mtx_);
                        if (loop_) { seekPending_ = true; seekTo_ = 0.0; ended_ = false; }
                        else holding_ = true;
                    }
                    cv_.notify_all();
                }
                ++st_.empty;
                break;
            }
            if (int(pkt_->stream_index) != vIdx_) { av_packet_unref(pkt_); continue; }
            if (avcodec_send_packet(ctx_, pkt_) < 0) { av_packet_unref(pkt_); ++st_.empty; break; }
            av_packet_unref(pkt_);
            const int rc = avcodec_receive_frame(ctx_, frm_);
            if (rc == AVERROR(EAGAIN)) { ++st_.empty; continue; }
            if (rc < 0) { ++st_.empty; break; }
            const int64_t pts = frm_->best_effort_timestamp != AV_NOPTS_VALUE ? frm_->best_effort_timestamp
                                                                             : frm_->pts;
            outTs = pts != AV_NOPTS_VALUE ? int64_t(pts * av_q2d(tb_) * 1e7) : 0;
            // A 4K NV12 picture is 12.4 MB, and Pack() copies it on the decode thread while the desktop
            // is trying to draw. If the pending slot still holds the picture from the last step, this one
            // will be swapped out before any frame is drawn - copied, uploaded by nobody, freed. Measured
            // 2026-10-08: the decoder runs at the clip's own 30 fps, the slot is capped to 24, so 6 of
            // every 30 pictures (74 MB/s) were packed for nothing; the log called them `dropped`.
            // Publishing only into an empty slot costs the same picture the desktop would have taken -
            // one frame per present, the present phase decides which - and skips the copy instead of
            // doing it and throwing it away.
            // Two cases must keep every picture: running to a park target (the thumbnails ask for a named
            // moment and the only proof that it arrived is position_ reaching it, which only a published
            // frame stamps), and the first picture after a seek, which is what the hero transition waits
            // for. The pending slot is empty in both, so neither is special-cased here beyond the guard.
            bool held = false;
            {
                std::lock_guard lk(mtx_);
                held = SlotFull() && !holdNext_;
            }
            if (held) {
                av_frame_unref(frm_);
                ++st_.notPacked;
                skipped = true;
                break;
            }
            if (gpuOut_ && frm_->format == AV_PIX_FMT_D3D11) {
                // No copy at all: the picture stays where the decoder put it and the publish below takes
                // a reference. `frm_` is deliberately not unref'd here - the publish block refs it.
                gotFrame = true;
                break;
            }
            const Clock packClock;
            if (!Pack(frm_, frame)) { av_frame_unref(frm_); ++st_.empty; break; }
            const unsigned long long packUs = packClock.us();
            st_.packUsSum += packUs;
            AddMax(st_.packUsMax, packUs);
            ++st_.packSteps;
            av_frame_unref(frm_);
            gotFrame = true;
            break;
        }
        const unsigned long long stepUs = stepClock.us();
        st_.decodeUsSum += stepUs;
        AddMax(st_.decodeUsMax, stepUs);
        ++st_.steps;

        if (gotFrame || skipped) {
            // A skipped picture is still a step of the clip's clock: pacing it like a published frame is
            // what keeps position_ advancing at wall-clock speed (each published frame is now one clip
            // interval further along than the last present, so the caption does not drift behind the file)
            // and keeps the decoder from running ahead into the next GOP.
            idleStreak = 0;
            if (gotFrame) {
                // The braces are load-bearing: std::mutex is not recursive, and holding this guard
                // into the pacing wait below made _Mtx_lock return thrd_busy, which MSVC turns into a
                // std::system_error that escapes the thread -> terminate/abort (BEX64 in ucrtbase).
                // Measured 2026-10-07: the renderer died ~2 s after `local_05 prepared`.
                std::lock_guard lk(mtx_);
                if (gpuOut_) {
                    av_frame_unref(pendingAv_);
                    if (av_frame_ref(pendingAv_, frm_) < 0) {
                        ++st_.empty;
                    } else {
                        position_ = double(outTs) / 1e7;
                        ++delivered_;
                    }
                } else {
                    if (!pending_.empty()) ++st_.dropped;
                    pending_.swap(frame);
                    position_ = double(outTs) / 1e7;
                    ++delivered_;
                }
                if (holdNext_ && (position_ >= holdTarget_ - 0.02 || ended_.load())) {
                    holding_ = true;
                    holdNext_ = false;
                }
            }
            if (gpuOut_) av_frame_unref(frm_);   // the reference now lives in pendingAv_
            cv_.notify_all();
            // `frame` now holds whatever that swap displaced - the buffer the previous pending frame
            // lived in - and it goes back to the pool instead of being freed. On the skipped path it is
            // the empty buffer this step took out of the pool, which has to go back the same way or the
            // pool loses a slot every time the desktop is behind.
            PutBuffer(std::move(frame));
            const double pos = position_.load();
            const auto nowT = std::chrono::steady_clock::now();
            if (gotFrame && seamOpen_ && pos + 0.0005 < lastShownPos_) {
                // The cut. `lastShownPos_/lastShownAt_` is by definition the cycle's last picture - the
                // one the position just jumped back from - so the gap from it to this frame is the hitch
                // a viewer could call a freeze. The pair this used to report (seamTailAt_/seamTailPos_)
                // only filled in when a tail frame happened to still be in the queue when the reader ran
                // out; on the GPU path the queue drains first, and the line printed `-1.000 s` and a gap
                // of 3.0e8 ms measured against a default-constructed clock. Measured on the CPU path the
                // same evening: 4.683 s -> 0.000 s, 7.4-7.7 ms apart.
                Info(MOD, "{}: loop seam: last frame of the cycle {:.3f} s of {:.1f}, first frame of the "
                          "next {:.3f} s, {:.1f} ms apart on the decode side",
                     ToUtf8(file_), lastShownPos_, duration_.load(), pos,
                     std::chrono::duration<double, std::milli>(nowT - lastShownAt_).count());
                seamOpen_ = false;
            }
            if (gotFrame) {
                lastShownPos_ = pos;
                lastShownAt_ = nowT;
            }
            nextDue += period;
            if (nextDue < nowT) nextDue = nowT;
            // Running toward a park target is not playback: every intermediate picture is thrown away
            // before it is ever displayed, so pacing them would add one frame interval per frame between
            // the previous keyframe and the requested moment (a 60 fps clip with its IDR 2 s back would
            // need 120 of them, which is longer than the grab's whole budget and leaves the card
            // unwritten). Only a grab parks, and the instance it parks is never the one on screen.
            const bool runningToTarget = [&] { std::lock_guard lk(mtx_); return holdNext_; }();
            if (!runningToTarget && nowT < nextDue) {
                std::unique_lock wlk(mtx_);
                cv_.wait_until(wlk, nextDue, [&] { return stop_ || seekPending_ || !active_ || paused_; });
            }
        } else {
            if (++idleStreak > 4) {
                std::unique_lock ulk(mtx_);
                cv_.wait_for(ulk, period, [&] { return stop_ || seekPending_ || !active_ || paused_; });
            }
        }
    }
}

} // namespace

std::unique_ptr<VideoSource> MakeFFmpegPlayer(bool preferNvdec) {
    return std::make_unique<FFmpegPlayer>(preferNvdec);
}

std::string FfmpegBuildNote() {
    const unsigned v = avcodec_version();
    return std::format("{} libavcodec {}.{}.{} {}", av_version_info() ? av_version_info() : "(unknown)",
                       AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v),
                       avcodec_license() ? avcodec_license() : "(no license string)");
}

bool FfmpegProbeVideo(const std::wstring& file, VideoProbe& out, std::string& error) {
    const std::string utf8 = ToUtf8(file);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open " + utf8;
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        error = "no stream information in " + utf8;
        avformat_close_input(&fmt);
        return false;
    }
    const AVCodec* dec = nullptr;
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (idx < 0 || !dec) {
        error = "no video stream, or no decoder for it, in " + utf8;
        avformat_close_input(&fmt);
        return false;
    }
    AVStream* st = fmt->streams[idx];
    const AVCodecParameters* cp = st->codecpar;
    out.w = unsigned(cp->width);
    out.h = unsigned(cp->height);
    const AVRational r = av_guess_frame_rate(fmt, st, nullptr);
    out.fps = r.den ? double(r.num) / double(r.den) : 0.0;
    if (fmt->duration > 0) out.duration_s = double(fmt->duration) / double(AV_TIME_BASE);
    else if (st->duration > 0 && st->time_base.den)
        out.duration_s = double(st->duration) * av_q2d(st->time_base);
    // Same rule the shader's bt601 parameter has always used: only a *declared* 601-family matrix
    // switches it, because an unstored one is BT.709 for anything HD. FFmpeg reports the matrix, not
    // the primaries, which is the quantity that actually matters for YuvToRgb.
    out.bt601 = cp->color_space == AVCOL_SPC_SMPTE170M || cp->color_space == AVCOL_SPC_SMPTE240M ||
                cp->color_space == AVCOL_SPC_BT470BG || cp->color_space == AVCOL_SPC_FCC;
    out.codec = dec->name;
    avformat_close_input(&fmt);
    if (!out.w || !out.h) {
        error = "the video has no readable frame size: " + utf8;
        return false;
    }
    return true;
}

} // namespace sw
