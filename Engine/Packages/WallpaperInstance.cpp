#include "Engine/Packages/WallpaperInstance.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Graphics/FFmpegPlayer.hpp"
#include "Engine/Core/Json.hpp"

#include <iterator>
#include "Engine/Core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace sw {
static constexpr const char* MOD = "winst";

WallpaperInstance::~WallpaperInstance() = default;

void WallpaperInstance::SetMediaActive(bool active) {
    if (video_) video_->SetActive(active);
}

namespace {
// One backend, hardware first. Measured 2026-10-07 on the 4K60 clip that is on this desktop, 10 s
// windows from `--ctl status`: native h264 216% of a core / 556 MB / 54.8 frames/s, h264_cuvid 37% /
// 299 MB / 60.7 frames/s. The Media Foundation decoder that shipped before this cost 290-313% /
// 779-790 MB for the same picture and is gone.
//
// WALLPAPER_VIDEO=sw asks for the native decoder only. It exists because NVDEC is a second piece of
// software we do not control: when the picture goes wrong on a machine with a green colour gate, the
// first question is whether the vendor decoder or our pipeline did it, and that question needs a
// switch, not a rebuild.
bool NativeDecoderOnly() {
    wchar_t env[16]{};
    const DWORD n = GetEnvironmentVariableW(L"WALLPAPER_VIDEO", env, DWORD(std::size(env)));
    return n > 0 && ToUtf8(std::wstring_view(env, n)) == "sw";
}

// Where a package's shader actually lives and what the compiler gets handed. Shared between Prepare
// and the worker-thread warm-up below so the two can never disagree about which file was compiled.
bool ResolveShader(const WallpaperPackage& pkg, const std::wstring& rootDir, std::string& src,
                   std::wstring& fileName, std::vector<std::wstring>& includes, std::string& error) {
    namespace fs = std::filesystem;
    // A decoded frame is two planes, and only Video.hlsl reads them. The two clips imported before
    // that shader existed still carry "Image.hlsl" in their manifests, so the engine decides this one
    // rather than letting a stale manifest sample luma as if it were a whole picture.
    std::wstring shaderFile = pkg.shaderFile();
    if (pkg.isVideo() && shaderFile != L"Video.hlsl") {
        Warn(MOD, "{}: manifest asks for {}, but a video wallpaper is drawn by Video.hlsl", pkg.id(),
             ToUtf8(shaderFile));
        shaderFile = L"Video.hlsl";
    }
    std::wstring shaderPath = pkg.dir() + L"\\" + shaderFile;
    const std::wstring sharedDir = rootDir + L"\\Shaders";
    if (!fs::exists(shaderPath)) {
        const std::wstring alt = sharedDir + L"\\" + shaderFile;
        if (!fs::exists(alt)) {
            error = "shader " + ToUtf8(shaderFile) + " not found in " + ToUtf8(pkg.dir()) + " or Shaders/";
            return false;
        }
        shaderPath = alt;
    }
    bool ok = false;
    src = ReadFileUtf8(shaderPath, ok);
    if (!ok || src.empty()) { error = "cannot read shader " + ToUtf8(shaderPath); return false; }
    includes = { fs::path(shaderPath).parent_path().wstring(), sharedDir, pkg.dir() };
    fileName = fs::path(shaderPath).filename().wstring();
    return true;
}
} // namespace

bool WarmPackageShader(const WallpaperPackage& pkg, const std::wstring& rootDir, std::string& error) {
    std::string src;
    std::wstring fileName;
    std::vector<std::wstring> includes;
    if (!ResolveShader(pkg, rootDir, src, fileName, includes, error)) return false;
    Shader probe;   // Compile needs no device - only CreatePipeline does - so a worker may run it
    return probe.Compile(src, fileName, includes, pkg.entries(), error);
}

std::unique_ptr<VideoSource> OpenPackageMedia(const WallpaperPackage& pkg, std::string& error) {
    if (!pkg.isVideo()) return nullptr;
    auto src = MakeFFmpegPlayer(!NativeDecoderOnly());
    if (!src) { error = pkg.id() + ": no video backend could be created"; return nullptr; }
    const std::wstring path = pkg.dir() + L"\\" + pkg.videoFile();
    std::string verr;
    if (!src->OpenFile(path, pkg.videoLoop(), verr)) {
        error = pkg.id() + ": " + verr;
        Warn(MOD, "{}", error);
        return nullptr;
    }
    return src;
}

bool WallpaperInstance::Prepare(const std::wstring& rootDir, const WallpaperPackage& pkg, D3D11Device& dev,
                                std::string& error, std::unique_ptr<VideoSource> warmMedia) {
    namespace fs = std::filesystem;
    pkg_ = pkg;

    // A video wallpaper puts its decoded frame's two planes in t0 and t1, so it cannot also list
    // textures: the shader would sample the picture it was told to ignore and the fault would show up
    // as a wrong image rather than as an error.
    if (pkg_.isVideo() && !pkg_.textures().empty()) {
        error = pkg_.id() + ": a video wallpaper must not list textures, the video owns t0 and t1";
        return false;
    }

    // A decoded frame is two planes, and only Video.hlsl reads them - the resolution rule lives in
    // ResolveShader, which the worker-thread warm-up calls with the same arguments.
    std::string src;
    std::wstring fileName;
    std::vector<std::wstring> includes;
    if (!ResolveShader(pkg_, rootDir, src, fileName, includes, error)) return false;
    if (!shader_.Compile(src, fileName, includes, pkg_.entries(), error)) return false;
    if (!shader_.CreatePipeline(dev.dev.Get(), error)) return false;
    if (!shader_.ReflectParams(error)) return false;

    paramStore_.assign((shader_.paramBytes() / 4) + 4, 0.f);
    if (shader_.paramBytes()) {
        shader_.FillParams(pkg_.params(), paramStore_.data(), shader_.paramBytes(), unboundParams_);
        for (auto& key : unboundParams_)
            Warn(MOD, "{}: params.{} is in wallpaper.json but not in the shader - ignored", pkg_.id(), key);
    } else if (!pkg_.params().members().empty()) {
        Warn(MOD, "{}: wallpaper.json has {} param(s) but the shader declares no Params cbuffer", pkg_.id(),
             pkg_.params().members().size());
    }

    // The picture's own pixel width, for the fit modes that place it at native size. It comes from
    // the manifest rather than from params, so an image imported before the field existed still
    // centres and tiles correctly. Shaders that do not declare it are the normal case, not a warning.
    if (pkg_.srcWidth() > 0) {
        for (auto& f : shader_.fields())
            if (f.name == "src_w") *(float*)(paramStore_.data() + f.offset / 4) = float(pkg_.srcWidth());
    }

    srvCount_ = 0;
    for (auto& t : pkg_.textures()) {
        std::wstring path = pkg_.dir() + L"\\" + t.file;
        std::string terr;
        if (!t.tex.Load(dev.dev.Get(), dev.wic.Get(), path, terr)) {
            error = std::format("{}: texture '{}' failed - {}", pkg_.id(), t.name, terr);
            return false;
        }
        if (srvCount_ < 8) srvs_[srvCount_++] = t.tex.srv();
        Info(MOD, "{}: texture {} -> t{} ({}x{})", pkg_.id(), t.name, srvCount_ - 1, t.tex.width(), t.tex.height());
    }

    // The decoder runs on its own thread from here on; the frame is picked up in MakeArgs, which every
    // drawing path goes through.
    if (pkg_.isVideo()) {
        std::unique_ptr<VideoSource> player = std::move(warmMedia);
        if (!player) {
            player = OpenPackageMedia(pkg_, error);
            if (!player) return false;
        }
        // The device half either way: whoever opened the file still cannot build the plane textures off
        // the thread that owns the immediate context.
        std::string verr;
        if (!player->Attach(dev.dev.Get(), verr)) {
            error = pkg_.id() + ": " + verr;
            return false;
        }
        video_ = std::move(player);
    }

    if (pkg_.hasParticles()) {
        const UINT wanted = pkg_.particleCount();
        const UINT count = std::min(wanted, ParticleSystem::MaxCount);
        if (count != wanted)
            Warn(MOD, "{}: particles.count {} is over the engine cap, using {}", pkg_.id(), wanted, count);
        if (!particles_.Init(dev.dev.Get(), count, error)) return false;
        particles_.Seed(dev.ctx.Get(), count);

        // The buffer length and the shader's loop bound must be one number, and the engine owns it.
        // A wallpaper that forgot to declare count would dispatch over memory it never wrote.
        bool bound = false;
        for (auto& f : shader_.fields()) {
            if (f.name == "count" || f.name == "ucount" || f.name == "particle_count") {
                *(int*)(paramStore_.data() + f.offset / 4) = (int)count;
                bound = true;
            }
        }
        if (!bound) {
            error = std::format("{}: particles.count is set but the shader's Params cbuffer has no int 'count'",
                                pkg_.id());
            return false;
        }
    }

    Info(MOD, "{} prepared: {} param byte(s), {} texture(s){}{}", pkg_.id(), shader_.paramBytes(), srvCount_,
         pkg_.hasParticles() ? std::format(", {} particles", particles_.count()) : std::string(),
         pkg_.isVideo() ? std::format(", video {} via {}", ToUtf8(pkg_.videoFile()), video_->note()) : std::string());
    return true;
}

DrawArgs WallpaperInstance::MakeArgs(const FrameCB& frame, ID3D11DeviceContext* ctx) {
    DrawArgs a;
    a.frame = frame;
    a.vs = shader_.vs();
    a.ps = shader_.ps();
    if (video_ && ctx) {
        // Until the decoder's first frame lands there is nothing to bind, and the wallpaper draws black
        // for that moment rather than showing the previous wallpaper's texture.
        if (ID3D11ShaderResourceView* y = video_->FrameY(ctx)) {
            srvs_[0] = y;
            srvs_[1] = video_->FrameUV();
            srvCount_ = 2;
        }
    }
    a.srvCount = srvCount_;
    for (UINT i = 0; i < srvCount_; ++i) a.srvs[i] = srvs_[i];
    a.params = paramStore_.data();
    a.paramBytes = shader_.paramBytes();
    a.additive = pkg_.additiveBlend();
    if (pkg_.hasParticles()) {
        a.cs = shader_.cs();
        a.uav = particles_.uav();
        a.particles = particles_.srv();
        a.particleVs = shader_.particleVs();
        a.backgroundPs = shader_.backgroundPs();
        a.particleCount = particles_.count();
        a.threadsPerGroup = ParticleSystem::Threads;
    }
    return a;
}

} // namespace sw
