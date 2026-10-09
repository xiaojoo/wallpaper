#pragma once
// Engine/Graphics/Nv12Uploader.hpp - the decoded picture -> the two shader resources Video.hlsl reads.
//
// The one place plane geometry is written, because it is where the measured bugs live in this project:
//   - the coded height is larger than the display height (measured 1080 -> 1088) and the extra rows
//     sit at the bottom of the buffer, so the picture is the FIRST displayH rows, not the last;
//   - NV12 chroma is halved in BOTH axes, so a w x h picture's chroma plane is w/2 x h/2 held in
//     w bytes per row - sizing the R8G8 texture to w texels instead of w/2 makes the driver read
//     twice the row we own, which came out as an access violation inside the NVIDIA driver.
// Row order is not a variable: decoders hand back top-down NV12. A vertical flip used to sit here,
// decided from a probe that read the fixture's stored order as "the decoder flipped it" when the
// fixture generator had stored it inverted - the instrument that settles that question now is
// FfmpegProbeVideo's caller plus sampling the decoder's own buffer, and the whole story is in
// BACKLOG's video entry and the git history of VideoPlayer.cpp.
// Fixing the geometry here (rather than in the shader) is what lets Video.hlsl keep the same
// top-down, display-cropped contract as Image.hlsl.
#include "Engine/Core/Platform.hpp"

#include <vector>

namespace sw {

class Nv12Uploader {
public:
    // Per-frame costs, so the caller can keep them in its own once-a-second stats line.
    struct Out {
        unsigned long long copyUs = 0;                  // the two UpdateSubresource calls
        unsigned lumaMean = 0, chromaMean = 0;          // neutral chroma is 128; 0 means a blank plane
        unsigned haveBytes = 0, needBytes = 0;
    };

    // Idempotent; rebuilds the textures only when the geometry actually moved (a stream change can
    // report a bigger coded height after the first keyframes).
    bool Ensure(ID3D11Device* dev, UINT codedW, UINT codedH, UINT displayH);
    void Reset();
    bool Ready() const { return static_cast<bool>(ySrv_); }

    // Returns false without touching the textures when <have> is shorter than a coded plane - that is
    // how a half-filled buffer shows up before it can paint the wallpaper green.
    bool Upload(ID3D11DeviceContext* ctx, const BYTE* nv12, size_t have, Out& out);

    // The zero-copy alternative to Upload: hand the shader views over one surface of a D3D11VA decoder's
    // NV12 array instead of copying 12.4 MB through the CPU first. `slice` is `AVFrame::data[1]`, and
    // both planes live at that same slice - the plane is chosen by the view's format (R8 for luma,
    // R8G8 for chroma), which is what tools/dxvaspike.cpp measured rather than assumed: sampling slice
    // 17 gave the frame's luma and chroma, slices nobody had written gave 0, and 17 + ArraySize could
    // not even be viewed.
    //
    // False (and nothing changes) when the texture cannot be sampled - a decoder pool built without
    // D3D11_BIND_SHADER_RESOURCE, or geometry that does not describe it - so the caller keeps using the
    // CPU path for that frame. Views are cached per slice: the pool is 24 surfaces deep and the decoder
    // recycles through it, so building two per frame would be 96 COM allocations a second for something
    // that repeats.
    bool AdoptDecoded(ID3D11Texture2D* tex, UINT slice, UINT codedW, UINT codedH, UINT displayH);

    // Back to the textures Upload() fills. Called by Upload itself, so a stream that mixes hardware
    // and software frames cannot leave the shader reading a recycled decoder surface.
    void UseCpuViews();

    bool Adopted() const { return adopted_; }

    ID3D11ShaderResourceView* Y() const { return ySrv_.Get(); }
    ID3D11ShaderResourceView* UV() const { return uvSrv_.Get(); }

private:
    struct Views {
        UINT slice = 0;
        Com<ID3D11ShaderResourceView> y, uv;
    };

    Com<ID3D11Texture2D> yTex_, uvTex_;
    Com<ID3D11ShaderResourceView> ySrv_, uvSrv_;         // whichever pair the shader is reading now
    Com<ID3D11ShaderResourceView> cpuY_, cpuUv_;         // restored by UseCpuViews
    Com<ID3D11Texture2D> decTex_;                         // the decoder's array we are viewing into
    std::vector<Views> decViews_;
    bool adopted_ = false;
    ID3D11Device* dev_ = nullptr;
    UINT codedW_ = 0, codedH_ = 0, dispH_ = 0;
};

} // namespace sw
