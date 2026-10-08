#include "Engine/Packages/Thumbnailer.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Graphics/Texture.hpp"
#include "Engine/Graphics/VideoSource.hpp"
#include "Engine/Packages/WallpaperInstance.hpp"
#include "Engine/Core/Log.hpp"

#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace sw {
static constexpr const char* MOD = "thumb";

namespace {
// The card is an eighth of the desktop in each axis and the large preview a third, so these are the
// factors that bring each render exactly up to the monitor. Grab clamps them on a smaller screen.
constexpr UINT kCardSupersample = 8, kLargeSupersample = 3;
} // namespace

bool Thumbnailer::Grab(D3D11Device& dev, D3D11Renderer& rend, WallpaperInstance& inst, UINT w, UINT h,
                       double seconds, double delta, const std::wstring& file, std::vector<BYTE>* pixels,
                       std::string& error, int jpegQuality, UINT supersample) {
    // A card is an eighth of the desktop in each axis. A package that draws one-pixel hairlines
    // cannot survive that by being sampled once per output pixel - each hairline either lands or
    // does not, which is what reads as TV static on the browse page. Rendering the same picture at
    // the desktop size and averaging it down lets a hairline contribute the fraction of the card
    // pixel it really covers. The factor is clamped so the render never exceeds the monitor.
    UINT k = 1;
    if (supersample > 1 && designW_ >= w && designH_ >= h)
        k = std::max<UINT>(1, std::min({ supersample, designW_ / w, designH_ / h }));
    const UINT rw = w * k, rh = h * k;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = rw;
    td.Height = rh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    Com<ID3D11Texture2D> rt;
    if (FAILED(dev.dev->CreateTexture2D(&td, nullptr, &rt))) {
        error = "offscreen texture " + std::to_string(rw) + "x" + std::to_string(rh);
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
    f.resX = (float)rw;
    f.resY = (float)rh;
    f.invX = 1.f / (float)rw;
    f.invY = 1.f / (float)rh;
    f.mouseNX = 0.5f;
    f.mouseNY = 0.5f;
    f.quality = 3.f; // high, so the preview shows the wallpaper's intended look
    f.pixelRatio = 1.f;
    f.renderScale = 1.f;
    // Everything a package draws in absolute pixels has to shrink with the preview, or a 640x360
    // strip shows snowflakes six times too large for the picture and it is not the same wallpaper.
    // This is the *render* surface's fraction of the monitor, so supersampling leaves the drawn
    // proportions alone and only adds samples.
    f.sizeScale = (designW_ && designH_) ? std::min(float(rw) / float(designW_), float(rh) / float(designH_))
                                         : 1.f;

    // A video wallpaper has to be sampled *at* these times, not at whatever the decoder happened to
    // be playing: the clip runs on its own wall clock and the strip would otherwise be 24 frames from
    // a stretch of a second that has nothing to do with baseTime_.
    //
    // Not when this is the instance on screen. Asking it to park is what froze the desktop on a still
    // picture at every start, and taking the frame it is already showing costs no decoder at all - a
    // throwaway 4K player cost ~900 MB of decoder surface pool for the few seconds of the grab.
    if (inst.hasVideo() && !liveSource_) {
        const bool parked = inst.video()->ShowFrameAt(seconds, dev.ctx.Get(), idle_);
        Info(MOD, "grab at {:.2f} s: ShowFrameAt {}, {} frames delivered so far", seconds,
             parked ? "found a frame" : "TIMED OUT", inst.video()->delivered());
    } else if (inst.hasVideo()) {
        // This runs at startup too, before the player has produced anything - and an empty grab would
        // be saved as a black card that then survives every reload (only restarting the renderer
        // rebuilds thumbnails). Wait briefly for the first picture instead; if none arrives, the
        // frame that gets drawn is whatever the instance holds, and the card is rebuilt next restart.
        const ULONGLONG until = GetTickCount64() + 1500;
        while (inst.video()->delivered() == 0 && GetTickCount64() < until) Sleep(10);
        Info(MOD, "grab at {:.2f} s: live source, taking frame {} as it plays", seconds,
             inst.video()->delivered());
    }

    DrawArgs args = inst.MakeArgs(f, dev.ctx.Get());
    if (!rend.Render(dev.ctx.Get(), rtv.Get(), rw, rh, args, error)) return false;
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
    std::vector<BYTE> big((size_t)rw * rh * 4);
    const BYTE* src = static_cast<const BYTE*>(mp.pData);
    for (UINT y = 0; y < rh; ++y)
        memcpy(big.data() + (size_t)y * rw * 4, src + (size_t)y * mp.RowPitch, (size_t)rw * 4);
    dev.ctx->Unmap(st.Get(), 0);

    std::vector<BYTE> buf((size_t)w * h * 4);
    if (k > 1) {
        // A plain box average: the point is only that a sub-pixel hairline must contribute what it
        // covers instead of being rounded to on or off.
        const unsigned area = k * k;
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                unsigned b = 0, g = 0, r = 0;
                for (UINT dy = 0; dy < k; ++dy) {
                    const BYTE* px = big.data() + ((size_t)(y * k + dy) * rw + x * k) * 4;
                    for (UINT dx = 0; dx < k; ++dx) {
                        b += px[dx * 4];
                        g += px[dx * 4 + 1];
                        r += px[dx * 4 + 2];
                    }
                }
                BYTE* o = buf.data() + ((size_t)y * w + x) * 4;
                o[0] = (BYTE)(b / area);
                o[1] = (BYTE)(g / area);
                o[2] = (BYTE)(r / area);
                o[3] = 255;
            }
        }
    } else {
        buf.swap(big);
    }

    // A video card with no picture in it must never reach the disk. An un-uploaded plane pair reads as
    // zero, and the shader turns that into R0 G76 B0 - the green card that survived every reload
    // because only a restart rebuilds thumbnails. Uniform output is the signature (green and black
    // both), so refuse and let the previous card stand; scoped to video because a solid-colour image
    // wallpaper is legitimately uniform and should still get its card.
    if (inst.hasVideo()) {
        unsigned lo = 255, hi = 0;
        for (size_t i = 0; i < buf.size(); i += 4) {
            const unsigned l = (unsigned(buf[i]) * 77 + unsigned(buf[i + 1]) * 150
                                + unsigned(buf[i + 2]) * 29) >> 8;
            if (l < lo) lo = unsigned(l);
            if (l > hi) hi = unsigned(l);
        }
        if (hi - lo < 6) {
            error = "the grabbed video frame is uniform (luma " + std::to_string(lo) + ".."
                    + std::to_string(hi) + ") - no card written";
            Warn(MOD, "{}", error);
            return false;
        }
    }

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
    const auto t0 = std::chrono::steady_clock::now();
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

    if (!Grab(dev, rend, inst, stillW_, stillH_, baseTime_, 1.0 / 60.0, out.still, nullptr, error, 0, kCardSupersample)) {
        out.error = error;
        return false;
    }
    if (!Grab(dev, rend, inst, largeW_, largeH_, baseTime_, 1.0 / 60.0, out.large, nullptr, error, 0,
              kLargeSupersample)) {
        out.error = error;
        return false;
    }
    const double step = frameSpan_ / double(frameCount_);
    out.frameMs = step * 1000.0;
    std::vector<BYTE> kept, cur;
    // A video gets a still and the large picture, not a 24-frame strip. Two measured reasons: every
    // grab is a seek plus a full-size decode on the loop thread, which froze the desktop for 3.78 s on
    // a 4K import; and 19 of the 24 came back as the same picture (the decoder lands on the previous
    // keyframe, so the dedup dropped them). The motion the card used to show is available live instead.
    const int framesToGrab = inst.hasVideo() ? 0 : frameCount_;
    if (inst.hasVideo()) out.frameMs = 0.0;
    for (int i = 0; i < framesToGrab; ++i) {
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
    const long long builtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0).count();
    // Two different sentences, because one number line was read as a failure twice: "0 of 24 kept"
    // looks like a strip that failed to build, when for a video there is no strip to build and no
    // grab was ever attempted.
    if (inst.hasVideo())
        Info(MOD, "{}: still {}x{}, large {}x{}, no motion strip (video cards play live, by design) - built in {} ms",
             id, stillW_, stillH_, largeW_, largeH_, builtMs);
    else
        Info(MOD, "{}: still {}x{}, large {}x{}, {} of {} motion frames kept at {:.1f} ms - built in {} ms",
             id, stillW_, stillH_, largeW_, largeH_, (unsigned)out.frames.size(), frameCount_, out.frameMs, builtMs);
    return true;
}

} // namespace sw
