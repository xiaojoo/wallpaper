// tools/dxvaspike.cpp - does D3D11VA give us frames that never leave the GPU?
//
// Why this exists: the wallpaper's video path today is NVDEC (h264_cuvid) -> a system-memory AVFrame ->
// Pack() a 12.4 MB NV12 copy on the decode thread -> UpdateSubresource into two plane textures. Steps ①
// and ② cut the allocation and 6 of the 31 copies a second; the remaining copy only disappears if the
// decoder writes into D3D11 textures we sample directly. Before rewriting Nv12Uploader, Video.hlsl, the
// loop seam and the thumbnail park around that assumption, this probe answers four questions with
// measurements:
//
//   1. Will FFmpeg's D3D11VA hwaccel accept *our* ID3D11Device (the one the engine already owns, created
//      for DirectComposition) instead of making its own?
//   2. Does that device need D3D11_CREATE_DEVICE_VIDEO_SUPPORT, and does the engine's current flag set
//      fail? A decoder writing into a texture our context reads concurrently also needs the protected
//      multithread flag - both are reported.
//   3. What does a decoded frame actually hand back: texture + slice index, array size, bind flags.
//   4. Can an SRV be built over an individual slice with DXGI_FORMAT_R8_UNORM (luma) and
//      R8G8_UNORM (chroma)? That is the whole zero-copy bet: if those two views fail, Video.hlsl has
//      nothing to sample and the plan is dead.
//
// usage: dxvaspike.exe <file.mp4> [no-video-support]
// d3d11.h first: libavutil/hwcontext_d3d11va.h pulls it in, and inside an extern "C" block the SDK
// header's C++ comparison operators fail to compile ("cannot overload functions with external C
// linkage") - the include guard then decides which of the two wins.
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#pragma comment(lib, "d3d11.lib")

static SRWLOCK gLock = SRWLOCK_INIT;
static void Lock(void *) { AcquireSRWLockExclusive(&gLock); }
static void Unlock(void *) { ReleaseSRWLockExclusive(&gLock); }

static const char *err(int e) {
    static char b[64];
    av_strerror(e, b, sizeof(b));
    return b;
}

static int pick(const AVCodec *dec, int wantD3D11) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *c = avcodec_get_hw_config(dec, i);
        if (!c) break;
        printf("  hw config %d: methods=0x%x pix_fmt=%s device=%s\n", i, c->methods,
               c->pix_fmt >= 0 ? av_get_pix_fmt_name(c->pix_fmt) : "?",
               c->device_type >= 0 ? av_hwdevice_get_type_name(c->device_type) : "?");
        if (wantD3D11 && (c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            c->device_type == AV_HWDEVICE_TYPE_D3D11VA) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);     // a crash must not take the trail of prints with it
    const char *file = argc > 1 ? argv[1] : nullptr;
    const int noVideo = argc > 2 && strcmp(argv[2], "no-video-support") == 0;
    if (!file) { printf("usage: dxvaspike <file> [no-video-support]\n"); return 2; }

    // 1. A device like the engine's, plus/minus the video flag the decoder-written textures want.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (noVideo ? 0 : D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    printf("device create flags 0x%x (%s)\n", flags, noVideo ? "WITHOUT" : "WITH" " video support");
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed %08x\n", (unsigned)hr); return 1; }
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
        printf("SetMultithreadProtected(TRUE) ok\n");
        mt->Release();
    } else {
        printf("no ID3D10Multithread on this device\n");
    }

    // 2. Hand that device to FFmpeg instead of letting it open its own.
    AVBufferRef *devRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!devRef) { printf("hwdevice_ctx_alloc failed\n"); return 1; }
    AVHWDeviceContext *hc = (AVHWDeviceContext *)devRef->data;
    AVD3D11VADeviceContext *d3d = (AVD3D11VADeviceContext *)hc->hwctx;
    d3d->device = dev; dev->AddRef();
    d3d->device_context = ctx; ctx->AddRef();
    d3d->lock = Lock; d3d->unlock = Unlock;
    d3d->lock_ctx = nullptr;
    // The device-level flag is the one that counts: the d3d11va hwaccel builds the single array texture
    // itself and takes D3D11_TEXTURE2D_DESC.BindFlags from here ("applies globally to all
    // AVD3D11VFramesContext allocated from this device context"), so AVD3D11VAFramesContext::BindFlags
    // alone leaves the pool decoder-only and unsampleable - which is what the first run measured.
    d3d->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    hr = av_hwdevice_ctx_init(devRef);
    printf("av_hwdevice_ctx_init on our device: %s\n", hr >= 0 ? "OK" : err(hr));
    if (hr < 0) return 1;

    // 3. Decode with the *internal* h264 decoder (cuvid would copy to system memory, which is the thing
    //    we are trying to stop).
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, file, nullptr, nullptr) < 0) { printf("open failed\n"); return 1; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { printf("no stream info\n"); return 1; }
    const AVCodec *dec = avcodec_find_decoder_by_name("h264");
    if (!dec) { printf("no internal h264 decoder\n"); return 1; }
    const int cfg = pick(dec, 1);
    printf("chosen D3D11VA config index: %d\n", cfg);
    if (cfg < 0) return 1;
    const int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVCodecContext *c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, fmt->streams[vIdx]->codecpar);
    // The bind flags are the whole story. With only `hw_device_ctx` set, FFmpeg builds the decoder's
    // texture pool itself as D3D11_BIND_DECODER - and a resource without D3D11_BIND_SHADER_RESOURCE can
    // never be sampled, so every CreateShaderResourceView returns E_INVALIDARG (measured on this
    // machine: 3840x2160, array 20, bind 0x200, all seven view attempts FAIL). Handing the decoder an
    // explicit pool whose AVD3D11VAFramesContext::BindFlags adds SHADER_RESOURCE is what makes the
    // frames readable.
    c->hw_device_ctx = av_buffer_ref(devRef);
    AVBufferRef *frmRef = av_hwframe_ctx_alloc(devRef);
    if (!frmRef) { printf("hwframe_ctx_alloc failed\n"); return 1; }
    AVHWFramesContext *fc = (AVHWFramesContext *)frmRef->data;
    AVD3D11VAFramesContext *dfc = (AVD3D11VAFramesContext *)fc->hwctx;
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = c->width;
    fc->height = c->height;
    fc->initial_pool_size = 24;
    dfc->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    const int fri = av_hwframe_ctx_init(frmRef);
    printf("av_hwframe_ctx_init (decoder|shader_resource): %s\n", fri >= 0 ? "OK" : err(fri));
    if (fri >= 0) c->hw_frames_ctx = av_buffer_ref(frmRef);
    if (avcodec_open2(c, dec, nullptr) < 0) { printf("decoder open failed\n"); return 1; }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frm = av_frame_alloc();
    int shown = 0, slices = -1;
    ID3D11Texture2D *tex = nullptr;
    while (shown < 60 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vIdx) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(c, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);
        for (;;) {
            const int rc = avcodec_receive_frame(c, frm);
            if (rc == AVERROR(EAGAIN)) break;
            if (rc < 0) break;
            ++shown;
            if (shown <= 3 || shown == 60) {
                // data[1] is the slice index carried as a pointer value, not a string - printing it with
                // %s walks off into the address space and faults, which is how this probe died once.
                printf("frame %2d format %s (%d) texture=%p slice=%d\n", shown,
                       av_get_pix_fmt_name((AVPixelFormat)frm->format), frm->format,
                       (void *)frm->data[0], (int)(intptr_t)frm->data[1]);
                if (frm->format == AV_PIX_FMT_D3D11) {
                    tex = (ID3D11Texture2D *)frm->data[0];
                    D3D11_TEXTURE2D_DESC dd;
                    tex->GetDesc(&dd);
                    slices = (int)(intptr_t)frm->data[1];
                    printf("        texture: %ux%u array=%u mip=%u bind=0x%x fmt=%d\n",
                           dd.Width, dd.Height, dd.ArraySize, dd.MipLevels, dd.BindFlags, dd.Format);
                }
            }
            if (shown < 60) av_frame_unref(frm);   // the last frame stays referenced: the plane probe
                                                   // below reads its texture out of the pool
            break;
        }
    }
    printf("decoded %d frames, last slice index %d\n", shown, slices);
    if (!tex) { printf("no D3D11 texture handed back - zero-copy is not available here\n"); return 1; }

    // 4. The make-or-break: shader views over one slice each, luma as R8 and chroma as R8G8.
    D3D11_TEXTURE2D_DESC dd; tex->GetDesc(&dd);
    const struct { DXGI_FORMAT f; const char *n; } tries[] = {
        { DXGI_FORMAT_R8_UNORM, "R8 (luma plane)" },
        { DXGI_FORMAT_R8G8_UNORM, "R8G8 (chroma plane)" },
        { DXGI_FORMAT_NV12, "NV12 whole resource" },
    };
    for (auto &t : tries) {
        for (int sl = 0; sl < 2 && (unsigned)sl < dd.ArraySize; ++sl) {
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = t.f;
            v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Texture2DArray.MostDetailedMip = 0;
            v.Texture2DArray.MipLevels = 1;
            v.Texture2DArray.FirstArraySlice = sl;
            v.Texture2DArray.ArraySize = 1;
            ID3D11ShaderResourceView *srv = nullptr;
            const HRESULT r = dev->CreateShaderResourceView(tex, &v, &srv);
            printf("  SRV %-24s slice %d -> %s%p\n", t.n, sl,
                   SUCCEEDED(r) ? "OK " : "FAIL ", (void *)srv);
            if (srv) srv->Release();
        }
    }
    // 5. The last thing that can kill this plan: which subresource holds which plane. A view being
    //    creatable proves nothing about what it points at, and a wrong plane is the green-card colour
    //    again. So copy each candidate subresource out to a CPU-readable staging texture and average it.
    //    The engine's own log says what this clip's planes hold: luma mean 84, chroma mean 148 (neutral
    //    chroma is 128) - if luma lands near 84 and chroma near 148, the addressing is what we assumed.
    //    This runs while the frame is still held: the first version did it after av_frame_unref and read
    //    back zeros, which is a probe bug (the pool handed that texture back to the decoder), not a
    //    D3D11 answer. Held or not, this route still reads 0.0 for all three subresources on this machine:
    //    CopySubresourceRegion out of a D3D11_BIND_DECODER texture into a plain STAGING 2D is not how
    //    libavcodec moves frames to the CPU (it goes through av_hwframe_transfer_data). So these three
    //    numbers mean "the probe could not read them" - the plane layout has to be settled by sampling
    //    the views in a real draw and running the colour gate over that.
    if (shown == 60 && frm->format == AV_PIX_FMT_D3D11) {
        ID3D11Texture2D *t2 = (ID3D11Texture2D *)frm->data[0];
        const UINT s = (UINT)(intptr_t)frm->data[1];
        D3D11_TEXTURE2D_DESC d2; t2->GetDesc(&d2);
        const struct { UINT sub; DXGI_FORMAT f; UINT h; const char *n; } probes[] = {
            { s, DXGI_FORMAT_R8_UNORM, d2.Height, "luma   = subresource slice" },
            { d2.ArraySize + s, DXGI_FORMAT_R8G8_UNORM, d2.Height / 2, "chroma = ArraySize+slice" },
            { s + 1, DXGI_FORMAT_R8_UNORM, d2.Height, "(neighbour slice, must NOT match luma)" },
        };
        for (auto &pr : probes) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = d2.Width; sd.Height = pr.h; sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = pr.f; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *st = nullptr;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st)) || !st) {
                printf("  %-44s -> staging create failed\n", pr.n);
                continue;
            }
            ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t2, pr.sub, nullptr);
            D3D11_MAPPED_SUBRESOURCE mp{};
            const HRESULT mr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp);
            if (FAILED(mr)) {
                printf("  %-44s subresource %-3u -> Map failed %08x\n", pr.n, pr.sub, (unsigned)mr);
                st->Release();
                continue;
            }
            unsigned long long sum = 0, cnt = 0;
            const int bytes = pr.f == DXGI_FORMAT_R8G8_UNORM ? 2 : 1;
            for (UINT y = 0; y < pr.h; y += 7) {
                const unsigned char *row = (const unsigned char *)mp.pData + (size_t)y * mp.RowPitch;
                for (UINT x = 0; x < d2.Width; x += 7) { sum += row[(size_t)x * bytes]; ++cnt; }
            }
            ctx->Unmap(st, 0);
            printf("  %-44s subresource %-3u mean %6.1f  (expect 84 luma / 148 chroma)\n",
                   pr.n, pr.sub, cnt ? (double)sum / cnt : -1.0);
            st->Release();
        }
    }
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    av_buffer_unref(&devRef);
    ctx->Release();
    dev->Release();
    return 0;
}
