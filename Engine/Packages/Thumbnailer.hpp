#pragma once
// Engine/Packages/Thumbnailer.hpp - renders a wallpaper offscreen to PNG files for the settings
// window: a card still, a large preview, and a short frame strip for the motion preview.
#include "Engine/Core/Platform.hpp"
#include <functional>
#include <string>
#include <vector>

namespace sw {

class D3D11Device;
class D3D11Renderer;
class WallpaperInstance;

struct ThumbSet {
    std::wstring still;      // card image
    std::wstring large;      // modal image
    std::wstring framesDir;
    std::vector<std::wstring> frames;
    double frameMs = 0;      // how long each kept frame is on screen when the strip is played back
    bool ok = false;
    std::string error;
};

class Thumbnailer {
public:
    // 16:9 by default because every display in this product is a screen.
    bool Build(D3D11Device& dev, D3D11Renderer& rend, WallpaperInstance& inst, const std::wstring& outDir,
               const std::string& id, ThumbSet& out, std::string& error);

    // The picture these files must match: a preview drawn at 640x360 has to show the same flake
    // *proportions* as the desktop at 3840x2160, or it is a different wallpaper.
    void setDesignSize(UINT w, UINT h) { designW_ = w; designH_ = h; }

    // True when `inst` passed to Build() is the instance a monitor is *currently showing*. Then a
    // video must not be asked to jump to baseTime_: ShowFrameAt parks that player on the frame it
    // grabbed, and nothing but a later seek clears it - which is how the desktop ended up frozen on a
    // still picture at every start (measured 2026-10-07: delivered stuck at 84, thread parked at 0%
    // CPU, one `--ctl video seek` brought it back). The frame already on screen is representative of
    // a clip anyway, and taking it costs no decoder. Without this flag a video card needs its own
    // throwaway instance, and a throwaway 4K decoder cost ~900 MB of surface pool
    // for the two or three seconds the grab takes.
    void setLiveSource(bool live) { liveSource_ = live; }

    // Called while a video grab waits for the decoder to reach the requested moment. For a clip whose
    // previous keyframe is far back that is several hundred milliseconds (measured 2026-10-08: 521 ms
    // for a 3440x1440 and 699 ms for a 4K card), and the only caller is the thread that presents the
    // desktop - so without this hook the pass is a freeze, which is the 3.78 s stall BACKLOG records for
    // the old 26-grab version. Application passes a lambda that draws what is due.
    void setIdleHook(std::function<void()> idle) { idle_ = std::move(idle); }

    UINT stillWidth() const { return stillW_; }
    UINT stillHeight() const { return stillH_; }

private:
    bool Grab(D3D11Device& dev, D3D11Renderer& rend, WallpaperInstance& inst, UINT w, UINT h, double seconds,
              double delta, const std::wstring& file, std::vector<BYTE>* pixels, std::string& error,
              int jpegQuality = 0, UINT supersample = 1);

    UINT stillW_ = 480, stillH_ = 270;
    UINT largeW_ = 1280, largeH_ = 720;
    // The strip is what the big picture shows, and the big picture is about 1260 px wide on screen.
    // Rendering it smaller than that does not just soften it: a package that sizes anything in
    // absolute pixels loses its soft halos (snowfall's flakes went from "snow" to "sparse dots" at
    // 640x360 even with uPerf.z compensating the radius).
    UINT frameW_ = 1280, frameH_ = 720;
    // 8 samples of a 1 s window played back one after another is literally 8 fps, which is what
    // read as stuttering in the motion preview; 24 is the film rate and still costs one frame per
    // 42 ms of decode. Frames that turn out identical are dropped again below.
    int frameCount_ = 24;
    double frameSpan_ = 1.0;   // seconds of shader time covered by the strip
    double baseTime_ = 2.0;    // past the fade-in of most shaders, so the still is representative
    bool liveSource_ = false;  // see setLiveSource
    std::function<void()> idle_;   // see setIdleHook
    UINT designW_ = 0, designH_ = 0;   // the monitor these previews have to look like
};

} // namespace sw
