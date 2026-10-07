#include "Engine/Graphics/VideoPlayer.hpp"
#include "Engine/Core/Log.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <psapi.h>

#include <chrono>
#include <cstring>

namespace sw {
static constexpr const char* MOD = "video";

namespace {
// How many players this process is holding. The cost of a video wallpaper is per instance, so the
// first question about a big working set is "how many are alive" - the browse page used to leave one
// decoding per clip it had ever shown.
std::atomic<int> gLivePlayers{0};

double MemMb(BOOL priv) {
    PROCESS_MEMORY_COUNTERS_EX p{};
    p.cb = sizeof(p);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),
                                 sizeof(p)))
        return 0.0;
    return double(priv ? p.PrivateUsage : p.WorkingSetSize) / (1024.0 * 1024.0);
}

// Microseconds since construction, for splitting one frame's cost into decode / transpose / upload.
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
} // namespace

VideoPlayer::~VideoPlayer() {
    {
        std::lock_guard lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    xform_.Reset();
    outType_.Reset();
    nativeType_.Reset();
    reader_.Reset();
    uvSrv_.Reset();
    ySrv_.Reset();
    uvTex_.Reset();
    yTex_.Reset();
    dev_.Reset();
    const int live = --gLivePlayers;
    Info(MOD, "closed a player: {} still live, process ws {:.0f} MB private {:.0f} MB", live, MemMb(FALSE),
         MemMb(TRUE));
}

bool VideoPlayer::Open(ID3D11Device* dev, const std::wstring& file, bool loop, std::string& error) {
    dev_ = dev;
    file_ = file;
    loop_ = loop;
    active_ = true;

    if (!OpenDemuxer()) {
        error = "cannot demux " + ToUtf8(file_) + " (no video stream?)";
        return false;
    }
    if (!OpenDecoder()) {
        error = "no usable H.264-class decoder transform for " + ToUtf8(file_);
        return false;
    }
    const double privConfigured = MemMb(TRUE);
    privAtOpenMb_ = privConfigured;   // the delta below is then "what decoding costs", not "what opening costs"
    wsAtOpenMb_ = MemMb(FALSE);
    Info(MOD, "decoder configured, nothing decoded yet: ws {:.0f} MB private {:.0f} MB", wsAtOpenMb_,
         privConfigured);

    PROPVARIANT var{};
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8)
        duration_ = double(var.ulVal) / 1e7;
    PropVariantClear(&var);

    Info(MOD, "opened {} ({}x{}, {:.2f} fps, {:.2f} s, coded {} rows, decoder {})", ToUtf8(file_), w_.load(),
         h_.load(), fps_.load(), duration_.load(), codedH_, note_);
    ++gLivePlayers;
    wsAtOpenMb_ = MemMb(FALSE);
    privAtOpenMb_ = MemMb(TRUE);
    Info(MOD, "live players in this process: {} (at open ws {:.0f} MB, private {:.0f} MB)",
         gLivePlayers.load(), wsAtOpenMb_, privAtOpenMb_);
    thread_ = std::thread(&VideoPlayer::Thread, this);
    return true;
}

// The reader is only a demuxer here: no attributes, so it will not try to convert anything, and
// SetCurrentMediaType is never called (that call is what this machine refuses).
bool VideoPlayer::OpenDemuxer() {
    if (FAILED(MFCreateSourceReaderFromURL(file_.c_str(), nullptr, &reader_))) return false;
    // No SetStreamSelection here. Deselecting every stream and re-selecting the video one looked like a
    // free saving (a wallpaper never plays the audio), and it changed nothing measurable; the thumbnail
    // grab failure was reproduced with it both present and absent, so it is not the cause either.
    if (FAILED(reader_->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeType_)))
        return false;
    UINT32 ww = 0, hh = 0, num = 0, den = 0;
    if (FAILED(MFGetAttributeSize(nativeType_.Get(), MF_MT_FRAME_SIZE, &ww, &hh)) || !ww || !hh) return false;
    if (SUCCEEDED(MFGetAttributeRatio(nativeType_.Get(), MF_MT_FRAME_RATE, &num, &den)) && den)
        fps_ = double(num) / double(den);
    codedW_ = w_ = ww;
    codedH_ = dispH_ = h_ = hh;
    return true;
}

bool VideoPlayer::OpenDecoder() {
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    // Hardware first: on a vendor that registers one (Intel does) this same code decodes on the GPU.
    // Measured here: NVIDIA registers no hardware H.264 decoder with MF, so this falls to software.
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, &out, &acts, &n)) || n == 0) {
        n = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                             MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                                 MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n)) || n == 0)
            return false;
        note_ = "sw";
    } else {
        note_ = "hw";
    }
    WCHAR* friendly = nullptr;
    UINT32 chars = 0;
    acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &chars);
    if (friendly) {
        char utf8[160] = {};
        WideCharToMultiByte(CP_UTF8, 0, friendly, -1, utf8, sizeof(utf8), nullptr, nullptr);
        note_ += std::string(" ") + utf8;
        CoTaskMemFree(friendly);
    }
    const long hr = acts[0]->ActivateObject(IID_PPV_ARGS(&xform_));
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return false;

    if (FAILED(xform_->SetInputType(0, nativeType_.Get(), 0))) {
        Com<IMFMediaType> clean;
        MFCreateMediaType(&clean);
        nativeType_->CopyAllItems(clean.Get());
        if (FAILED(xform_->SetInputType(0, clean.Get(), 0))) {
            Warn(MOD, "SetInputType refused: {}", HResultToString(hr));
            return false;
        }
    }

    Com<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, codedW_, codedH_);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType_ = t;
    if (!NegotiateOutput()) return false;

    xform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    xform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    // Each rewind step has to out-distance every timestamp the clip can report, or the rebased clock
    // would catch up with itself once the clip has looped a few times.
    loopBias_ = LONGLONG((duration_.load() + 2.0) * 1e7);
    if (loopBias_ < 10000000LL) loopBias_ = 10000000LL;   // no readable duration: use 10 s
    return true;
}

bool VideoPlayer::NegotiateOutput() {
    xform_->SetOutputType(0, outType_.Get(), 0);
    xform_->GetInputStreamInfo(0, &isi_);
    xform_->GetOutputStreamInfo(0, &osi_);
    // What the transform asks us to allocate per output call. At 4K this is the number that decides
    // whether the buffer churn, or Media Foundation's own reference pool, is what the working set is
    // really paying for.
    Info(MOD, "stream info: in cbSize {} out cbSize {} out flags 0x{:X} ({} bytes per output attempt)",
         isi_.cbSize, osi_.cbSize, (unsigned)osi_.dwFlags, osi_.cbSize);
    return osi_.cbSize > 0 && isi_.cbSize > 0;
}

// After MF_E_TRANSFORM_STREAM_CHANGE the coded surface is bigger than the container claimed
// (measured: 1080 rows become 1088) and the only place that number is readable is the buffer size the
// transform now asks for. This MFT reports cbSize = w*h*2 for NV12, so h follows from that.
bool VideoPlayer::HandleStreamChange() {
    xform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    xform_->SetOutputType(0, nullptr, 0);
    xform_->GetOutputStreamInfo(0, &osi_);
    const UINT32 hh = UINT32(osi_.cbSize / (2 * UINT64(codedW_)));
    if (hh >= h_.load() && hh <= h_.load() + 64) codedH_ = hh;
    MFSetAttributeSize(outType_.Get(), MF_MT_FRAME_SIZE, codedW_, codedH_);
    outType_->SetUINT32(MF_MT_DEFAULT_STRIDE, codedW_);
    outType_->SetUINT32(MF_MT_SAMPLE_SIZE, codedW_ * codedH_ * 3 / 2);
    const long hr = NegotiateOutput() ? S_OK : E_FAIL;
    Info(MOD, "stream change: coded rows now {} (display {}), renegotiate hr={}", codedH_, h_.load(),
         HResultToString(hr));
    return SUCCEEDED(hr);
}

// One step: feed a compressed sample, then take whatever comes out. Returns true when out holds a
// decoded NV12 frame; false when the transform needs more input (the caller keeps feeding).
bool VideoPlayer::PumpOne(std::vector<BYTE>& out, long long& outTs) {
    DWORD stream = 0, flags = 0;
    LONGLONG ts = 0, dur = 0;
    IMFSample* raw = nullptr;
    const long rh = reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts,
                                        &raw);
    Com<IMFSample> comp;
    comp.Attach(raw);
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        ended_ = true;
        lastStepHard_ = false;  // the loop restarts through the seek path, that is not a fault
        return false;
    }
    if (FAILED(rh) || !comp) {
        lastStepHard_ = FAILED(rh);
        return false;
    }

    Com<IMFMediaBuffer> srcBuf;
    if (FAILED(comp->GetBufferByIndex(0, &srcBuf))) {
        lastStepHard_ = true;
        return false;
    }
    BYTE* src = nullptr;
    DWORD srcLen = 0;
    srcBuf->Lock(&src, nullptr, &srcLen);
    // isi_.cbSize is an alignment allowance, not a maximum: sizing the buffer to it truncates every
    // NAL unit (measured: a 8434 byte first sample cut to 4096 decoded nothing at all).
    Com<IMFMediaBuffer> inBuf;
    Com<IMFSample> inSample;
    const bool got = srcLen > 0 && SUCCEEDED(MFCreateAlignedMemoryBuffer(srcLen + UINT32(isi_.cbSize), 32,
                                                                        &inBuf)) &&
                     SUCCEEDED(MFCreateSample(&inSample));
    if (got) {
        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        inBuf->Lock(&dst, &maxLen, nullptr);
        memcpy(dst, src, srcLen);
        inBuf->Unlock();
        inBuf->SetCurrentLength(srcLen);
        inSample->AddBuffer(inBuf.Get());
        inSample->SetSampleTime(ts + bias_);
        inSample->SetSampleDuration(dur ? dur : LONGLONG(1e7 / (fps_ > 1 ? fps_.load() : 30.0)));
    }
    srcBuf->Unlock();
    if (!got) {
        lastStepHard_ = true;
        return false;
    }

    long hr = xform_->ProcessInput(0, inSample.Get(), 0);
    for (int retry = 0; retry < 3 && hr == MF_E_NOTACCEPTING; ++retry) {
        // Output is queued: pull it before retrying the same input, or that sample is lost for good.
        MFT_OUTPUT_DATA_BUFFER probe{};
        Com<IMFMediaBuffer> pb;
        Com<IMFSample> ps;
        if (!(osi_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            if (FAILED(MFCreateAlignedMemoryBuffer(osi_.cbSize, 32, &pb)) || FAILED(MFCreateSample(&ps)))
                return false;
            ps->AddBuffer(pb.Get());
            probe.pSample = ps.Get();
        }
        DWORD st = 0;
        xform_->ProcessOutput(0, 1, &probe, &st);
        if (probe.pSample && probe.pSample != ps.Get()) probe.pSample->Release();
        hr = xform_->ProcessInput(0, inSample.Get(), 0);
    }
    if (FAILED(hr)) {
        if (hr != MF_E_NOTACCEPTING) Warn(MOD, "ProcessInput: {}", HResultToString(hr));
        lastStepHard_ = true;
        return false;
    }
    lastStepHard_ = false;

    lastStepHard_ = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        DWORD status = 0;
        MFT_OUTPUT_DATA_BUFFER odb{};
        // A fresh output sample per call, deliberately: this decoder keeps a reference to the sample it
        // wrote, and handing the same IMFSample back the next time is refused with 0x80004005 E_FAIL -
        // measured here, that turned the feed loop into a 19-core spin that delivered one frame.
        Com<IMFMediaBuffer> ob;
        Com<IMFSample> os;
        if (osi_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
            hr = xform_->ProcessOutput(0, 1, &odb, &status);
        } else {
            if (FAILED(MFCreateAlignedMemoryBuffer(osi_.cbSize, 32, &ob)) || FAILED(MFCreateSample(&os)))
                return false;
            os->AddBuffer(ob.Get());
            odb.pSample = os.Get();
            hr = xform_->ProcessOutput(0, 1, &odb, &status);
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // Asking for more input is the transform working, not failing. Treating it as a fault is
            // what made every card picture pure green: the first frame of an H.264 stream needs ~35
            // samples fed before anything comes out, throttling those steps to one per frame interval
            // pushed the first frame past ShowFrameAt's 700 ms budget, and the shader was left
            // sampling an unbound plane pair (luma 0, U=V=0 -> R 3 G 173 B 0).
            lastStepHard_ = false;
            return false;
        }
        // Release only a sample the transform handed us. The one we created is held by os, and
        // releasing it here drops its last reference so os's own release is a double free: measured on
        // this machine, that corrupted the heap and came back as a purecall inside MFReadWrite on the
        // decode thread seconds later, with the stream-change branch firing on every clip's first frame.
        const bool foreign = odb.pSample && odb.pSample != os.Get();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (foreign) odb.pSample->Release();
            if (!HandleStreamChange()) return false;
            continue;
        }
        if (FAILED(hr) || !odb.pSample) {
            if (FAILED(hr)) Warn(MOD, "ProcessOutput: {}", HResultToString(hr));
            if (foreign) odb.pSample->Release();
            lastStepHard_ = true;
            return false;
        }
        Com<IMFMediaBuffer> resBuf;
        long long outT = 0;
        odb.pSample->GetSampleTime(&outT);
        odb.pSample->GetSampleDuration(&dur);
        odb.pSample->GetBufferByIndex(0, &resBuf);
        BYTE* p = nullptr;
        DWORD maxLen = 0, len = 0;
        resBuf->Lock(&p, &maxLen, &len);
        const size_t need = size_t(codedW_) * size_t(codedH_) * 3 / 2;
        if (len >= need) {
            out.resize(need);
            memcpy(out.data(), p, need);
            outTs = outT;
        }
        resBuf->Unlock();
        if (foreign) odb.pSample->Release();
        if (len < need) {
            lastStepHard_ = true;
            return false;
        }
        return true;
    }
    return false;
}

// One line a second per live player, because "the video stutters" has three different places it can
// mean: the decode step on this thread, the transpose+upload on the render thread, and frames the
// decoder produced that the render thread never looked at.
void VideoPlayer::LogStats() {
    lastStats_ = std::chrono::steady_clock::now();
    const auto steps = st_.steps.exchange(0), empty = st_.empty.exchange(0);
    const auto taken = st_.taken.exchange(0), dropped = st_.dropped.exchange(0);
    const auto dSum = st_.decodeUsSum.exchange(0), dMax = st_.decodeUsMax.exchange(0);
    const auto tSum = st_.transUsSum.exchange(0), tMax = st_.transUsMax.exchange(0);
    const auto cSum = st_.copyUsSum.exchange(0), cMax = st_.copyUsMax.exchange(0);
    const auto fSum = st_.frameUsSum.exchange(0), fMax = st_.frameUsMax.exchange(0);
    const double k = 1.0 / 1000.0;  // microseconds to milliseconds
    Info(MOD,
         "{}: decode {} steps in 1 s ({} empty), step mean {:.1f} max {:.1f} ms | render took {} frame(s), "
         "FrameY mean {:.2f} max {:.2f} ms (transpose {:.2f}/{:.2f}, upload {:.2f}/{:.2f}) | {} produced but "
         "never displayed | ws {:.0f} MB private {:.0f} MB",
         ToUtf8(file_), steps, empty, steps ? dSum * k / steps : 0.0, dMax * k, taken,
         taken ? fSum * k / taken : 0.0, fMax * k, taken ? tSum * k / taken : 0.0, tMax * k,
         taken ? cSum * k / taken : 0.0, cMax * k, dropped, MemMb(FALSE), MemMb(TRUE));
    Info(MOD, "{}: planes hold luma mean {} chroma mean {} (neutral chroma is 128), frame {} B "
             "against a coded plane of {} B",
         ToUtf8(file_), st_.lumaMean.load(), st_.chromaMean.load(), st_.haveBytes.load(),
         st_.needBytes.load());
}

void VideoPlayer::Thread() {
    const long co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    double fps = fps_.load();
    if (fps < 1.0) fps = 30.0;
    const auto period = std::chrono::microseconds(static_cast<long long>(1e6 / fps + 0.5));
    auto nextDue = std::chrono::steady_clock::now();
    std::vector<BYTE> frame;
    long long ts = 0;
    bool loggedFirst = false;
    int idleStreak = 0;

    while (true) {
        double seekSeconds = -1.0;
        bool doLoop = false;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] { return stop_ || seekPending_ || loopPending_ ||
                                          (active_ && !paused_ && !drained_ && !holding_); });
            if (stop_) break;
            if (seekPending_) {
                seekPending_ = false;
                seekSeconds = seekTo_;
            } else if (loopPending_) {
                loopPending_ = false;
                doLoop = true;
            }
        }
        if (std::chrono::steady_clock::now() - lastStats_ >= std::chrono::seconds(1)) LogStats();
        if (doLoop) {
            // Rewind the demuxer only. No flush, no pending_.clear(): the pictures still inside the
            // transform are the clip's own tail, and they belong on screen while the head warms up.
            // This is what removes the 146-167 ms freeze every loop used to cause (the flush itself
            // measured 66-105 ms and the re-priming 117-129 ms, with file reads only 2.6-3.5 ms).
            PROPVARIANT start{};
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = 0;
            const long hr = reader_->SetCurrentPosition(GUID_NULL, start);
            PropVariantClear(&start);
            if (FAILED(hr)) Warn(MOD, "loop rewind: {}", HResultToString(hr));
            prevBias_ = bias_;
            bias_ += loopBias_;
            nextDue = std::chrono::steady_clock::now();
            continue;
        }
        if (seekSeconds >= 0.0) {
            PROPVARIANT start{};
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = LONGLONG(seekSeconds * 1e7);
            const long hr = reader_->SetCurrentPosition(GUID_NULL, start);
            PropVariantClear(&start);
            if (FAILED(hr)) Warn(MOD, "seek {:.2f}s: {}", seekSeconds, HResultToString(hr));
            xform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            {
                std::lock_guard lk(mtx_);
                pending_.clear();
                // A real seek breaks the reference chain on purpose (the transform is flushed) and the
                // clock starts over: the loop bias only belongs to rewinds that keep the pipeline warm.
                bias_ = 0;
                prevBias_ = 0;
                // Park *after* the frame this seek brings, not before it: holding_ gates the wait at the
                // top of the loop, so setting it here would strand the caller of ShowFrameAt waiting for
                // a frame the thread is no longer allowed to produce.
                holding_ = false;
                holdNext_ = holdAfterFrame_;
                holdTarget_ = seekSeconds;
                holdAfterFrame_ = false;
                drained_ = false;
                position_ = seekSeconds;
                ++seekApplied_;
            }
            nextDue = std::chrono::steady_clock::now();
            continue;
        }

        const Clock stepClock;
        const bool gotFrame = PumpOne(frame, ts);
        const unsigned long long stepUs = stepClock.us();
        st_.decodeUsSum += stepUs;
        AddMax(st_.decodeUsMax, stepUs);
        ++st_.steps;
        if (gotFrame) {
            idleStreak = 0;
            if (!loggedFirst) {
                loggedFirst = true;
                lastStats_ = std::chrono::steady_clock::now();
                Info(MOD, "first decoded frame ({}x{} coded, {} bytes)", codedW_, codedH_, frame.size());
                const double ours =
                    (2.0 * codedW_ * codedH_ * 1.5 + codedW_ * h_ * 1.5 + codedW_ * h_ * 1.5) / 1048576.0;
                Info(MOD, "{}: this player has cost ws {:+.0f} MB, private {:+.0f} MB so far; ours is "
                          "~{:.0f} MB (2 frame copies + transpose staging + 2 plane textures)",
                     ToUtf8(file_), MemMb(FALSE) - wsAtOpenMb_, MemMb(TRUE) - privAtOpenMb_, ours);
            }
            {
                std::lock_guard lk(mtx_);
                if (!pending_.empty()) ++st_.dropped;  // the render thread never took the last one
                pending_.swap(frame);  // newest wins: an undisplayed frame is dropped, never queued.
                                       // A copy here would be 12 MB per frame on a 4K clip.
                // ts is the sample's own (biased) time; take it back to clip time. A picture still on
                // its way out of the pipeline after the rewind carries the *previous* cycle's bias.
                position_ = double(ts >= bias_ ? ts - bias_ : ts - prevBias_) / 1e7;
                ++delivered_;
                // Park only once the clip has actually reached the moment that was asked for. The
                // first frame out of a seek is the previous keyframe, and for a clip whose keyframes
                // carry no recovery point it can be a neutral filler - which is what made every card
                // of the synthetic fixture a flat grey picture.
                if (holdNext_ && (position_ >= holdTarget_ - 0.02 || ended_.load())) {
                    holding_ = true;
                    holdNext_ = false;
                }
            }
            cv_.notify_all();
            const auto nowT = std::chrono::steady_clock::now();
            nextDue += period;
            if (nextDue < nowT) nextDue = nowT;   // fell behind: skip ahead, never bank a burst
            if (nowT < nextDue) {
                std::unique_lock lk(mtx_);
                cv_.wait_until(lk, nextDue,
                               [&] { return stop_ || seekPending_ || !active_ || paused_ || drained_; });
            }
        } else {
            ++st_.empty;
            // Only a *hard* failure is worth backing off from. "needs more input" is the transform
            // working, and an H.264 stream needs ~35 of those before the first picture out; throttling
            // them starved ShowFrameAt past its 700 ms budget and every card came out pure green.
            if (!lastStepHard_) idleStreak = 0;
            // Measured here after an output-buffer mistake: 0x80004005 every 6 ms, the whole file read
            // to the end, the picture standing still and the process at 1900 % of one core. So back off
            // to one attempt per frame interval instead of spinning, and say so once.
            if (++idleStreak > 4) {
                std::unique_lock lk(mtx_);
                cv_.wait_for(lk, period, [&] { return stop_ || seekPending_ || !active_ || paused_; });
            }
            if (idleStreak == 200)
                Warn(MOD, "{}: no decoded frame in {} attempts, throttled to one per {:.1f} ms",
                     ToUtf8(file_), idleStreak, period.count() / 1000.0);
            std::lock_guard lk(mtx_);
            if (ended_ && !loop_) drained_ = true;
            else if (ended_ && loop_) {
                // Loop without touching the transform: rewinding the demuxer and keeping the pipeline
                // warm is what makes the seam invisible. See the doLoop branch above.
                loopPending_ = true;
                ended_ = false;
            }
        }
    }
    if (co == S_OK) CoUninitialize();
}

void VideoPlayer::SetActive(bool active) {
    {
        std::lock_guard lk(mtx_);
        active_ = active;
    }
    cv_.notify_all();
}

void VideoPlayer::Pause() {
    {
        std::lock_guard lk(mtx_);
        paused_ = true;
    }
    cv_.notify_all();
}

void VideoPlayer::Resume() {
    {
        std::lock_guard lk(mtx_);
        paused_ = false;
    }
    cv_.notify_all();
}

void VideoPlayer::RequestSeek(double seconds, bool hold) {
    {
        std::lock_guard lk(mtx_);
        seekTo_ = seconds < 0.0 ? 0.0 : seconds;
        if (duration_ > 0.0 && seekTo_ > duration_) seekTo_ = duration_;
        seekPending_ = true;
        holdAfterFrame_ = hold;  // park after the frame this seek brings, not before it
        drained_ = false;
    }
    // Without this the second grab of a thumbnail pass hangs: the first one parks the thread on
    // holding_, and a parked thread only re-checks its predicate when somebody notifies. The seek
    // flag alone is invisible to it - measured as "ShowFrameAt found a frame" for the still and
    // "TIMED OUT" for the large picture of the same clip.
    cv_.notify_all();
}

void VideoPlayer::Seek(double seconds) { RequestSeek(seconds, false); }

bool VideoPlayer::EnsurePlanes() {
    const UINT w = codedW_, dy = h_.load(), dh = dy / 2;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (!yTex_) {
        td.Height = dy;
        td.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &yTex_))) return false;
        if (FAILED(dev_->CreateShaderResourceView(yTex_.Get(), nullptr, &ySrv_))) return false;
        // NV12 chroma is half size in BOTH axes: one R8G8 texel carries the U/V pair for a 2x2 block
        // of luma, so a 1920-wide picture has 960 chroma texels in 1920 bytes. Sizing the texture to
        // w texels makes the driver read twice the row I own - measured here as an access violation
        // inside nvwgf2umx on UpdateSubresource, and heap corruption that surfaced later as a purecall
        // inside MFReadWrite.
        td.Width = w / 2;
        td.Height = dh;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &uvTex_))) return false;
        if (FAILED(dev_->CreateShaderResourceView(uvTex_.Get(), nullptr, &uvSrv_))) return false;
        Info(MOD, "plane textures {}x{} R8 + {}x{} R8G8", w, dy, w / 2, dh);
    } else if (yTex_ && (yTex_->GetDesc(&td), td.Width != w || td.Height != dy)) {
        yTex_.Reset();
        uvTex_.Reset();
        ySrv_.Reset();
        uvSrv_.Reset();
        return EnsurePlanes();
    }
    return true;
}

// The decoded plane is bottom-up and has more rows than the picture displays; the upload is where
// both are fixed, so the shader (and Image.hlsl's rule that v=0 is the top row) stay untouched.
void VideoPlayer::UploadPlanes(ID3D11DeviceContext* ctx, const BYTE* nv12, size_t have) {
    const UINT w = codedW_, dispH = h_.load();
    const size_t yPlane = size_t(w) * codedH_;
    rows_.resize(size_t(w) * dispH);
    chroma_.resize(size_t(w) * (dispH / 2));
    const Clock t0;
    for (UINT r = 0; r < dispH; ++r)
        memcpy(rows_.data() + size_t(r) * w, nv12 + size_t(codedH_ - 1 - r) * w, size_t(w));
    for (UINT r = 0; r < dispH / 2; ++r)
        memcpy(chroma_.data() + size_t(r) * w, nv12 + yPlane + size_t(codedH_ / 2 - 1 - r) * w, size_t(w));
    const unsigned long long transUs = t0.us();
    const Clock t1;
    ctx->UpdateSubresource(yTex_.Get(), 0, nullptr, rows_.data(), w, 0);
    ctx->UpdateSubresource(uvTex_.Get(), 0, nullptr, chroma_.data(), w, 0);
    const unsigned long long copyUs = t1.us();
    // UpdateSubresource returns nothing, so a blank chroma plane can only be seen in the bytes before
    // they go up. The green thumbnails measure R 3 / G 173 / B 0, which is exactly "luma present,
    // U=V=0" - so log what each plane actually holds and compare the grab path with the live path.
    unsigned long long ySum = 0, uSum = 0;
    size_t yn = 0, un = 0;
    for (size_t i = 0; i < rows_.size(); i += 97) { ySum += rows_[i]; ++yn; }
    for (size_t i = 0; i < chroma_.size(); i += 97) { uSum += chroma_[i]; ++un; }
    st_.lumaMean = yn ? (unsigned)(ySum / yn) : 0;
    st_.chromaMean = un ? (unsigned)(uSum / un) : 0;
    st_.haveBytes = (unsigned)have;
    st_.needBytes = (unsigned)(size_t(w) * codedH_ * 3 / 2);
    st_.transUsSum += transUs;
    AddMax(st_.transUsMax, transUs);
    st_.copyUsSum += copyUs;
    AddMax(st_.copyUsMax, copyUs);
}

ID3D11ShaderResourceView* VideoPlayer::FrameY(ID3D11DeviceContext* ctx) {
    const Clock whole;
    std::vector<BYTE> frame;
    {
        std::lock_guard lk(mtx_);
        if (!pending_.empty()) frame.swap(pending_);
    }
    if (!frame.empty()) {
        if (!EnsurePlanes()) return nullptr;
        UploadPlanes(ctx, frame.data(), frame.size());
        st_.frameUsSum += whole.us();
        AddMax(st_.frameUsMax, whole.us());
        ++st_.taken;
    }
    return ySrv_.Get();
}

ID3D11ShaderResourceView* VideoPlayer::FrameUV() {
    return uvSrv_.Get();
}

bool VideoPlayer::ShowFrameAt(double seconds, ID3D11DeviceContext* ctx) {
    const unsigned gen = seekApplied_.load();
    RequestSeek(seconds, true);
    const ULONGLONG until = GetTickCount64() + 700;
    while (seekApplied_.load() == gen && GetTickCount64() < until) Sleep(5);
    const unsigned long long want = delivered_.load();
    while (GetTickCount64() < until &&
           (delivered_.load() == want || position_.load() < seconds - 0.05))
        Sleep(5);
    FrameY(ctx);
    return delivered_.load() > want && position_.load() >= seconds - 0.2;
}

} // namespace sw
