// Engine/Audio/AudioTrack.cpp - libavformat/libavcodec on one audio stream, into the backend.
//
// The only audio translation unit that includes libav*, matching Engine/Graphics/FFmpegPlayer.cpp:
// nothing outside this file can take on an FFmpeg type. The resampler does all the adapting - the
// file's rate and channel count are whatever the encoder chose, and the track feeds the pair AudioOut
// reported at open (48000 x 2 here, which is also what the mixer hands back).
//
// The fixture that first exercised this path is mono on purpose (build/fixtures/ambient6.ogg: pink
// noise plus a 174 Hz hum). A stereo-only path would have passed that and failed every real
// single-channel bed, so the up-mix is part of the test rather than an accident of it.
#include "Engine/Audio/AudioTrack.hpp"
#include "Engine/Audio/AudioOut.hpp"
#include "Engine/Core/Log.hpp"

#include <windows.h>

#pragma warning(push, 0)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#pragma warning(pop)

#include <vector>

namespace sw {
namespace {

std::string U(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

AudioTrack::~AudioTrack() { Stop(); }

bool AudioTrack::Start(AudioOut& out, const std::wstring& file, bool loop, std::string& note) {
    Stop();
    if (!out.ready()) { note = "no audio backend open"; return false; }
    if (file.empty()) { note = "no file"; return false; }
    out_ = &out;
    file_ = file;
    loop_ = loop;
    stop_ = 0;
    written_ = 0;
    loops_ = 0;
    state_ = 0;
    { std::lock_guard lk(errMtx_); err_.clear(); }
    thread_ = std::thread([this] { Run(); });
    note = U(file);
    return true;
}

void AudioTrack::Stop() {
    stop_ = 1;
    if (thread_.joinable()) thread_.join();
}

// The track thread's way of telling the engine it is not going to produce anything. The log already
// gets the full path; this is the short reason the status block answers with, so "why is it silent"
// does not need the log at all.
void AudioTrack::Fail(const std::string& why) {
    {
        std::lock_guard lk(errMtx_);
        err_ = why;
    }
    state_.store(2);
}

std::string AudioTrack::error() const {
    std::lock_guard lk(errMtx_);
    return err_;
}

void AudioTrack::Run() {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    const std::string path = U(file_);

    auto cleanup = [&]() {
        if (frame) av_frame_free(&frame);
        if (swr) swr_free(&swr);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    };

    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        Warn("audio", "track: cannot open {}", path);
        Fail("cannot open the file");
        return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { Fail("cannot read the header"); cleanup(); return; }
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) {
        Warn("audio", "track: {} carries no audio stream", path);
        Fail("no audio stream in it");
        cleanup();
        return;
    }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec || !(dec = avcodec_alloc_context3(codec))) {
        Fail("no decoder for this audio codec");
        cleanup();
        return;
    }
    if (avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar) < 0
        || avcodec_open2(dec, codec, nullptr) < 0) {
        Warn("audio", "track: no decoder for {} ({})", path, codec->name ? codec->name : "?");
        Fail(std::string("cannot open the decoder (") + (codec->name ? codec->name : "?") + ")");
        cleanup();
        return;
    }

    const unsigned outCh = out_->channels() ? out_->channels() : 2u;
    const unsigned outRate = out_->rate() ? out_->rate() : 48000u;
    // Output: float32 interleaved at the device's own rate and channel count. Asking for a default
    // layout at that count is what lets swresample do the mono->stereo work instead of us guessing.
    AVChannelLayout outLay{};
    av_channel_layout_default(&outLay, (int)outCh);
    if (swr_alloc_set_opts2(&swr, &outLay, AV_SAMPLE_FMT_FLT, (int)outRate,
                            &dec->ch_layout, (AVSampleFormat)dec->sample_fmt,
                            dec->sample_rate, 0, nullptr) < 0 || !swr
        || swr_init(swr) < 0) {
        av_channel_layout_uninit(&outLay);
        if (swr) swr_free(&swr);
        Fail("the resampler refused this format");
        cleanup();
        return;
    }
    av_channel_layout_uninit(&outLay);

    frame = av_frame_alloc();
    // One decoded frame plus the headroom swresample wants at a block edge. `frame_size` is 0 for
    // codecs that do not fix it (PCM, mp3), so the floor is what actually sizes the buffer there.
    const int headroom = 256;
    int cap = dec->frame_size > 0 ? dec->frame_size : 4096;
    std::vector<float> sink((size_t)(cap + headroom) * outCh);

    Info("audio", "track: {} - {} {} Hz {} ch -> {} Hz {} ch", path,
         codec->name ? codec->name : "?", dec->sample_rate, dec->ch_layout.nb_channels,
         outRate, outCh);

    AVPacket* pkt = av_packet_alloc();
    state_ = 1;                       // from here on, "running" means it is really producing samples
    while (!stop_.load()) {
        const int rr = av_read_frame(fmt, pkt);
        if (rr < 0) {
            if (!loop_) break;
            if (avformat_seek_file(fmt, -1, INT64_MIN, 0, INT64_MAX, 0) < 0) {
                Fail("cannot rewind it for the next loop");
                break;
            }
            avcodec_flush_buffers(dec);
            loops_.fetch_add(1);
            continue;
        }
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        const int spr = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (spr < 0 && spr != AVERROR(EAGAIN)) continue;
        while (!stop_.load() && avcodec_receive_frame(dec, frame) == 0) {
            const int inN = frame->nb_samples;
            int want = swr_get_out_samples(swr, inN);
            if (want <= 0) want = inN + headroom;
            if ((size_t)want * outCh > sink.size()) sink.resize((size_t)want * outCh);
            // swresample wants an array of plane pointers. The output is packed float32, so one
            // plane; the input is `extended_data`, which is where a planar source keeps its planes.
            uint8_t* planes[1] = { reinterpret_cast<uint8_t*>(sink.data()) };
            const int got = swr_convert(swr, planes, want,
                                        (const uint8_t**)frame->extended_data, inN);
            av_frame_unref(frame);
            if (got <= 0) continue;

            // Backpressure is the pacing: the backend takes only what its ring can hold, and waiting
            // here is what keeps a stalled or unplugged device from becoming unbounded memory. The
            // decoder thread has no other clock.
            unsigned off = 0;
            while (off < (unsigned)got && !stop_.load()) {
                const int took = out_->write(sink.data() + (size_t)off * outCh, (unsigned)got - off);
                if (took <= 0) { Sleep(2); continue; }
                off += (unsigned)took;
                written_ += (unsigned long long)took;
            }
        }
    }
    av_packet_free(&pkt);
    cleanup();
    state_ = 2;                       // a track that reached its end is not a track that is playing
    Info("audio", "track: stopped, {} frames written, {} loops",
         written_.load(), loops_.load());
}

} // namespace sw
