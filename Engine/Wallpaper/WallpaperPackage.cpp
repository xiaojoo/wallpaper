#include "Engine/Wallpaper/WallpaperPackage.hpp"
#include "Engine/Core/Log.hpp"

#include <algorithm>
#include <filesystem>

namespace sw {
static constexpr const char* MOD = "wpkg";

Quality ParseQuality(std::string_view s, Quality def) {
    if (s == "battery") return Quality::Battery;
    if (s == "low") return Quality::Low;
    if (s == "medium") return Quality::Medium;
    if (s == "high") return Quality::High;
    if (s == "ultra") return Quality::Ultra;
    return def;
}

const char* QualityName(Quality q) {
    switch (q) {
        case Quality::Battery: return "battery";
        case Quality::Low: return "low";
        case Quality::Medium: return "medium";
        case Quality::High: return "high";
        case Quality::Ultra: return "ultra";
    }
    return "high";
}

// Render scale per quality level: a wallpaper that costs too much on an iGPU drops to the next rung
// instead of dropping frames, which is the difference between "smooth" and "stuttering" on a desktop.
float QualityScale(Quality q) {
    switch (q) {
        case Quality::Battery: return 0.5f;
        case Quality::Low: return 0.65f;
        case Quality::Medium: return 0.8f;
        case Quality::High: return 1.0f;
        case Quality::Ultra: return 1.0f;
    }
    return 1.0f;
}

std::optional<WallpaperPackage> WallpaperPackage::LoadFromDir(const std::wstring& dir, std::string& error) {
    namespace fs = std::filesystem;
    WallpaperPackage p;
    p.dir_ = dir;
    std::wstring manifest = dir + L"\\wallpaper.json";
    if (!fs::exists(manifest)) {
        error = "no wallpaper.json in " + ToUtf8(dir);
        return std::nullopt;
    }
    bool ok = false;
    std::string text = ReadFileUtf8(manifest, ok);
    if (!ok) {
        error = "cannot read " + ToUtf8(manifest);
        return std::nullopt;
    }
    auto root = Json::Parse(text, error);
    if (!root) return std::nullopt;
    const Json& j = *root;
    if (!j.isObject()) {
        error = "wallpaper.json is not an object";
        return std::nullopt;
    }

    std::string folderName = ToUtf8(fs::path(dir).filename().wstring());
    p.id_ = j.strOr("id", folderName);
    p.name_ = j.strOr("name", p.id_);
    p.category_ = j.strOr("category", "未分类");
    p.srcW_ = j.intOr("width", 0);
    p.srcH_ = j.intOr("height", 0);
    const bool wantsParticles = j.strOr("renderer", "") == "particles";
    std::string renderer = j.strOr("renderer", "");
    if (!renderer.empty() && renderer != "d3d11" && renderer != "particles") {
        error = "renderer '" + renderer + "' is not available in this build";
        return std::nullopt;
    }
    p.fps_ = j.find("fps") ? j.find("fps")->asInt(60) : 60;
    p.quality_ = ParseQuality(j.strOr("quality", ""), Quality::High);
    p.additive_ = j.find("blend") ? j.find("blend")->asString() == "additive" : false;
    std::string shaderFile = j.strOr("shader", "main.hlsl");
    p.shaderFile_ = ToWide(shaderFile);
    if (const Json* e = j.find("entries"); e && e->isObject()) {
        p.entries_.vs = e->strOr("vs", p.entries_.vs);
        p.entries_.ps = e->strOr("ps", p.entries_.ps);
        p.entries_.cs = e->strOr("cs", p.entries_.cs);
        p.entries_.bg = e->strOr("bg", p.entries_.bg);
        p.entries_.pvs = e->strOr("pvs", p.entries_.pvs);
    }
    if (const Json* pr = j.find("particles"); pr && pr->isObject()) {
        p.particleCount_ = (UINT)std::max(0, pr->intOr("count", 0));
        if (p.particleCount_) {
            // The particle pipeline is fixed: the engine always draws the background first, so a
            // manifest that names its own entries would silently compile the wrong functions.
            if (p.entries_.cs.empty() || p.entries_.pvs.empty()) {
                error = "particles.count needs entries.cs and entries.pvs to dispatch and expand them";
                return std::nullopt;
            }
        }
    }
    if (const Json* pr = j.find("params"); pr && pr->isObject()) p.params_ = *pr;

    if (const Json* tx = j.find("textures"); tx && tx->isArray()) {
        for (auto& item : tx->items()) {
            TextureRef r;
            if (item.isString()) {
                r.file = ToWide(item.asString());
                r.name = ToUtf8(std::filesystem::path(r.file).filename().wstring());
            } else if (item.isObject()) {
                r.name = item.strOr("name", "");
                r.file = item.find("file") ? ToWide(item.find("file")->asString()) : L"";
            }
            if (r.file.empty()) {
                error = "texture entry without a file";
                return std::nullopt;
            }
            p.textures_.push_back(std::move(r));
        }
    }
    if (p.textures_.size() > 8) p.textures_.resize(8);
    // The renderer string and the particles block both have to agree, and textures are indexed from
    // t1 in a particle wallpaper because the buffer owns t0.
    if (wantsParticles && !p.particleCount_) {
        error = "renderer 'particles' needs a particles.count";
        return std::nullopt;
    }
    if (p.particleCount_ && p.textures_.size() > 7) {
        error = "a particle wallpaper can carry at most 7 textures, t0 belongs to the buffer";
        return std::nullopt;
    }
    return p;
}

std::string WallpaperPackage::summary() const {
    std::string out = std::format("{} ('{}') category={} fps={} quality={} shader={} params={} textures={} blend={}",
                                  id_, name_, category_, fps_, QualityName(quality_), ToUtf8(shaderFile_),
                                  params_.members().size(), textures_.size(), additive_ ? "additive" : "opaque");
    if (particleCount_)
        out += std::format(" particles={} cs={} pvs={} bg={}", particleCount_, entries_.cs, entries_.pvs,
                           entries_.bg);
    return out;
}

std::vector<WallpaperPackage> DiscoverWallpapers(const std::wstring& root, std::string& errorOut,
                                                 std::vector<std::string>& skipped) {
    namespace fs = std::filesystem;
    std::vector<WallpaperPackage> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        errorOut = "not a directory: " + ToUtf8(root);
        return out;
    }
    for (auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        std::string err;
        auto p = WallpaperPackage::LoadFromDir(entry.path().wstring(), err);
        if (p) out.push_back(std::move(*p));
        else {
            skipped.push_back(ToUtf8(entry.path().filename().wstring()) + ": " + err);
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.id() < b.id(); });
    return out;
}

} // namespace sw
