// tools/mkfixture.cpp - writes a synthetic H.264 mp4 whose pixel values are known exactly.
//
// The video wallpaper tests need a clip that answers three questions no real clip can answer:
//   1. Is the NV12 -> RGB conversion using the matrix the encoder used? -> saturated colour bars.
//   2. Is a frame actually advancing? -> a block at a position predicted from the frame index.
//   3. Did we get the row order right? -> a white strip along the top edge, black along the bottom.
// The same generator at two sizes is also the ruler for "does CPU cost track resolution".
//
// build: see tools/make-fixture.sh
// usage: mkfixture <out.mp4> <width> <height> <seconds> [fps] [bitrate_kbps]
// Paths must be ASCII: this is an ANSI main, and the fixture names are chosen by us.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // windows.h defines min/max macros that break std::max
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// Bar 5 is pure red: decoded through the wrong matrix it comes back several units off in green and
// blue, which is far more than H.264 chroma subsampling moves it. That is what makes it a test.
static const uint32_t kBars[8][3] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {16, 16, 16},
};

[[noreturn]] static void Fail(const char* what, long hr) {
    std::printf("FAIL %s hr=0x%08X (%d)\n", what, (unsigned)hr, (int)hr);
    exit(1);
}

// Everything is a fraction of the frame, so one generator serves 1080p and 4K identically.
static void DrawFrame(std::vector<uint8_t>& px, int w, int h, int frame, int frames) {
    const double progress = frames > 1 ? double(frame) / double(frames - 1) : 0.0;
    const int barH = h / 3;
    const int bw = std::max(8, w * 40 / 1920);
    const int cx = int(bw + progress * double(w - 2 * bw));
    const int blockCy = 2 * barH + barH / 2;
    const int edge = std::max(4, h * 8 / 1080);

    for (int y = 0; y < h; ++y) {
        uint8_t* row = px.data() + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
            uint32_t r = 128, g = 128, b = 128;
            if (y < barH) {
                const int i = std::min(7, x * 8 / w);
                r = kBars[i][0]; g = kBars[i][1]; b = kBars[i][2];
            } else if (y < 2 * barH) {
                const int i = std::min(10, x * 11 / w);
                const int v = 16 + i * (235 - 16) / 10;  // studio-swing luma ramp
                r = g = b = uint32_t(v);
            } else if (std::abs(x - cx) < bw / 2 && std::abs(y - blockCy) < bw / 2) {
                r = g = b = 255;
            }
            if (y < edge) r = g = b = 255;
            else if (y >= h - edge) r = g = b = 0;
            uint8_t* p = row + size_t(x) * 4;
            p[0] = uint8_t(b); p[1] = uint8_t(g); p[2] = uint8_t(r); p[3] = 255;  // BGRA
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: mkfixture <out.mp4> <width> <height> <seconds> [fps] [bitrate_kbps]\n");
        return 2;
    }
    const int w = std::atoi(argv[2]);
    const int h = std::atoi(argv[3]);
    const int seconds = std::atoi(argv[4]);
    const int fps = argc > 5 ? std::atoi(argv[5]) : 30;
    const int kbps = argc > 6 ? std::atoi(argv[6]) : (w * h) / 200;

    std::wstring wpath;
    for (const char* c = argv[1]; *c; ++c) wpath.push_back(wchar_t(unsigned char(*c)));

    // Multithreaded on purpose: the MP4 sink writer posts its completion events to the apartment that
    // created it. On an STA thread with no message pump - which is what a console program is -
    // Finalize() waits for an event that can only be delivered by a pump that never runs, and hangs.
    long hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) Fail("CoInitializeEx", hr);
    hr = MFStartup(MF_VERSION);  // full startup: the sink writer needs the attribute store
    if (FAILED(hr)) Fail("MFStartup", hr);

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    ComPtr<IMFSinkWriter> writer;
    hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.Get(), &writer);
    if (FAILED(hr)) Fail("MFCreateSinkWriterFromURL (bad path, or no H.264 encoder)", hr);

    ComPtr<IMFMediaType> outType, inType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG_START_TIME_CODE, 0);
    outType->SetUINT32(MF_MT_AVG_BITRATE, UINT32(kbps * 1000));
    outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, UINT(w), UINT(h));
    MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, UINT(fps), 1);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(INT32(-4 * w)));  // negative stride: rows top-down
    inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    inType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    inType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, UINT(w), UINT(h));
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, UINT(fps), 1);
    inType->SetUINT32(MF_MT_SAMPLE_SIZE, UINT32(UINT64(w) * UINT64(h) * 4));

    DWORD stream = 0;
    hr = writer->AddStream(outType.Get(), &stream);
    if (FAILED(hr)) Fail("AddStream", hr);
    hr = writer->SetInputMediaType(stream, inType.Get(), nullptr);
    if (FAILED(hr)) Fail("SetInputMediaType (no RGB32 -> H.264 converter)", hr);
    hr = writer->BeginWriting();
    if (FAILED(hr)) Fail("BeginWriting", hr);

    const int frames = seconds * fps;
    const size_t rowBytes = size_t(w) * 4;
    std::vector<uint8_t> px(rowBytes * size_t(h));
    const LONGLONG dur = LONGLONG(10000000) / fps;
    for (int i = 0; i < frames; ++i) {
        DrawFrame(px, w, h, i, frames);
        // A plain memory buffer, not MFCreate2DMediaBuffer: the row order is already declared on the
        // input type through MF_MT_DEFAULT_STRIDE, and the 2D helper rejects the pitch it is given
        // (MF_E_INVALIDMEDIATYPE) on this SDK.
        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateMemoryBuffer(UINT32(rowBytes * size_t(h)), &buf);
        if (FAILED(hr)) Fail("MFCreateMemoryBuffer", hr);
        BYTE* data = nullptr;
        hr = buf->Lock(&data, nullptr, nullptr);
        if (FAILED(hr)) Fail("IMFMediaBuffer::Lock", hr);
        memcpy(data, px.data(), rowBytes * size_t(h));
        buf->Unlock();
        buf->SetCurrentLength(UINT32(rowBytes * size_t(h)));

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buf.Get());
        sample->SetSampleTime(LONGLONG(i) * dur);
        sample->SetSampleDuration(dur);
        hr = writer->WriteSample(stream, sample.Get());
        if (FAILED(hr)) Fail("WriteSample", hr);
        if ((i + 1) % fps == 0) { std::printf("  %d/%d s\n", (i + 1) / fps, seconds); std::fflush(stdout); }
    }
    hr = writer->Flush(stream);
    if (FAILED(hr)) Fail("Flush", hr);
    // Finalize, not just Release: the MP4 handler writes the movie header here, and a file without a
    // moov box is unreadable - MFCreateSourceReaderFromURL answers MF_E_UNSUPPORTED_REPRESENTATION
    // (0xC00D36C4) for it, which looks exactly like "this machine has no decoder".
    hr = writer->Finalize();
    if (FAILED(hr)) Fail("Finalize", hr);
    writer.Reset();
    MFShutdown();
    std::printf("wrote %s %dx%d %d fps %d s @%d kbps\n", argv[1], w, h, fps, seconds, kbps);
    return 0;
}
