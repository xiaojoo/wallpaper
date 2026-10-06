#include "Engine/Wallpaper/WallpaperInstance.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace sw {
static constexpr const char* MOD = "winst";

bool WallpaperInstance::Prepare(const std::wstring& rootDir, const WallpaperPackage& pkg, D3D11Device& dev,
                                std::string& error) {
    namespace fs = std::filesystem;
    pkg_ = pkg;

    std::wstring shaderPath = pkg_.dir() + L"\\" + pkg_.shaderFile();
    std::wstring sharedDir = rootDir + L"\\Shaders";
    if (!fs::exists(shaderPath)) {
        std::wstring alt = sharedDir + L"\\" + pkg_.shaderFile();
        if (!fs::exists(alt)) {
            error = "shader " + ToUtf8(pkg_.shaderFile()) + " not found in " + ToUtf8(pkg_.dir()) + " or Shaders/";
            return false;
        }
        shaderPath = alt;
    }

    bool ok = false;
    std::string src = ReadFileUtf8(shaderPath, ok);
    if (!ok || src.empty()) {
        error = "cannot read shader " + ToUtf8(shaderPath);
        return false;
    }

    std::vector<std::wstring> includes;
    includes.push_back(fs::path(shaderPath).parent_path().wstring());
    includes.push_back(sharedDir);
    includes.push_back(pkg_.dir());

    std::wstring fileName = fs::path(shaderPath).filename().wstring();
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

    Info(MOD, "{} prepared: {} param byte(s), {} texture(s){}", pkg_.id(), shader_.paramBytes(), srvCount_,
         pkg_.hasParticles() ? std::format(", {} particles", particles_.count()) : std::string());
    return true;
}

DrawArgs WallpaperInstance::MakeArgs(const FrameCB& frame) {
    DrawArgs a;
    a.frame = frame;
    a.vs = shader_.vs();
    a.ps = shader_.ps();
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
