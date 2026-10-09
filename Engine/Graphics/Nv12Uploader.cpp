// Engine/Graphics/Nv12Uploader.cpp - see Nv12Uploader.hpp for what this fixes and why it is shared.
#include "Engine/Graphics/Nv12Uploader.hpp"

#include "Engine/Core/Log.hpp"

#include <algorithm>
#include <cstring>

namespace sw {

namespace {
constexpr const char* MOD = "video";

// Microseconds since construction.
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
} // namespace

void Nv12Uploader::Reset() {
    yTex_.Reset();
    uvTex_.Reset();
    ySrv_.Reset();
    uvSrv_.Reset();
    cpuY_.Reset();
    cpuUv_.Reset();
    decTex_.Reset();
    decViews_.clear();
    adopted_ = false;
}

void Nv12Uploader::UseCpuViews() {
    if (!adopted_ && ySrv_ == cpuY_) return;
    adopted_ = false;
    ySrv_ = cpuY_;
    uvSrv_ = cpuUv_;
}

bool Nv12Uploader::AdoptDecoded(ID3D11Texture2D* tex, UINT slice, UINT codedW, UINT codedH,
                                UINT displayH) {
    if (!dev_ || !tex || !codedW || !codedH || !displayH) return false;
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    // The whole plan rests on this flag being on the pool. It is not by default: FFmpeg builds the
    // decoder's textures as D3D11_BIND_DECODER only, and every CreateShaderResourceView then fails with
    // E_INVALIDARG. The flag that fixes it is the DEVICE-level one (AVD3D11VADeviceContext::BindFlags),
    // not the frames-level one - measured, see BACKLOG's D3D11VA entry.
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
    if (d.Width < codedW || d.Height < codedH || (d.Format != DXGI_FORMAT_NV12 && d.Format != 103))
        return false;
    if (decTex_.Get() != tex) {           // a new pool (resolution change, flush): the views are stale
        decViews_.clear();
        decTex_ = tex;
    }
    for (auto& v : decViews_) {
        if (v.slice != slice) continue;
        ySrv_ = v.y;
        uvSrv_ = v.uv;
        adopted_ = true;
        return true;
    }
    auto view = [&](DXGI_FORMAT fmt, Com<ID3D11ShaderResourceView>* out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = fmt;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MostDetailedMip = 0;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.FirstArraySlice = slice;
        vd.Texture2DArray.ArraySize = 1;
        return SUCCEEDED(dev_->CreateShaderResourceView(tex, &vd, out->GetAddressOf()));
    };
    Views v;
    if (!view(DXGI_FORMAT_R8_UNORM, &v.y) || !view(DXGI_FORMAT_R8G8_UNORM, &v.uv)) {
        return false;
    }
    v.slice = slice;
    decViews_.push_back(std::move(v));
    ySrv_ = decViews_.back().y;
    uvSrv_ = decViews_.back().uv;
    adopted_ = true;
    return true;
}

bool Nv12Uploader::Ensure(ID3D11Device* dev, UINT codedW, UINT codedH, UINT displayH) {
    if (!dev || !codedW || !codedH || !displayH) return false;
    if (ySrv_ && dev == dev_ && codedW == codedW_ && codedH_ == codedH && dispH_ == displayH) return true;

    dev_ = dev;
    codedW_ = codedW;
    codedH_ = codedH;
    dispH_ = displayH;
    Reset();   // the geometry moved (or the device did): the old pair cannot describe the new frame

    D3D11_TEXTURE2D_DESC td{};
    // Explicit ARRAY views: a null descriptor on a 1-slice texture gives a TEXTURE2D view, and Video.hlsl
    // now declares Texture2DArray so that a D3D11VA decoder's slices (which can only be viewed as
    // TEXTURE2DARRAY + FirstArraySlice) bind to the same slot. The two sources have to be interchangeable
    // at the shader, so the CPU path presents itself as a one-slice array too.
    auto arrayView = [&](ID3D11Texture2D* tex, Com<ID3D11ShaderResourceView>& srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MostDetailedMip = 0;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.FirstArraySlice = 0;
        vd.Texture2DArray.ArraySize = 1;
        return SUCCEEDED(dev->CreateShaderResourceView(tex, &vd, srv.GetAddressOf()));
    };

    td.Width = codedW;
    td.Height = displayH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &yTex_))) return false;
    if (!arrayView(yTex_.Get(), ySrv_)) return false;

    td.Width = codedW / 2;
    td.Height = displayH / 2;
    td.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &uvTex_))) return false;
    if (!arrayView(uvTex_.Get(), uvSrv_)) return false;

    // Zero-filled textures are not neutral: the shader reads Y=0,U=0,V=0 and turns it into
    // R0 G76 B0 - the green that showed up on a card and on a desktop that had not received its first
    // frame yet. Limited-range video black is Y=16 with neutral chroma, so clearing to that makes
    // "no picture yet" draw black like every other video source does instead of inventing a colour.
    // (Unbound shader slots read zero too, so this only holds together with FrameY never returning
    // null once the file is open - see FFmpegPlayer::Open.)
    std::vector<BYTE> blank(size_t(codedW) * displayH + size_t(codedW) * (displayH / 2), 0);
    std::fill_n(blank.begin(), size_t(codedW) * displayH, BYTE(16));
    std::fill(blank.begin() + size_t(codedW) * displayH, blank.end(), BYTE(128));
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);   // Ensure runs on the render thread, same context the draw uses
    if (imm) {
        imm->UpdateSubresource(yTex_.Get(), 0, nullptr, blank.data(), codedW, 0);
        imm->UpdateSubresource(uvTex_.Get(), 0, nullptr, blank.data() + size_t(codedW) * displayH,
                               codedW, 0);
        imm->Release();
    }

    Info(MOD, "plane textures {}x{} R8 + {}x{} R8G8 (cleared to video black)", codedW, displayH,
         codedW / 2, displayH / 2);
    cpuY_ = ySrv_;
    cpuUv_ = uvSrv_;
    adopted_ = false;
    return true;
}

bool Nv12Uploader::Upload(ID3D11DeviceContext* ctx, const BYTE* nv12, size_t have, Out& out) {
    UseCpuViews();      // a software frame after a hardware one must not leave the shader on the pool
    const UINT w = codedW_, dispH = dispH_, codedH = codedH_;
    const size_t need = size_t(w) * codedH * 3 / 2;
    out.haveBytes = (unsigned)have;
    out.needBytes = (unsigned)need;
    if (!ctx || !nv12 || have < need || !yTex_ || !uvTex_) return false;

    // Straight from the packed buffer into the two textures: one D3D call per plane, the box doing the
    // coded-to-display crop, so no CPU staging rows are built first. The old version memcpy'd every
    // row into `rows_`/`chroma_` and then handed those to UpdateSubresource, which measured
    // 1.04-1.15 ms per 4K60 frame of pure copying on top of the upload itself.
    //
    // The pitches are legal because a coded width is macroblock-aligned (a multiple of 16), and
    // UpdateSubresource wants a 16-byte-multiple row pitch. The chroma texture is R8G8 with w/2
    // texels per row, which is exactly the w bytes an NV12 chroma row occupies - so its box is in
    // texels while its pitch stays in bytes.
    const D3D11_BOX yBox{ 0, 0, 0, w - 1, dispH - 1, 1 };
    const D3D11_BOX uvBox{ 0, 0, 0, w / 2 - 1, dispH / 2 - 1, 1 };
    const size_t yPlane = size_t(w) * codedH;
    const Clock t1;
    ctx->UpdateSubresource(yTex_.Get(), 0, &yBox, nv12, w, 0);
    ctx->UpdateSubresource(uvTex_.Get(), 0, &uvBox, nv12 + yPlane, w, 0);
    out.copyUs = t1.us();

    // UpdateSubresource reports nothing, so a blank chroma plane can only be caught in the bytes
    // before they go up: the green frames measured R 3 / G 173 / B 0, which is exactly
    // "luma present, U=V=0". Sampled out of the source buffer, since there is no copy to sample.
    unsigned long long ySum = 0, uSum = 0;
    size_t yn = 0, un = 0;
    for (size_t i = 0; i < yPlane; i += 97) { ySum += nv12[i]; ++yn; }
    for (size_t i = 0; i < yPlane / 2; i += 194) { uSum += nv12[yPlane + i]; ++un; }   // every U byte
    out.lumaMean = yn ? (unsigned)(ySum / yn) : 0;
    out.chromaMean = un ? (unsigned)(uSum / un) : 0;
    return true;
}

} // namespace sw
