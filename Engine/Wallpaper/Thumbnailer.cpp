#include "Engine/Wallpaper/Thumbnailer.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Graphics/Texture.hpp"
#include "Engine/Wallpaper/WallpaperInstance.hpp"
#include "Engine/Core/Log.hpp"

#include <wincodec.h>
#include <algorithm>
#include <filesystem>

namespace sw {
static constexpr const char* MOD = "thumb";

bool Thumbnailer::Grab(D3D11Device& dev, D3D11Renderer& rend, WallpaperInstance& inst, UINT w, UINT h,
                       double seconds, double delta, const std::wstring& file, std::vector<BYTE>* pixels,
                       std::string& error, int jpegQuality) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    Com<ID3D11Texture2D> rt;
    if (FAILED(dev.dev->CreateTexture2D(&td, nullptr, &rt))) {
        error = "offscreen texture " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    Com<ID3D11RenderTargetView> rtv;
    if (FAILED(dev.dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv))) {
        error = "offscreen rtv";
        return false;
    }

    FrameCB f{};
    f.time = (float)seconds;
    // A stateful shader (snowfall integrates p.pos += vel * dt in its compute pass) advances by
    // this per grab, not by f.time, so the strip has to be stepped with its own spacing or the
    // 24 frames would only cover 24/60 s of the animation and play back in slow motion.
    f.delta = (float)delta;
    f.frame = (float)(seconds / delta);
    f.targetFps = 60.f;
    f.resX = (float)w;
    f.resY = (float)h;
    f.invX = 1.f / (float)w;
    f.invY = 1.f / (float)h;
    f.mouseNX = 0.5f;
    f.mouseNY = 0.5f;
    f.quality = 3.f; // high, so the preview shows the wallpaper's intended look
    f.pixelRatio = 1.f;
    f.renderScale = 1.f;
    // Everything a package draws in absolute pixels has to shrink with the preview, or a 640x360
    // strip shows snowflakes six times too large for the picture and it is not the same wallpaper.
    f.sizeScale = (designW_ && designH_) ? std::min(float(w) / float(designW_), float(h) / float(designH_))
                                         : 1.f;

    DrawArgs args = inst.MakeArgs(f);
    if (!rend.Render(dev.ctx.Get(), rtv.Get(), w, h, args, error)) return false;
    dev.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> st;
    if (FAILED(dev.dev->CreateTexture2D(&td, nullptr, &st))) {
        error = "staging texture";
        return false;
    }
    dev.ctx->CopyResource(st.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(dev.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp))) {
        error = "map staging";
        return false;
    }
    std::vector<BYTE> buf((size_t)w * h * 4);
    const BYTE* src = static_cast<const BYTE*>(mp.pData);
    for (UINT y = 0; y < h; ++y)
        memcpy(buf.data() + (size_t)y * w * 4, src + (size_t)y * mp.RowPitch, (size_t)w * 4);
    dev.ctx->Unmap(st.Get(), 0);

    const bool wrote = WriteImage(dev.wic.Get(), file, w, h, buf, error, jpegQuality);
    if (pixels) pixels->swap(buf);   // after the write: swapping before it would hand it an empty buffer
    return wrote;
}

namespace {
void MakeDirs(const std::wstring& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec); // CreateDirectoryW does not make intermediate folders
}

// Mean absolute difference per byte over two same-sized BGRA buffers. PNG here is lossless, so a
// wallpaper that does not animate returns exactly 0.
double MeanAbsDiff(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    unsigned long long sum = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int ch = 0; ch < 3; ++ch)
            sum += (a[i + ch] > b[i + ch]) ? (a[i + ch] - b[i + ch]) : (b[i + ch] - a[i + ch]);
    return double(sum) / double((a.size() / 4) * 3);
}
} // namespace

bool Thumbnailer::Build(D3D11Device& dev, D3D11Renderer& rend, WallpaperInstance& inst,
                        const std::wstring& outDir, const std::string& id, ThumbSet& out, std::string& error) {
    out = {};
    MakeDirs(outDir);
    const std::wstring wide = ToWide(id);

    out.still = outDir + L"\\" + wide + L"_still.png";
    out.large = outDir + L"\\" + wide + L"_large.png";
    out.framesDir = outDir + L"\\" + wide + L"_frames";
    // Wiped before the rewrite: a previous build may have left more (or larger-numbered) frames
    // behind, and the catalog only lists what this run kept, so the leftovers would be pure disk.
    std::error_code ec;
    std::filesystem::remove_all(out.framesDir, ec);
    MakeDirs(out.framesDir);

    if (!Grab(dev, rend, inst, stillW_, stillH_, baseTime_, 1.0 / 60.0, out.still, nullptr, error)) {
        out.error = error;
        return false;
    }
    if (!Grab(dev, rend, inst, largeW_, largeH_, baseTime_, 1.0 / 60.0, out.large, nullptr, error)) {
        out.error = error;
        return false;
    }
    const double step = frameSpan_ / double(frameCount_);
    out.frameMs = step * 1000.0;
    std::vector<BYTE> kept, cur;
    for (int i = 0; i < frameCount_; ++i) {
        const double t = baseTime_ + step * i;
        wchar_t name[64];
        swprintf(name, 64, L"\\f%02d.jpg", i);
        const std::wstring path = out.framesDir + name;
        // JPEG at 85: the strip is 24 frames of a 1280x720 picture, and lossless costs 28 MB per
        // wallpaper to store pixels nobody is inspecting one by one.
        if (!Grab(dev, rend, inst, frameW_, frameH_, t, step, path, &cur, error, 85)) {
            out.error = error;
            return false;
        }
        // Compared against the last frame that survived, not the one before it, so a slow drift
        // cannot accumulate into two neighbours that look the same on screen.
        if (i > 0 && MeanAbsDiff(kept, cur) < 0.02) {
            std::filesystem::remove(path, ec);
            continue;
        }
        kept = cur;
        out.frames.push_back(path);
    }
    dev.ctx->Flush();
    out.ok = true;
    Info(MOD, "{}: still {}x{}, large {}x{}, {} of {} motion frames kept at {:.1f} ms", id, stillW_, stillH_,
         largeW_, largeH_, (unsigned)out.frames.size(), frameCount_, out.frameMs);
    return true;
}

} // namespace sw
