// tools/mkdecode.cpp - can we decode H.264 at all on this machine without the source reader's help?
//
// Measured background: MF's source reader hands back compressed samples and refuses every
// SetCurrentMediaType (E_INVALIDARG) in all attribute/type combinations tried here, and MFTEnumEx
// shows NVIDIA registers no hardware H.264 decoder with MF - only the Microsoft software decoder is
// discoverable. So this tool drives that decoder by hand: the reader for demux, IMFTransform for
// decode, and the first decoded frames get written out as PNG after NV12 -> RGB on the CPU.
//
// The CPU conversion is deliberately done twice, BT.709 and BT.601, and both are printed for a pixel
// inside the fixture's pure-red bar: whichever lands nearer (255,0,0) is the matrix the encoder used.
// That answers a colour question the GPU pipeline would otherwise guess at.
//
// build: tools/mkdecode-build.bat
// usage: mkdecode <file> [framesToSave]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void Die(const char* what, long hr) {
    std::printf("FAIL %s hr=0x%08X\n", what, (unsigned)hr);
    std::fflush(stdout);
    exit(1);
}

static std::string Fourcc(GUID g) {
    char b[5] = { char(g.Data1 & 0xFF), char((g.Data1 >> 8) & 0xFF), char((g.Data1 >> 16) & 0xFF),
                  char((g.Data1 >> 24) & 0xFF), 0 };
    return b;
}

static std::string Friendly(IMFActivate* a) {
    WCHAR* s = nullptr;
    UINT32 chars = 0;
    if (FAILED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &chars)) || !s) return "(unnamed)";
    char utf8[192] = {};
    WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, sizeof(utf8), nullptr, nullptr);
    CoTaskMemFree(s);
    return utf8;
}

static int Clamp255(long v) { return v < 0 ? 0 : (v > 255 ? 255 : int(v)); }

// Limited-range (studio swing) YUV -> RGB. Coefficients x256, matching what the fixture expects.
static void YuvToRgb(int y, int u, int v, int* rgb, bool bt709) {
    const long c = y - 16, d = u - 128, e = v - 128;
    if (bt709) {
        rgb[0] = Clamp255((298 * c + 459 * e + 128) >> 8);
        rgb[1] = Clamp255((298 * c - 55 * d - 136 * e + 128) >> 8);
        rgb[2] = Clamp255((298 * c + 541 * d + 128) >> 8);
    } else {
        rgb[0] = Clamp255((298 * c + 516 * e + 128) >> 8);
        rgb[1] = Clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
        rgb[2] = Clamp255((298 * c + 516 * d + 128) >> 8);
    }
}

static void SavePng(IWICImagingFactory* wic, const std::wstring& path, int w, int h,
                    std::vector<BYTE>& bgra) {
    if (!wic) return;
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
    frame->SetSize(UINT(w), UINT(h));
    if (SUCCEEDED(frame->WritePixels(UINT(h), UINT(w) * 4, UINT(bgra.size()), bgra.data()))) {
        frame->Commit();
        enc->Commit();
        char utf8[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        std::printf("  wrote %s (%dx%d)\n", utf8, w, h);
    }
}

// NV12 (Y plane then interleaved UV, one row per yStride) -> BGRA -> PNG.
static void SaveFrameFromNv12(IWICImagingFactory* wic, const std::wstring& path, UINT32 w, UINT32 h,
                              long long yStride, const BYTE* y) {
    std::vector<BYTE> bgra(size_t(w) * size_t(h) * 4);
    const long long plane = yStride * h;
    for (UINT32 row = 0; row < h; ++row) {
        for (UINT32 col = 0; col < w; ++col) {
            const int u2 = y[plane + size_t(row / 2) * yStride + ((col >> 1) << 1)];
            const int v2 = y[plane + size_t(row / 2) * yStride + ((col >> 1) << 1) + 1];
            int rgb[3];
            YuvToRgb(y[size_t(row) * yStride + col], u2, v2, rgb, true);
            BYTE* o = &bgra[(size_t(row) * w + col) * 4];
            o[0] = BYTE(rgb[2]); o[1] = BYTE(rgb[1]); o[2] = BYTE(rgb[0]); o[3] = 255;
        }
    }
    SavePng(wic, path, int(w), int(h), bgra);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: mkdecode <file> [framesToSave]\n"); return 2; }
    const int saveFrames = argc > 2 ? std::atoi(argv[2]) : 2;
    std::wstring wpath;
    for (const char* c = argv[1]; *c; ++c) wpath.push_back(wchar_t(unsigned char(*c)));

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) Die("CoInitializeEx", E_FAIL);
    if (FAILED(MFStartup(MF_VERSION))) Die("MFStartup", E_FAIL);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    // --- demux: the reader does this part fine -----------------------------------------------
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader)))
        Die("MFCreateSourceReaderFromURL", E_FAIL);
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)))
        Die("GetNativeMediaType", E_FAIL);
    GUID nsub{};
    native->GetGUID(MF_MT_SUBTYPE, &nsub);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 seqChars = 0;
    BYTE* seq = nullptr;
    if (SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &seqChars)) && seqChars) {
        seq = new BYTE[seqChars];
        if (FAILED(native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seq, seqChars, &seqChars))) seqChars = 0;
    }
    std::printf("%s\n  native='%s' %ux%u  MF_MT_MPEG_SEQUENCE_HEADER=%u bytes\n", argv[1],
                Fourcc(nsub).c_str(), w, h, seqChars);
    std::fflush(stdout);

    // --- find the software decoder MF can enumerate -------------------------------------------
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_NV12 };
    IMFActivate** acts = nullptr;
    UINT32 nAct = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                             MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &nAct)) || nAct == 0)
        Die("no H264->NV12 decoder enumerated", E_FAIL);
    std::printf("  decoder: [%u] %s\n", nAct, Friendly(acts[0]).c_str());

    ComPtr<IMFTransform> xf;
    if (FAILED(acts[0]->ActivateObject(IID_PPV_ARGS(&xf)))) Die("ActivateObject", E_FAIL);
    for (UINT32 i = 0; i < nAct; ++i) acts[i]->Release();
    CoTaskMemFree(acts);

    // --- configure it -------------------------------------------------------------------------
    if (FAILED(xf->SetInputType(0, native.Get(), 0))) {
        // Some decoders want a type without the source handler's extras; try a clean copy.
        ComPtr<IMFMediaType> clean;
        MFCreateMediaType(&clean);
        native->CopyAllItems(clean.Get());
        const long hr2 = xf->SetInputType(0, clean.Get(), 0);
        std::printf("  SetInputType(native) refused, clean copy hr=0x%08X\n", (unsigned)hr2);
        if (FAILED(hr2)) Die("SetInputType", hr2);
    } else {
        std::printf("  SetInputType(native) ok\n");
    }

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(INT32(w)));
    outType->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
    const long hot = xf->SetOutputType(0, outType.Get(), 0);
    std::printf("  SetOutputType(NV12) hr=0x%08X\n", (unsigned)hot);
    if (FAILED(hot)) Die("SetOutputType", hot);

    MFT_INPUT_STREAM_INFO isi{};
    MFT_OUTPUT_STREAM_INFO osi{};
    xf->GetInputStreamInfo(0, &isi);
    xf->GetOutputStreamInfo(0, &osi);
    std::printf("  in cbSize=%u flags=0x%X | out cbSize=%u flags=0x%X\n", UINT(isi.cbSize), isi.dwFlags,
                UINT(osi.cbSize), osi.dwFlags);
    // WRITE_TO_OUTPUT_STREAM means the MFT owns the output buffer; most decoders do not set it, but
    // if it is set we would have to supply nothing and reading our own buffer would be wrong.
    if (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
        std::printf("  (decoder provides samples - we only pass through the returned IMFSample)\n");

    xf->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    xf->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // --- pump ---------------------------------------------------------------------------------
    long long yStride = w;   // updated if the decoder renegotiates the surface size
    int saved = 0, decoded = 0, fed = 0;
    for (int i = 0; i < 4000 && saved < saveFrames; ++i) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0, dur = 0;
        IMFSample* raw = nullptr;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts,
                                      &raw)))
            break;
        ComPtr<IMFSample> comp;
        comp.Attach(raw);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { std::printf("  end of stream after %d fed\n", fed); break; }
        if (!comp) continue;

        ComPtr<IMFMediaBuffer> srcBuf;
        comp->GetBufferByIndex(0, &srcBuf);
        BYTE* src = nullptr;
        DWORD srcLen = 0;
        srcBuf->Lock(&src, nullptr, &srcLen);
        ComPtr<IMFMediaBuffer> inBuf;
        // isi.cbSize is the MFT's alignment allowance, not a maximum: the first sample here is 8434
        // bytes and 4096 would truncate every NAL unit, which is what kept this probe at zero frames.
        const UINT32 need = srcLen + UINT32(isi.cbSize);
        if (FAILED(MFCreateAlignedMemoryBuffer(need, 32, &inBuf))) Die("MFCreateAlignedMemoryBuffer", E_FAIL);
        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        inBuf->Lock(&dst, &maxLen, nullptr);
        memcpy(dst, src, srcLen);
        inBuf->Unlock();
        inBuf->SetCurrentLength(srcLen);
        srcBuf->Unlock();

        ComPtr<IMFSample> inSample;
        MFCreateSample(&inSample);
        inSample->AddBuffer(inBuf.Get());
        inSample->SetSampleTime(ts);
        inSample->SetSampleDuration(dur ? dur : 333333);
        long hr = S_OK;
        for (int tryIn = 0; tryIn < 3; ++tryIn) {
            hr = xf->ProcessInput(0, inSample.Get(), 0);
            if (hr != MF_E_NOTACCEPTING) break;
            // NOTACCEPTING means output is ready: pull it, then retry the *same* sample. Dropping it
            // here loses reference frames and the decoder never emits anything.
            DWORD st2 = 0;
            MFT_OUTPUT_DATA_BUFFER d2{};
            ComPtr<IMFMediaBuffer> b2;
            ComPtr<IMFSample> s2;
            if (SUCCEEDED(MFCreateAlignedMemoryBuffer(osi.cbSize, 32, &b2)) && SUCCEEDED(MFCreateSample(&s2))) {
                s2->AddBuffer(b2.Get());
                d2.pSample = s2.Get();
                const long h2 = xf->ProcessOutput(0, 1, &d2, &st2);
                std::printf("  NOTACCEPTING -> drain hr=0x%08X outFlags=0x%X", (unsigned)h2, osi.dwFlags);
                if (h2 == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    std::printf(" (needs more input)\n");
                } else if (SUCCEEDED(h2) && d2.pSample) {
                    // Read it: this is the frame the main path is failing to hand back.
                    ComPtr<IMFMediaBuffer> rb;
                    d2.pSample->GetBufferByIndex(0, &rb);
                    BYTE* py = nullptr;
                    DWORD mm = 0, ll = 0;
                    rb->Lock(&py, &mm, &ll);
                    long sum = 0;
                    const long long plane = yStride * h;
                    for (long long p = 0; p + 0 < (long long)ll && p < plane; p += 997) sum += py[p];
                    // Sample the fixture's known bars at two symmetric rows: if the plane is stored
                    // bottom-up, the "top" row reads as the picture's bottom band (flat grey 128) and
                    // the mirrored row reads as the bar. Both are printed so the row order is measured
                    // rather than assumed. x = 11w/16 is inside bar 5, the pure-red one.
                    const int px = int(w * 11 / 16), prow = int(h * 10 / 100);
                    const long long plane2 = plane;
                    auto At = [&](int row, int* out) {
                        const int yv = py[size_t(row) * yStride + px];
                        const int uu = py[plane2 + size_t(row / 2) * yStride + (px & ~1)];
                        const int vv = py[plane2 + size_t(row / 2) * yStride + (px & ~1) + 1];
                        int r7[3], r6[3];
                        YuvToRgb(yv, uu, vv, r7, true);
                        YuvToRgb(yv, uu, vv, r6, false);
                        out[0] = yv; out[1] = uu; out[2] = vv;
                        out[3] = r7[0]; out[4] = r7[1]; out[5] = r7[2];
                        out[6] = r6[0]; out[7] = r6[1]; out[8] = r6[2];
                    };
                    int a[9], b[9];
                    At(prow, a);
                    At(int(h) - 1 - prow, b);
                    std::printf("  DECODED len=%u meanY=%.1f\n    row %d: Y=%d U=%d V=%d 709=(%d,%d,%d) 601=(%d,%d,%d)\n"
                                "    row %d: Y=%d U=%d V=%d 709=(%d,%d,%d) 601=(%d,%d,%d)\n",
                                ll, double(sum) / double(((plane < (long long)ll ? plane : (long long)ll) + 996) / 997),
                                prow, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                                int(h) - 1 - prow, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                    if (saved < saveFrames) {
                        std::wstring outp = L"build/decoded-";
                        outp += std::to_wstring(saved + 1);
                        outp += L".png";
                        SaveFrameFromNv12(wic.Get(), outp, w, h, yStride, py);
                        ++saved;
                    }
                    rb->Unlock();
                } else {
                    std::printf("\n");
                }
            }
        }
        if (FAILED(hr)) {
            std::printf("  ProcessInput hr=0x%08X at %d\n", (unsigned)hr, i);
            break;
        }
        ++fed;

        for (int attempt = 0; attempt < 4; ++attempt) {
            DWORD status = 0;
            MFT_OUTPUT_DATA_BUFFER odb{};
            odb.dwStreamID = 0;
            ComPtr<IMFMediaBuffer> ob;
            IMFSample* passed = nullptr;
            if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if (FAILED(MFCreateAlignedMemoryBuffer(osi.cbSize, 32, &ob))) Die("out buffer", E_FAIL);
                ComPtr<IMFSample> os;
                MFCreateSample(&os);
                os->AddBuffer(ob.Get());
                odb.pSample = os.Get();
                passed = os.Get();   // the MFT fills our sample; releasing it here would double-free
            }
            hr = xf->ProcessOutput(0, 1, &odb, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // The H.264 MFT signals this once it has parsed the SPS: the coded surface is not the
                // size the container claimed (1080 rows become 1088), so the client has to re-read the
                // type it is being offered, re-assert it and re-query the buffer size before continuing.
                std::printf("  STREAM_CHANGE at sample %d - renegotiating\n", i);
                // A stream change invalidates the decoder's internal queue: it has to be flushed, the
                // output type cleared and re-set, and streaming restarted - swapping the type alone
                // leaves ProcessOutput returning E_FAIL forever.
                xf->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                xf->SetOutputType(0, nullptr, 0);
                xf->GetOutputStreamInfo(0, &osi);
                {
                    // This MFT reports cbSize as w*h*2 for NV12, measured: 4147200 = 1920*1080*2 before
                    // the change and 4177920 = 1920*1088*2 after it. Deriving h from its own number is
                    // the only way to learn the coded height - the container says 1080 and lies.
                    const long long hh = long long(osi.cbSize) / (2 * long long(w));
                    if (hh >= long long(h) && hh <= long long(h) + 64) {
                        std::printf("    coded rows %lld (container said %u) from cbSize=%u\n", hh, h,
                                    UINT(osi.cbSize));
                        h = UINT32(hh);
                    }
                }
                outType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(w));
                outType->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
                MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, w, h);
                const long rr = xf->SetOutputType(0, outType.Get(), 0);
                xf->GetOutputStreamInfo(0, &osi);
                yStride = w;
                std::printf("    re-SetOutputType hr=0x%08X, out cbSize=%u, now %ux%u\n", (unsigned)rr,
                            UINT(osi.cbSize), w, h);
                if (odb.pSample && odb.pSample != passed) odb.pSample->Release();
                continue;
            }
            if (FAILED(hr)) {
                static int hard = 0;
                ++hard;
                if (hard <= 3)
                    std::printf("  ProcessOutput hr=0x%08X (hard failure #%d)\n", (unsigned)hr, hard);
                if (hard > 40) { std::printf("  giving up after %d hard failures\n", hard); i = 4000; }
                if (odb.pSample && odb.pSample != passed) odb.pSample->Release();
                break;
            }
            if (!odb.pSample) break;
            ComPtr<IMFMediaBuffer> resBuf;
            odb.pSample->GetBufferByIndex(0, &resBuf);
            BYTE* y = nullptr;
            DWORD m = 0, l = 0;
            resBuf->Lock(&y, &m, &l);
            const long long plane = yStride * h;
            if (l < UINT32(plane)) { std::printf("  decoded buffer too small: %u < %lld\n", l, plane); }
            else {
                ++decoded;
                // brightness + the red-bar probe (bar 5 of 8 sits at 5/8..6/8 of the width)
                long sum = 0;
                for (long long p = 0; p < plane; p += 997) sum += y[p];
                const double meanY = double(sum) / double((plane + 996) / 997);
                const int px = int(w * 55 / 100), py = int(h * 10 / 100);
                const int yy = y[size_t(py) * yStride + px];
                const int uu = y[plane + size_t(py / 2) * yStride + (px & ~1)];
                const int vv = y[plane + size_t(py / 2) * yStride + (px & ~1) + 1];
                int r7[3], r6[3];
                YuvToRgb(yy, uu, vv, r7, true);
                YuvToRgb(yy, uu, vv, r6, false);
                std::printf("  frame %d: meanY=%.1f  red-bar pixel(%d,%d) Y=%d U=%d V=%d -> 709=(%d,%d,%d) 601=(%d,%d,%d)\n",
                            decoded, meanY, px, py, yy, uu, vv, r7[0], r7[1], r7[2], r6[0], r6[1], r6[2]);
                if (saved < saveFrames) {
                    std::vector<BYTE> bgra(size_t(w) * size_t(h) * 4);
                    for (int row = 0; row < int(h); ++row) {
                        for (int col = 0; col < int(w); ++col) {
                            const int u2 = y[plane + size_t(row / 2) * yStride + ((col >> 1) << 1)];
                            const int v2 = y[plane + size_t(row / 2) * yStride + ((col >> 1) << 1) + 1];
                            int rgb[3];
                            YuvToRgb(y[size_t(row) * yStride + col], u2, v2, rgb, true);
                            BYTE* o = &bgra[(size_t(row) * w + col) * 4];
                            o[0] = BYTE(rgb[2]); o[1] = BYTE(rgb[1]); o[2] = BYTE(rgb[0]); o[3] = 255;
                        }
                    }
                    std::wstring outp = L"build/decoded-";
                    outp += std::to_wstring(saved + 1);
                    outp += L".png";
                    SavePng(wic.Get(), outp, int(w), int(h), bgra);
                    ++saved;
                }
            }
            resBuf->Unlock();
            if (odb.pSample && odb.pSample != passed) odb.pSample->Release();
        }
    }

    xf->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    xf->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    std::printf("RESULT fed=%d decoded=%d saved=%d\n", fed, decoded, saved);
    std::fflush(stdout);
    delete[] seq;
    MFShutdown();
    return decoded > 0 ? 0 : 1;
}
