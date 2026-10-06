#pragma once
// Engine/Wallpaper/WallpaperInstance.hpp - a package compiled and ready to draw on one monitor.
#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Graphics/ParticleSystem.hpp"
#include "Engine/Wallpaper/WallpaperPackage.hpp"

namespace sw {

class D3D11Device;

class WallpaperInstance {
public:
    // Compiles the package's HLSL against the shared Shaders directory.
    bool Prepare(const std::wstring& rootDir, const WallpaperPackage& pkg, D3D11Device& dev, std::string& error);

    const std::string& id() const { return pkg_.id(); }
    const WallpaperPackage& package() const { return pkg_; }
    const Shader& shader() const { return shader_; }

    // Fills the per-frame constants and returns what D3D11Renderer::Draw needs.
    DrawArgs MakeArgs(const FrameCB& frame);

    UINT paramBytes() const { return shader_.paramBytes(); }

    // The self test reads the buffer back through these to prove the simulation is advancing.
    bool hasParticles() const { return pkg_.hasParticles(); }
    ParticleSystem& particles() { return particles_; }

private:
    WallpaperPackage pkg_;
    Shader shader_;
    ParticleSystem particles_;
    std::vector<float> paramStore_;
    ID3D11ShaderResourceView* srvs_[8] = {};
    UINT srvCount_ = 0;
    std::vector<std::string> unboundParams_;
};

} // namespace sw
