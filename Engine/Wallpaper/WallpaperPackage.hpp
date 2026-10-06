#pragma once
// Engine/Wallpaper/WallpaperPackage.hpp - a wallpaper on disk: manifest + HLSL + textures.
#include "Engine/Core/Json.hpp"
#include "Engine/Graphics/Shader.hpp"
#include "Engine/Graphics/Texture.hpp"

namespace sw {

enum class Quality { Battery = 0, Low = 1, Medium = 2, High = 3, Ultra = 4 };

Quality ParseQuality(std::string_view s, Quality def = Quality::High);
const char* QualityName(Quality q);
// Percentage of native resolution that this quality level renders at.
float QualityScale(Quality q);

struct TextureRef {
    std::string name;   // shader register follows list order: index 0 -> t0
    std::wstring file;
    Texture tex;
};

class WallpaperPackage {
public:
    // Reads <dir>/wallpaper.json. No compilation happens here; that is Prepare().
    static std::optional<WallpaperPackage> LoadFromDir(const std::wstring& dir, std::string& error);

    const std::string& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::wstring& dir() const { return dir_; }
    const std::wstring& shaderFile() const { return shaderFile_; }
    const Shader::Entries& entries() const { return entries_; }
    // The engine writes the count into the shader's Params.count and allocates the buffer with it,
    // so a manifest can never disagree with what the GPU actually iterates over.
    bool hasParticles() const { return particleCount_ > 0; }
    UINT particleCount() const { return particleCount_; }
    int baseFps() const { return fps_; }
    Quality defaultQuality() const { return quality_; }
    bool additiveBlend() const { return additive_; }
    const std::string& category() const { return category_; }
    // Source resolution of an image wallpaper; 0 for generated shaders, which have none.
    int srcWidth() const { return srcW_; }
    int srcHeight() const { return srcH_; }
    const Json& params() const { return params_; }
    std::vector<TextureRef>& textures() { return textures_; }
    const std::vector<TextureRef>& textures() const { return textures_; }
    std::string summary() const;

private:
    std::string id_, name_, category_;
    int srcW_ = 0, srcH_ = 0;
    std::wstring dir_, shaderFile_;
    Shader::Entries entries_;
    UINT particleCount_ = 0;
    int fps_ = 60;
    Quality quality_ = Quality::High;
    bool additive_ = false;
    Json params_ = Json::Object();
    std::vector<TextureRef> textures_;
};

// Directory scan for the wallpaper browser.
std::vector<WallpaperPackage> DiscoverWallpapers(const std::wstring& root, std::string& errorOut,
                                                 std::vector<std::string>& skipped);

} // namespace sw
