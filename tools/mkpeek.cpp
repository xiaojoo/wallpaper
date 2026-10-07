// tools/mkpeek.cpp - pull frames out of a video file with Media Foundation and print per-frame stats.
//
// Two jobs. (1) It is the only way on this machine to look inside a screen recording without ffmpeg:
// it reports mean/min/max brightness and the change against the previous frame, so a flicker or a
// black frame shows up as a number before anyone has to look at pixels. (2) It is the probe for the
// question the wallpaper video path is stuck on: this build asks the source reader for RGB32 in
// system memory with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING (the flag that actually enables format
// conversion) and no DXGI device manager - the exact opposite of what VideoPlayer::Open does.
//
// build: tools/mkpeek-build.bat    usage: mkpeek <in.mp4> [maxFrames] [outPrefix]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void Fail(const char* what, long hr) {
    std::printf("FAIL %s hr=0x%08X (%d)\n", what, (unsigned)hr, (int)hr);
    exit(1);
}

// Saves one BGRA frame, cropped from the centre, as a PNG. Small on purpose: the point of a saved
// frame is to be looked at, and a 4K PNG of a dark wallpaper tells you nothing anyway.
static void SavePng(IWICImagingFactory* wic, const std::wstring& path, const BYTE* bgra, int fullW, int fullH,
                    int stride, int cw, int ch) {
    if (!wic) return;
    int x0 = (fullW - cw) / 2, y0 = (fullH - ch) / 2;
    std::vector<BYTE> crop(size_t(cw) * size_t(ch) * 4);
    for (int y = 0; y < ch; ++y)
        memcpy(crop.data() + size_t(y) * cw * 4, bgra + size_t(y0 + y) * stride + size_t(x0) * 4,
               size_t(cw) * 4);
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pf);
    frame->SetSize(UINT(cw), UINT(ch));
    if (SUCCEEDED(frame->WritePixels(UINT(ch), pf == GUID_WICPixelFormat32bppBGRA ? UINT(cw * 4) : UINT(cw * 3),
                                    UINT(crop.size()), crop.data()))) {
        frame->Commit();
        enc->Commit();
        std::printf("  saved %s (%dx%d centre crop)\n", path.c_str(), cw, ch);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: mkpeek <in.mp4> [maxFrames] [outPrefix]\n");
        return 2;
    }
    const int maxFrames = argc > 2 ? std::atoi(argv[2]) : 400;
    std::wstring wpath;
    for (const char* c = argv[1]; *c; ++c) wpath.push_back(wchar_t(unsigned char(*c)));

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) Fail("CoInitializeEx", E_FAIL);
    if (FAILED(MFStartup(MF_VERSION))) Fail("MFStartup", E_FAIL);

    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    // The reader is created with no DXGI manager at all: system memory is the point.
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    long hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &reader);
    if (FAILED(hr)) Fail("MFCreateSourceReaderFromURL", hr);

    ComPtr<IMFMediaType> native;
    hr = reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
    if (FAILED(hr)) Fail("GetNativeMediaType", hr);
    GUID nsub{};
    native->GetGUID(MF_MT_SUBTYPE, &nsub);
    UINT32 nw = 0, nh = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
    UINT32 un = 0, ud = 0;
    MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &un, &ud);
    char fourcc[5] = { char(nsub.Data1 & 0xFF), char((nsub.Data1 >> 8) & 0xFF), char((nsub.Data1 >> 16) & 0xFF), char((nsub.Data1 >> 24) & 0xFF), 0 };
    std::printf("native: '%s' %ux%u %u/%u fps\n", fourcc, nw, nh, un, ud);

    const bool nativeOnly = argc > 4 && std::strcmp(argv[4], "--native") == 0;
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    want->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeRatio(want.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD result = 0;
    hr = nativeOnly ? S_OK
        : reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &result, want.Get());
    std::printf("SetCurrentMediaType(NV12, system memory): hr=0x%08X pdwResult=0x%X\n", (unsigned)hr, result);
    if (FAILED(hr)) Fail("SetCurrentMediaType refused", hr);
    if (nativeOnly) std::printf("  (native mode: SetCurrentMediaType not called)\n");

    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &got);
    GUID gsub{};
    got->GetGUID(MF_MT_SUBTYPE, &gsub);
    UINT32 gw = 0, gh = 0, gstride = 0;
    MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &gw, &gh);
    got->GetUINT32(MF_MT_DEFAULT_STRIDE, &gstride);
    char g4[5] = { char(gsub.Data1 & 0xFF), char((gsub.Data1 >> 8) & 0xFF), char((gsub.Data1 >> 16) & 0xFF), char((gsub.Data1 >> 24) & 0xFF), 0 };
    std::printf("current after negotiate: '%s' %ux%u stride=%u\n", g4, gw, gh, gstride);

    const int cw = std::min(720u, gw), ch = std::min(405u, gh);
    double prevMean = -1.0;
    int n = 0;
    std::printf("  idx      time   mean    min   max   dMean   note\n");
    while (n < maxFrames) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* raw = nullptr;
        hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &raw);
        if (FAILED(hr)) Fail("ReadSample", hr);
        ComPtr<IMFSample> sample;
        sample.Attach(raw);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { std::printf("  end of stream at frame %d\n", n); break; }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) continue;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen))) continue;
        const int rowBytes = int(gw);
        double sum = 0.0;
        int mn = 255, mx = 0;
        for (int y = 0; y < int(gh); y += 4) {
            const BYTE* row = data + size_t(y) * rowBytes;
            for (int x = 0; x < rowBytes; x += 4) {
                const int v = row[x];
                sum += v;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        }
        const double mean = sum / (double(gh / 4) * double(rowBytes / 4));
        const char* note = "";
        if (prevMean >= 0.0 && std::abs(mean - prevMean) > 12.0) note = "<== 亮度跳变";
        std::printf("  %3d  %7.3f  %6.2f  %4d  %4d  %+6.2f  %s\n", n, double(ts) / 1e7, mean, mn, mx,
                    prevMean < 0.0 ? 0.0 : mean - prevMean, note);
        if (false) {
            char base[MAX_PATH];
            wcstombs(base, wpath.c_str(), sizeof(base) - 32);
            std::string outp = std::string(argc > 3 ? argv[3] : "build/peek") + "-" + std::to_string(n) + ".png";
            SavePng(wic.Get(), std::wstring(outp.begin(), outp.end()).c_str(), data, int(gw), int(gh), rowBytes, cw, ch);
        }
        prevMean = mean;
        buf->Unlock();
        ++n;
    }
    MFShutdown();
    return 0;
}
