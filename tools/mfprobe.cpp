// tools/mfprobe.cpp - ask Media Foundation what this machine can decode, and what it will hand back.
//
//   build: tools/mfprobe-build.bat
//   usage: mfprobe decoders
//   usage: mfprobe negotiate <file> [<file> ...]
//
// `decoders` lists the MFTs registered in the video decoder / transform / effect categories with the
// subtypes each one takes. A source reader that opens a file, reports its native type, and then
// answers E_INVALIDARG to every decoded output type looks exactly like a missing decoder, and this is
// the one call that says whether that is true.
//
// `negotiate` runs several recipes against one file. SetCurrentMediaType's HRESULT is not the answer:
// the reader can accept a type and still never deliver a sample of it, so every variant that is
// accepted is followed by real ReadSample calls and the delivered buffer's size.

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mfobjects.h>
#include <d3d11.h>
#include <d3d11_4.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static std::string Fourcc(GUID g) {
    char b[8] = {char(g.Data1 >> 24), char(g.Data1 >> 16), char(g.Data1 >> 8), char(g.Data1), 0, 0, 0, 0};
    for (char* p = b; *p; ++p) if (*p < 32 || *p > 126) *p = '?';
    return b;
}

static std::string Hex(long hr) {
    char b[16];
    sprintf(b, "0x%08X", (unsigned)hr);
    return b;
}

static std::string W(const wchar_t* s) {
    if (!s) return "";
    std::string o;
    for (; *s; ++s) o += char(*s < 32 || *s > 126 ? '?' : *s);
    return o;
}

// ------------------------------------------------------------------ decoders
static int Decoders() {
    const struct { const char* name; REFGUID g; } cats[] = {
        {"decoder", MFT_CATEGORY_VIDEO_DECODER},
        {"transform", MFT_CATEGORY_VIDEO_PROCESSOR},
        {"effect", MFT_CATEGORY_VIDEO_EFFECT},
        {"encoder", MFT_CATEGORY_VIDEO_ENCODER},
    };
    IMFActivate** acts = nullptr;
    UINT n = 0;
    for (auto& c : cats) {
        acts = nullptr;
        n = 0;
        const long hr = MFTEnumEx(c.g, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, nullptr, &acts, &n);
        std::printf("category %-9s enum=%s count=%u\n", c.name, Hex(hr).c_str(), n);
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMFAttributes> a;
            if (SUCCEEDED(acts[i]->QueryInterface(IID_PPV_ARGS(&a)))) {
                WCHAR* nm = nullptr;
                UINT len = 0;
                a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len);
                std::printf("   %s\n", W(nm).c_str());
                CoTaskMemFree(nm);
            }
            // The input/output type lists are what decides whether a conversion is possible at all.
            ComPtr<IMFTransform> tf;
            if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&tf)))) {
                std::string in, out;
                for (DWORD i2 = 0; i2 < 24; ++i2) {
                    ComPtr<IMFMediaType> mt;
                    if (FAILED(tf->GetInputAvailableType(0, i2, &mt))) break;
                    GUID sub{};
                    mt->GetGUID(MF_MT_SUBTYPE, &sub);
                    if (!in.empty()) in += ",";
                    in += Fourcc(sub);
                }
                for (DWORD i2 = 0; i2 < 24; ++i2) {
                    ComPtr<IMFMediaType> mt;
                    if (FAILED(tf->GetOutputAvailableType(0, i2, &mt))) break;
                    GUID sub{};
                    mt->GetGUID(MF_MT_SUBTYPE, &sub);
                    if (!out.empty()) out += ",";
                    out += Fourcc(sub);
                }
                std::printf("       in=[%s] out=[%s]\n", in.c_str(), out.c_str());
            }
            acts[i]->Release();
        }
        if (acts) CoTaskMemFree(acts);
    }
    return 0;
}


// ------------------------------------------------------------------ mft filter
// The reader picks a decoder with exactly this call. If it comes back empty for H264 -> NV12, no
// recipe in a media type can ever be accepted, and the fault is in the plugin layer.
static int MftFilter() {
    const struct { const char* name; GUID in; GUID out; unsigned flags; } tries[] = {
        {"H264->NV12 sw", MFVideoFormat_H264, MFVideoFormat_NV12, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER},
        {"H264->NV12 any", MFVideoFormat_H264, MFVideoFormat_NV12, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER},
        {"H264->NV12 hard", MFVideoFormat_H264, MFVideoFormat_NV12, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER},
        {"H264->RGB32 any", MFVideoFormat_H264, MFVideoFormat_RGB32, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER},
        {"H264->* any", MFVideoFormat_H264, GUID{}, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER},
        {"*->NV12 any", GUID{}, MFVideoFormat_NV12, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER},
        {"HEVC->NV12 any", MFVideoFormat_HEVC, MFVideoFormat_NV12, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER},
    };
    for (auto& t : tries) {
        MFT_REGISTER_TYPE_INFO in{}, out{};
        const MFT_REGISTER_TYPE_INFO* pin = nullptr;
        if (t.in.Data1) { in.guidMajorType = MFMediaType_Video; in.guidSubtype = t.in; pin = &in; }
        const MFT_REGISTER_TYPE_INFO* pout = nullptr;
        IMFActivate** acts = nullptr;
        UINT n = 0;
        if (t.out.Data1) { out.guidMajorType = MFMediaType_Video; out.guidSubtype = t.out; pout = &out; }
        const long hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, t.flags, pin, pout, &acts, &n);
        std::printf("decoder %-16s enum=%s count=%u\n", t.name, Hex(hr).c_str(), n);
        for (UINT i = 0; i < n && acts; ++i) {
            WCHAR* nm = nullptr;
            UINT len = 0;
            ComPtr<IMFAttributes> a;
            if (SUCCEEDED(acts[i]->QueryInterface(IID_PPV_ARGS(&a)))) a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len);
            std::printf("        %s\n", W(nm).c_str());
            CoTaskMemFree(nm);
            acts[i]->Release();
        }
        if (acts) CoTaskMemFree(acts);
    }
    return 0;
}


// ------------------------------------------------------------------ chain
// Drive the decoder MFT by hand: the reader is the thing that says no, so this asks the decoder
// itself whether it will take this file's native type and give back a frame. Three separate answers
// are possible and they mean different fixes: SetInputType failing points at the native type,
// SetOutputType failing points at the plugin filter, and a delivered frame points at the reader.
static int Chain(const std::wstring& file) {
    std::wprintf(L"=== chain %s\n", file.c_str());
    ComPtr<IMFSourceReader> rd;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &rd))) { std::printf("  reader failed\n"); return 1; }
    ComPtr<IMFMediaType> native;
    if (FAILED(rd->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
        std::printf("  no native type\n");
        return 1;
    }
    GUID insub{};
    native->GetGUID(MF_MT_SUBTYPE, &insub);
    UINT w = 0, h = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    std::printf("  native %s %ux%u\n", Fourcc(insub).c_str(), w, h);

    IMFActivate** acts = nullptr;
    UINT n = 0;
    MFT_REGISTER_TYPE_INFO tin{MFMediaType_Video, insub}, tout{MFMediaType_Video, MFVideoFormat_NV12};
    long hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &tin, &tout,
                        &acts, &n);
    std::printf("  MFTEnumEx(%s->NV12)=%s count=%u\n", Fourcc(insub).c_str(), Hex(hr).c_str(), n);
    if (!n) return 1;
    ComPtr<IMFTransform> tf;
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(&tf));
    std::printf("  ActivateObject=%s\n", Hex(hr).c_str());
    acts[0]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr) || !tf) return 1;

    WCHAR* nm = nullptr;
    UINT len = 0;
    ComPtr<IMFAttributes> ta;
    if (SUCCEEDED(tf.As(&ta))) {
        ta->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len);
        std::printf("  mft: %s\n", W(nm).c_str());
        CoTaskMemFree(nm);
    }
    tf->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    std::printf("  SetInputType(native)=%s\n", Hex(tf->SetInputType(0, native.Get(), 0)).c_str());

    MFT_INPUT_STREAM_INFO isi{};
    std::printf("  GetInputStreamInfo=%s align=%u latency=%u\n",
                Hex(tf->GetInputStreamInfo(0, &isi)).c_str(), isi.cbAlignment, isi.cbSize);

    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    out->SetUINT64(MF_MT_FRAME_SIZE, (UINT64(w) << 32) | h);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = tf->SetOutputType(0, out.Get(), 0);
    std::printf("  SetOutputType(NV12)=%s\n", Hex(hr).c_str());
    if (FAILED(hr)) return 1;
    tf->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);

    // Now feed it: compressed samples in, one NV12 frame out.
    DWORD want = w * h + (w * h) / 2;
    std::printf("  expecting %u bytes per NV12 frame\n", want);
    for (int fed = 0; fed < 150; ++fed) {
        ComPtr<IMFSample> cs;
        DWORD flags = 0;
        if (FAILED(rd->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &cs)) || !cs)
            break;
        long ih = tf->ProcessInput(0, cs.Get(), 0);
        if (fed < 3) std::printf("  fed=%d ProcessInput=%s\n", fed, Hex(ih).c_str());
        if (FAILED(ih)) continue;
        for (int attempt = 0; attempt < 3; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER odb{};
            // The caller owns the output buffer: ProcessOutput with a null pSample answers
            // E_INVALIDARG, which is not a decoder failure.
            ComPtr<IMFMediaBuffer> mb;
            if (FAILED(MFCreateMemoryBuffer(want, &mb))) return 1;
            ComPtr<IMFSample> os;
            if (FAILED(MFCreateSample(&os))) return 1;
            os->AddBuffer(mb.Get());
            odb.pSample = os.Get();
            DWORD st = 0;
            hr = tf->ProcessOutput(0, 1, &odb, &st);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                // The decoder has read the SPS and now wants its own output type re-set. Take the
                // first NV12 type it advertises and recompute the frame size from it.
                std::printf("  fed=%d stream change ->", fed);
                for (DWORD k = 0; k < 16; ++k) {
                    ComPtr<IMFMediaType> ot2;
                    if (FAILED(tf->GetOutputAvailableType(0, k, &ot2))) break;
                    GUID s2{};
                    ot2->GetGUID(MF_MT_SUBTYPE, &s2);
                    if (s2 != MFVideoFormat_NV12) continue;
                    std::printf(" reset=%s", Hex(tf->SetOutputType(0, ot2.Get(), 0)).c_str());
                    UINT cw = 0, ch = 0;
                    MFGetAttributeSize(ot2.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
                    if (cw && ch) {
                        w = cw;
                        h = ch;
                        want = w * h + (w * h) / 2;
                    }
                    std::printf(" now %ux%u want=%u\n", w, h, want);
                    break;
                }
                continue;
            }
            if (FAILED(hr)) { std::printf("  fed=%d ProcessOutput=%s\n", fed, Hex(hr).c_str()); break; }
            if (odb.pEvents) odb.pEvents->Release();
            ComPtr<IMFMediaBuffer> b;
            DWORD bl = 0;
            if (os && SUCCEEDED(os->GetBufferByIndex(0, &b)) && b) b->GetCurrentLength(&bl);
            std::printf("  fed=%d frame out len=%u (want %u) %s\n", fed, bl, want,
                        bl == want ? "<== DECODE WORKS\n" : "");
            if (bl == want) return 0;
            break;
        }
    }
    std::printf("  no frame came out\n");
    return 1;
}

// ------------------------------------------------------------------ decoder types
// Hand the reader the very type the decoder advertises for itself. If the reader accepts that, the
// refusals are about how the requested type is spelled; if it still says no, the reader is not
// inserting the decoder at all.
static int DecoderTypes(const std::wstring& file) {
    std::wprintf(L"=== dt %s\n", file.c_str());
    ComPtr<IMFSourceReader> rd;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &rd))) return 1;
    ComPtr<IMFMediaType> native;
    if (FAILED(rd->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) return 1;
    GUID insub{};
    native->GetGUID(MF_MT_SUBTYPE, &insub);
    MFT_REGISTER_TYPE_INFO tin{MFMediaType_Video, insub};
    IMFActivate** acts = nullptr;
    UINT n = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &tin, nullptr, &acts,
              &n);
    std::printf("  decoders for input %s: %u\n", Fourcc(insub).c_str(), n);
    for (UINT a = 0; acts && a < n; ++a) {
        ComPtr<IMFTransform> tf;
        if (FAILED(acts[a]->ActivateObject(IID_PPV_ARGS(&tf)))) {
            acts[a]->Release();
            continue;
        }
        std::printf("  SetInputType=%s\n", Hex(tf->SetInputType(0, native.Get(), 0)).c_str());
        for (DWORD i = 0; i < 16; ++i) {
            ComPtr<IMFMediaType> ot;
            if (FAILED(tf->GetOutputAvailableType(0, i, &ot))) break;
            GUID sub{};
            ot->GetGUID(MF_MT_SUBTYPE, &sub);
            ComPtr<IMFSourceReader> r2;
            if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &r2))) continue;
            DWORD res = 0;
                        BOOL sel = FALSE;
            r2->GetStreamSelection(0, &sel);
            const long selH = r2->SetStreamSelection(0, TRUE);
            std::printf("    stream0 selected_before=%d force_select=%s|", (int)sel, Hex(selH).c_str());
            const long h2 = r2->SetCurrentMediaType(0, &res, ot.Get());
            std::printf("    decoder out %-6s -> reader set=%s pdwResult=0x%X\n", Fourcc(sub).c_str(),
                        Hex(h2).c_str(), res);
            if (SUCCEEDED(h2)) {
                ComPtr<IMFSample> s2;
                DWORD fl = 0, len = 0;
                const long rh = r2->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &fl, nullptr,
                                               &s2);
                ComPtr<IMFMediaBuffer> buf;
                if (s2 && SUCCEEDED(s2->GetBufferByIndex(0, &buf)) && buf) buf->GetCurrentLength(&len);
                std::printf("      read=%s len=%u\n", Hex(rh).c_str(), len);
            }
        }
        acts[a]->Release();
    }
    if (acts) CoTaskMemFree(acts);
    return 0;
}

// ------------------------------------------------------------------ byte stream
// The other way in: hand the reader an IMFByteStream (MFCreateFile) instead of a URL. If the URL
// path is what stops the reader from inserting a decoder, this costs three lines in the product.
static int ByteStream(const std::wstring& file) {
    std::wprintf(L"=== bs %s\n", file.c_str());
    ComPtr<IMFSourceReader> rd;
    ComPtr<IMFByteStream> bs;
    long hr = MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, file.c_str(), &bs);
    std::printf("  MFCreateFile=%s\n", Hex(hr).c_str());
    if (FAILED(hr)) return 1;
    hr = MFCreateSourceReaderFromByteStream(bs.Get(), nullptr, &rd);
    std::printf("  create-from-bytestream=%s\n", Hex(hr).c_str());
    if (FAILED(hr)) return 1;
    ComPtr<IMFMediaType> native;
    rd->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native);
    UINT w = 0, h = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    GUID nsub{};
    native->GetGUID(MF_MT_SUBTYPE, &nsub);
    std::printf("  native %s %ux%u\n", Fourcc(nsub).c_str(), w, h);
    const GUID targets[] = {MFVideoFormat_NV12, MFVideoFormat_RGB32};
    for (GUID t : targets) {
        ComPtr<IMFSourceReader> r2;
        MFCreateSourceReaderFromByteStream(bs.Get(), nullptr, &r2);
        if (!r2) continue;
        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, t);
        want->SetUINT64(MF_MT_FRAME_SIZE, (UINT64(w) << 32) | h);
        want->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        DWORD res = 0;
        const long sh = r2->SetCurrentMediaType(0, &res, want.Get());
        std::printf("  bytestream %-6s set=%s pdw=0x%X", Fourcc(t).c_str(), Hex(sh).c_str(), res);
        if (FAILED(sh)) {
            std::printf("\n");
            continue;
        }
        for (int i = 0; i < 60; ++i) {
            ComPtr<IMFSample> s;
            DWORD fl = 0, len = 0;
            const long rh = r2->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &fl, nullptr, &s);
            ComPtr<IMFMediaBuffer> b;
            if (s && SUCCEEDED(s->GetBufferByIndex(0, &b)) && b) b->GetCurrentLength(&len);
            if (SUCCEEDED(rh) && len) {
                std::printf(" | i=%d read=ok len=%u\n", i, len);
                break;
            }
            if (FAILED(rh)) {
                std::printf(" | i=%d read=%s\n", i, Hex(rh).c_str());
                break;
            }
        }
        std::printf("\n");
    }
    return 0;
}

// ------------------------------------------------------------------ negotiate
static void DumpNative(IMFSourceReader* rd) {
    ComPtr<IMFMediaType> t;
    if (FAILED(rd->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &t))) {
        std::printf("  no native video type\n");
        return;
    }
    GUID maj{}, sub{};
    t->GetGUID(MF_MT_MAJOR_TYPE, &maj);
    t->GetGUID(MF_MT_SUBTYPE, &sub);
    UINT w = 0, h = 0, stride = 0, ss = 0, inter = 0;
    UINT32 num = 0, den = 0;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
    t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    t->GetUINT32(MF_MT_SAMPLE_SIZE, &ss);
    t->GetUINT32(MF_MT_INTERLACE_MODE, &inter);
    UINT32 hasProfile = 0;
    const bool prof = SUCCEEDED(t->GetUINT32(MF_MT_MPEG2_PROFILE, &hasProfile));
    std::printf("  native: %s %ux%u fps=%u/%u stride=%u sample_size=%u interlace=%u %s\n", Fourcc(sub).c_str(), w, h,
                num, den, stride, ss, inter, prof ? "has-MPEG2_PROFILE" : "no-MPEG2_PROFILE");
}

enum Recipe { NATIVE = 0, FRESH_MIN, FRESH_SIZE, CLONE_SUBTYPE, CLONE_STRIP };

// One negotiation attempt. Everything about it is printed, including the sample the reader
// eventually hands over, because "accepted" and "works" are different claims.
static void Try(IMFSourceReader* rd, const char* name, GUID subtype, Recipe rec, IMFMediaType* native) {
    ComPtr<IMFMediaType> want;
    long hr = S_OK;
    if (rec == NATIVE) {
        std::printf("  %-22s (no SetCurrentMediaType)\n", name);
    } else if (rec == FRESH_MIN) {
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, subtype);
    } else if (rec == FRESH_SIZE) {
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, subtype);
        UINT w = 0, h = 0;
        MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h);
        want->SetUINT64(MF_MT_FRAME_SIZE, (UINT64(w) << 32) | h);
        want->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (UINT64(1) << 32) | 1);
        want->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        want->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(INT32(-4 * int(w))));
    } else {
        MFCreateMediaType(&want);
        native->CopyAllItems(want.Get());
        if (rec == CLONE_STRIP) {
            // Attributes that describe the compressed stream, which an uncompressed output type has
            // no business carrying. RemoveItem lives on IMFAttributes, not on IMFMediaType.
            ComPtr<IMFAttributes> wa;
            want.As(&wa);
            if (wa) {
                wa->DeleteItem(MF_MT_SAMPLE_SIZE);
                wa->DeleteItem(MF_MT_AVG_BITRATE);
                wa->DeleteItem(MF_MT_MPEG2_PROFILE);
                wa->DeleteItem(MF_MT_MPEG2_LEVEL);
                wa->DeleteItem(MF_MT_VIDEO_PROFILE);
                wa->DeleteItem(MF_MT_VIDEO_LEVEL);
            }
        }
        want->SetGUID(MF_MT_SUBTYPE, subtype);
        want->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        want->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    DWORD res = 0;
    if (rec != NATIVE) hr = rd->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &res, want.Get());
    if (FAILED(hr)) {
        std::printf("  %-22s set=%s pdwResult=0x%X\n", name, Hex(hr).c_str(), res);
        return;
    }
    ComPtr<IMFMediaType> got;
    rd->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &got);
    GUID sub{};
    if (got) got->GetGUID(MF_MT_SUBTYPE, &sub);
    std::printf("  %-22s set=ok got=%s", name, Fourcc(sub).c_str());
    // Two samples: the first one is often the decoder flushing its pipeline.
    for (int i = 0; i < 2; ++i) {
        ComPtr<IMFSample> s;
        DWORD flags = 0;
        DWORD len = 0;
        const long rh = rd->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, nullptr, &s);
        if (s) {
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && buf) buf->GetCurrentLength(&len);
        }
        std::printf(" | read%d=%s len=%u flags=0x%X", i, Hex(rh).c_str(), len, flags);
    }
    std::printf("\n");
}

static int Negotiate(const std::wstring& file) {
    std::wprintf(L"=== %s\n", file.c_str());
    // The attribute sets the two shipped code paths use, plus the older flag that is documented to
    // enable the conversion path.
    // The shipped code always sets MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, which makes MF prefer the
    // vendor (NVIDIA) decoders over Microsoft's software one, and always asks for a video-processing
    // flag. Both are switched off here in turn to see which of them, if either, is load-bearing.
    const struct { const char* tag; bool dx, advanced, videoproc, hwxf, disableDxva, tronly; } attrsets[] = {
        {"none",       false, false, false, false, false, false},
        {"tronly",     false, false, false, false, false, true},
        {"vp+tr",      false, false, true,  false, false, true},
        {"adv+tr",     false, true,  false, false, false, true},
        {"vp",         false, false, true,  true,  false},
        {"adv",        false, true,  false, true,  false},
        {"vp nohw",    false, false, true,  false, false, false},
        {"adv nohw",   false, true,  false, false, false, false},
        {"none+dxvaoff", false, false, false, false, true, false},
        {"vp+dx",      true,  false, true,  true,  false, false},
        {"adv+dx",     true,  true,  false, true,  false, false},
        {"adv+dx+tr",  true,  true,  false, true,  false, true},
    };
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        ComPtr<ID3D11Multithread> mt;
        ctx.As(&mt);
        if (mt) mt->SetMultithreadProtected(TRUE);
    } else {
        dev.Reset();
    }

    for (auto& as : attrsets) {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 4);
        ComPtr<IMFDXGIDeviceManager> dxm;
        if (as.dx && dev) {
            UINT token = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &dxm))) {
                dxm->ResetDevice(dev.Get(), token);
                a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxm.Get());
            }
        }
        if (as.advanced) a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (as.videoproc) a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (as.hwxf) a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (as.disableDxva) a->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
        if (as.tronly) a->SetUINT32(MF_SOURCE_READER_ENABLE_TRANSCODE_ONLY_TRANSFORMS, TRUE);
        ComPtr<IMFSourceReader> rd;
        long hr = MFCreateSourceReaderFromURL(file.c_str(), a.Get(), &rd);
        if (FAILED(hr)) {
            std::printf("  [%s] create-reader=%s\n", as.tag, Hex(hr).c_str());
            continue;
        }
        ComPtr<IMFMediaType> native;
        if (FAILED(rd->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
            std::printf("  [%s] no native type\n", as.tag);
            continue;
        }
        if (strcmp(as.tag, "vp") == 0) DumpNative(rd.Get());
        std::printf("  --- attrs [%s]\n", as.tag);
        char nm[64];
        sprintf(nm, "%s native", as.tag);
        Try(rd.Get(), nm, GUID{}, NATIVE, native.Get());
        rd.Reset();

        // Each recipe needs a fresh reader: a refused SetCurrentMediaType can leave state behind.
        auto reopen = [&] {
            ComPtr<IMFSourceReader> r;
            ComPtr<IMFAttributes> at;
            MFCreateAttributes(&at, 4);
            if (as.dx && dev) {
                UINT token = 0;
                ComPtr<IMFDXGIDeviceManager> m;
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &m))) {
                    m->ResetDevice(dev.Get(), token);
                    at->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m.Get());
                }
            }
            if (as.advanced) at->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            if (as.videoproc) at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            if (as.hwxf) at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            if (as.disableDxva) at->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
            if (as.tronly) at->SetUINT32(MF_SOURCE_READER_ENABLE_TRANSCODE_ONLY_TRANSFORMS, TRUE);
            MFCreateSourceReaderFromURL(file.c_str(), at.Get(), &r);
            return r;
        };
        const GUID targets[] = {MFVideoFormat_RGB32, MFVideoFormat_NV12, MFVideoFormat_YUY2};
        for (GUID t : targets) {
            for (Recipe rec : {FRESH_MIN, FRESH_SIZE, CLONE_SUBTYPE, CLONE_STRIP}) {
                ComPtr<IMFSourceReader> r = reopen();
                if (!r) continue;
                ComPtr<IMFMediaType> n2;
                r->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &n2);
                sprintf(nm, "%s %s rec%d", as.tag, Fourcc(t).c_str(), int(rec));
                Try(r.Get(), nm, t, rec, n2.Get());
            }
        }
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: mfprobe decoders | mfprobe negotiate <file> [...]\n");
        return 1;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("MFStartup failed\n");
        return 1;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not cost the readings already taken
    const std::string mode = W(argv[1]);
    int rc = 0;
    if (mode == "decoders") rc = Decoders();
    if (mode == "mft") rc = MftFilter();
    for (int i = 2; i < argc && mode == "chain"; ++i) rc = Chain(argv[i]);
    for (int i = 2; i < argc && mode == "dt"; ++i) rc = DecoderTypes(argv[i]);
    for (int i = 2; i < argc && mode == "bs"; ++i) rc = ByteStream(argv[i]);
    for (int i = 2; i < argc && mode == "negotiate"; ++i) Negotiate(argv[i]);
    MFShutdown();
    return rc;
}
