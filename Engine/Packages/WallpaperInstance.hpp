#pragma once
// Engine/Packages/WallpaperInstance.hpp - a package compiled and ready to draw on one monitor.
#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Graphics/ParticleSystem.hpp"
#include "Engine/Packages/WallpaperPackage.hpp"
#include <memory>

namespace sw {

class D3D11Device;
class VideoPlayer;

class WallpaperInstance {
public:
    WallpaperInstance() = default;
    // Out of line because the video player is only forward declared here, and a Slot (which owns an
    // instance) is destroyed in Application.cpp.
    ~WallpaperInstance();
    WallpaperInstance(WallpaperInstance&&) = default;
    WallpaperInstance& operator=(WallpaperInstance&&) = default;

    // Compiles the package's HLSL against the shared Shaders directory.
    bool Prepare(const std::wstring& rootDir, const WallpaperPackage& pkg, D3D11Device& dev, std::string& error);

    const std::string& id() const { return pkg_.id(); }
    const WallpaperPackage& package() const { return pkg_; }
    const Shader& shader() const { return shader_; }

    // Fills the per-frame constants and returns what D3D11Renderer::Draw needs. A video wallpaper
    // pulls its newest decoded frame here, which is immediate-context work, so the context has to
    // come in: this is the one place every drawing path passes through.
    DrawArgs MakeArgs(const FrameCB& frame, ID3D11DeviceContext* ctx);

    UINT paramBytes() const { return shader_.paramBytes(); }

    // The self test reads the buffer back through these to prove the simulation is advancing.
    bool hasParticles() const { return pkg_.hasParticles(); }
    ParticleSystem& particles() { return particles_; }

    bool hasVideo() const { return video_ != nullptr; }
    VideoPlayer* video() { return video_.get(); }
    // Idle the decoder while this wallpaper is not being drawn (paused, or covered by a fullscreen app).
    void SetMediaActive(bool active);

private:
    WallpaperPackage pkg_;
    Shader shader_;
    ParticleSystem particles_;
    std::unique_ptr<VideoPlayer> video_;
    std::vector<float> paramStore_;
    ID3D11ShaderResourceView* srvs_[8] = {};
    UINT srvCount_ = 0;
    std::vector<std::string> unboundParams_;
};

} // namespace sw
