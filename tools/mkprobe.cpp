// tools/mkprobe.cpp - what does the source reader actually hand us?
//
// The wallpaper video path is blocked on one measured fact: SetCurrentMediaType answers
// E_INVALIDARG for both RGB32 and NV12 on this machine, in DX and system-memory modes alike. Before
// writing any colour conversion we have to know what the reader produces when we ask for nothing -
// the decoded NV12 it negotiated on its own, or the compressed stream.
//
// Prints, for each mode: the current subtype, SAMPLE_SIZE and DEFAULT_STRIDE, then the first few
// samples' byte length and whether their buffer can be opened as IMFDXGIBuffer (a D3D texture) or
// IMF2DBuffer2 (a pitched CPU buffer). It never reads pixel contents, so it cannot crash the way the
// frame-peek tool did when it treated a compressed sample as a Y plane.
//
// build: tools/mkprobe-build.bat    usage: mkprobe <file> [samples]

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void Die(const char* what, long hr) {
    std::printf("FAIL %s hr=0x%08X\n", what, (unsigned)hr);
    std::fflush(stdout);
    exit(1);
}

static std::string Fourcc(const GUID& g) {
    char b[5] = { char(g.Data1 & 0xFF), char((g.Data1 >> 8) & 0xFF), char((g.Data1 >> 16) & 0xFF),
                  char((g.Data1 >> 24) & 0xFF), 0 };
    return std::string(b);
}

static void Describe(IMFSourceReader* reader, const char* label) {
    ComPtr<IMFMediaType> mt;
    const long hr = reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &mt);
    if (FAILED(hr)) { std::printf("%s: GetCurrentMediaType hr=0x%08X\n", label, (unsigned)hr); return; }
    GUID sub{};
    mt->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT32 w = 0, h = 0, sample = 0, stride = 0;
    MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
    mt->GetUINT32(MF_MT_SAMPLE_SIZE, &sample);
    mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    std::printf("  [%s] subtype='%s' %ux%u SAMPLE_SIZE=%u DEFAULT_STRIDE=%d\n", label,
                Fourcc(sub).c_str(), w, h, sample, int(stride));
    std::fflush(stdout);
}

static void Pump(IMFSourceReader* reader, int n) {
    for (int i = 0; i < n; ++i) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* raw = nullptr;
        const long hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags,
                                           &ts, &raw);
        ComPtr<IMFSample> s;
        s.Attach(raw);
        std::printf("  sample %d: hr=0x%08X flags=0x%X ts=%.3fs", i, (unsigned)hr, flags, double(ts) / 1e7);
        if (FAILED(hr) || !s) { std::printf("\n"); std::fflush(stdout); continue; }
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->GetBufferByIndex(0, &buf))) { std::printf(" (no buffer)\n"); std::fflush(stdout); continue; }
        DWORD maxLen = 0, curLen = 0;
        buf->GetMaxLength(&maxLen);
        buf->GetCurrentLength(&curLen);
        ComPtr<IMFDXGIBuffer> dxgi;
        ComPtr<IMF2DBuffer2> two;
        const bool isDxgi = SUCCEEDED(buf.As(&dxgi));
        const bool is2d = SUCCEEDED(buf.As(&two));
        std::printf(" len=%u/%u IMFDXGIBuffer=%s IMF2DBuffer2=%s\n", curLen, maxLen,
                    isDxgi ? "yes" : "no", is2d ? "yes" : "no");
        if (is2d) {
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            BYTE* start = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(two->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len))) {
                std::printf("    2D: pitch=%d buffer_len=%u\n", pitch, len);
                two->Unlock2D();
            }
        }
        std::fflush(stdout);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
}


static void TryClsid(const char* name, REFCLSID id) {
    ComPtr<IUnknown> o;
    const long hr = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&o));
    std::printf("  %-22s CoCreateInstance hr=0x%08X  %s\n", name, (unsigned)hr,
                SUCCEEDED(hr) ? "可用" : "不可用");
    std::fflush(stdout);
}

static void ProbeMfts() {
    std::printf("MFT availability (is there any decoder to insert at all):\n");
    const CLSID h264dec = { 0x62CE7E72, 0x4C71, 0x4D20, { 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D } };
    const CLSID videoprocessor = { 0x88753B26, 0x5B24, 0x49BD, { 0xB2, 0xE7, 0x0C, 0x44, 0x5C, 0x78, 0xC9, 0x82 } };
    const CLSID h264enc = { 0xCBB68E48, 0xBB4A, 0x4703, { 0xA105, 0x4F, 0x11, 0x97, 0x62, 0x9, 0xE4 } };
    TryClsid("H.264 decoder MFT", h264dec);
    TryClsid("Video Processor MFT", videoprocessor);
    TryClsid("H.264 encoder MFT", h264enc);
}

// The reader inserts a decoder by *enumerating* installed transforms, which is a different thing
// from being able to CoCreateInstance one. MFTEnumEx is the documented enumerator, so this answers
// "can MF even find the H.264 decoder" separately from "does the codec DLL exist".
static void EnumDecoders() {
    std::printf("MFTEnumEx - what MF can discover:\n");
    struct Case { const char* label; UINT32 flags; } cases[] = {
        { "SYNC|ASYNC|HARDWARE|LOCAL|SORTANDFILTER",
          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
          MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER },
        { "SYNC|ASYNC (software only)", MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT },
        { "HARDWARE", MFT_ENUM_FLAG_HARDWARE },
        { "LOCAL", MFT_ENUM_FLAG_LOCALMFT },
        // The docs require HARDWARE to be combined with SORTANDFILTER, so the HARDWARE-only case
        // above proves nothing about whether a GPU decoder is registered. This is the real test.
        { "HARDWARE|SORTANDFILTER", MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER },
        { "ALL (no type filter)",
          MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
          MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER },
    };
    for (auto& c : cases) {
        MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_H264 };
        MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_NV12 };
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        const long hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, c.flags, &in, &out, &acts, &n);
        std::printf("  H264->NV12  %-38s hr=0x%08X count=%u\n", c.label, (unsigned)hr, n);
        if (acts) {
            for (UINT32 i = 0; i < n; ++i) {
                WCHAR* friendly = nullptr;
                UINT32 chars = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &chars)) && friendly) {
                    char utf8[192] = {};
                    WideCharToMultiByte(CP_UTF8, 0, friendly, -1, utf8, sizeof(utf8), nullptr, nullptr);
                    ComPtr<IMFTransform> xf;
                    const long ah = acts[i]->ActivateObject(IID_PPV_ARGS(&xf));
                    std::printf("      [%u] %s  ActivateObject hr=0x%08X\n", i, utf8, (unsigned)ah);
                    CoTaskMemFree(friendly);
                }
            }
            CoTaskMemFree(acts);
        }
    }

    // Is this machine missing hardware MFTs *generally* - which is normal for a vendor that exposes
    // decode through CUVID/DXVA2 instead of registering an MF transform - or is only the
    // H264->NV12 query empty? Enumerating without a type filter tells the two apart.
    std::printf("hardware MFTs, no type filter:\n");
    struct Cat { const char* name; REFGUID guid; } cats[] = {
        { "VIDEO_DECODER", MFT_CATEGORY_VIDEO_DECODER },
        { "VIDEO_ENCODER", MFT_CATEGORY_VIDEO_ENCODER },
        { "VIDEO_PROCESSOR", MFT_CATEGORY_VIDEO_PROCESSOR },
    };
    for (auto& c : cats) {
        IMFActivate** acts = nullptr;
        UINT32 n = 0;
        const long hr = MFTEnumEx(c.guid, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                                  nullptr, &acts, &n);
        std::printf("  %-16s hr=0x%08X count=%u\n", c.name, (unsigned)hr, n);
        for (UINT32 i = 0; i < n && i < 6; ++i) {
            WCHAR* friendly = nullptr;
            UINT32 chars = 0;
            if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &chars)) &&
                friendly) {
                char utf8[192] = {};
                WideCharToMultiByte(CP_UTF8, 0, friendly, -1, utf8, sizeof(utf8), nullptr, nullptr);
                std::printf("      [%u] %s\n", i, utf8);
                CoTaskMemFree(friendly);
            }
        }
        if (acts) {
            for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
        }
    }
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: mkprobe <file> [samples]\n"); return 2; }
    const int nsamples = argc > 2 ? std::atoi(argv[2]) : 3;
    std::wstring wpath;
    for (const char* c = argv[1]; *c; ++c) wpath.push_back(wchar_t(unsigned char(*c)));

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) Die("CoInitializeEx", E_FAIL);
    if (FAILED(MFStartup(MF_VERSION))) Die("MFStartup", E_FAIL);

    std::printf("%s\n", argv[1]);
    ProbeMfts();
    EnumDecoders();

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &reader)))
        Die("MFCreateSourceReaderFromURL", E_FAIL);

    // Control group: the same file with NO attributes at all. The source reader is documented to
    // insert a decoder on its own; if this mode reports NV12 while the flagged mode reports H264,
    // then the attribute we pass is what suppresses auto-decode.
    {
        ComPtr<IMFSourceReader> plain;
        const long h0 = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &plain);
        std::printf("  [no attributes] create hr=0x%08X\n", (unsigned)h0);
        if (SUCCEEDED(h0)) {
            Describe(plain.Get(), "current, no attributes");
            Pump(plain.Get(), 1);
            ComPtr<IMFMediaType> w2;
            MFCreateMediaType(&w2);
            w2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            w2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            w2->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            DWORD res2 = 0;
            const long hn = plain->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &res2,
                                                       w2.Get());
            std::printf("  [no attributes] request NV12 -> hr=0x%08X pdwResult=0x%X\n", (unsigned)hn,
                        res2);
            if (SUCCEEDED(hn)) {
                Describe(plain.Get(), "current after NV12, no attributes");
                Pump(plain.Get(), 2);
            }
            // Last cheap variable: a type cloned from the native one (so it carries size, rate,
            // aspect, colours) with only the subtype changed. A minimal hand-built type may be what
            // the reader rejects, not the format itself.
            ComPtr<IMFMediaType> clone;
            ComPtr<IMFMediaType> nat2;
            if (SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nat2))) {
                MFCreateMediaType(&clone);
                nat2->CopyAllItems(clone.Get());
                clone->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                clone->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                DWORD res3 = 0;
                const long hc = plain->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                           &res3, clone.Get());
                std::printf("  [no attributes] request NV12 as a clone of native -> hr=0x%08X pdwResult=0x%X\n",
                            (unsigned)hc, res3);
                if (SUCCEEDED(hc)) {
                    Describe(plain.Get(), "current after cloned NV12");
                    Pump(plain.Get(), 2);
                }
            }
        }
    }

    // Third reader: ADVANCED video processing with no DXGI manager at all. The product only ever
    // tried ADVANCED *together with* a D3D manager, so this combination is still unmeasured.
    {
        ComPtr<IMFAttributes> a3;
        MFCreateAttributes(&a3, 1);
        a3->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> adv;
        const long hc = MFCreateSourceReaderFromURL(wpath.c_str(), a3.Get(), &adv);
        std::printf("  [ADVANCED, no DX] create hr=0x%08X\n", (unsigned)hc);
        if (SUCCEEDED(hc)) {
            Describe(adv.Get(), "current, ADVANCED no DX");
            ComPtr<IMFMediaType> w3;
            MFCreateMediaType(&w3);
            w3->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            w3->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            w3->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            DWORD r3 = 0;
            const long hn = adv->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &r3,
                                                     w3.Get());
            std::printf("  [ADVANCED, no DX] request NV12 -> hr=0x%08X pdwResult=0x%X\n", (unsigned)hn,
                        r3);
            if (SUCCEEDED(hn)) {
                Describe(adv.Get(), "current after NV12, ADVANCED no DX");
                Pump(adv.Get(), 2);
            }
        }
    }

    ComPtr<IMFMediaType> nat;
    if (SUCCEEDED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nat))) {
        GUID sub{};
        nat->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(nat.Get(), MF_MT_FRAME_SIZE, &w, &h);
        std::printf("  native='%s' %ux%u\n", Fourcc(sub).c_str(), w, h);
    }

    Describe(reader.Get(), "current, nothing requested");
    Pump(reader.Get(), nsamples);

    // Now ask for NV12 explicitly and report the refusal code with the flag it returns.
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    want->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    DWORD result = 0;
    const long h2 = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &result,
                                                want.Get());
    std::printf("  request NV12 -> hr=0x%08X pdwResult=0x%X\n", (unsigned)h2, result);
    if (SUCCEEDED(h2)) {
        Describe(reader.Get(), "current after NV12");
        Pump(reader.Get(), nsamples);
    }
    std::fflush(stdout);
    MFShutdown();
    return 0;
}
