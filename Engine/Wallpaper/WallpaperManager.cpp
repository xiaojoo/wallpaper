#include "Engine/Wallpaper/WallpaperManager.hpp"
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Core/Log.hpp"

#include <filesystem>

namespace sw {
static constexpr const char* MOD = "wpmgr";

ImageFit ParseImageFit(std::string_view s, ImageFit def) {
    if (s == "fill") return ImageFit::Fill;
    if (s == "fit") return ImageFit::Fit;
    if (s == "stretch") return ImageFit::Stretch;
    if (s == "center") return ImageFit::Center;
    if (s == "tile") return ImageFit::Tile;
    return def;
}

const char* ImageFitName(ImageFit f) {
    switch (f) {
        case ImageFit::Fill: return "fill";
        case ImageFit::Fit: return "fit";
        case ImageFit::Stretch: return "stretch";
        case ImageFit::Center: return "center";
        case ImageFit::Tile: return "tile";
    }
    return "fill";
}

namespace {
// Walk up from the exe until a folder holding Wallpapers/ + Shaders/ is found: an installed layout
// and build/bin/<Config>/ both work then, with no copy step required.
std::wstring ResolveRoot(const std::wstring& exeDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::wstring cur = exeDir;
    for (int depth = 0; depth < 4; ++depth) {
        if (fs::exists(cur + L"\\Wallpapers", ec) && fs::exists(cur + L"\\Shaders", ec)) return cur;
        std::wstring parent = fs::weakly_canonical(fs::path(cur) / L"..", ec).wstring();
        if (parent.empty() || parent == cur) break;
        cur = parent;
    }
    return exeDir;
}
} // namespace

bool WallpaperManager::Load(const std::wstring& exeDir, std::string& error) {
    namespace fs = std::filesystem;
    s_.root = ResolveRoot(exeDir);
    s_.configFile = s_.root + L"\\config.json";

    bool ok = false;
    std::string text = ReadFileUtf8(s_.configFile, ok);
    if (!ok) {
        Info(MOD, "no config at {} - using defaults and writing one", ToUtf8(s_.configFile));
        s_.assignments.push_back(Assignment{}); // wallpaper filled in after Scan()
        catalog_.clear();
        std::vector<std::string> skipped;
        Scan(skipped);
        if (!catalog_.empty()) s_.assignments.front().wallpaper = catalog_.front().id();
        return Save(error);
    }
    auto j = Json::Parse(text, error);
    if (!j) {
        error = "config.json: " + error;
        return false;
    }
    const Json& r = *j;
    raw_ = r;
    s_.logLevel = r.strOr("log_level", "info");
    s_.tray = r.find("tray") ? r.find("tray")->asBool(true) : true;
    s_.ipc = r.find("ipc") ? r.find("ipc")->asBool(true) : true;
    if (r.find("pipe_name")) s_.pipeName = ToWide(r.find("pipe_name")->asString());
    s_.globalMaxFps = r.find("max_fps") ? r.find("max_fps")->asInt(60) : 60;
    s_.useGlobalQuality = r.find("quality") ? r.find("quality")->asString() != "package" : false;
    s_.globalQuality = ParseQuality(r.strOr("quality", "high"), Quality::High);
    s_.imageFit = ParseImageFit(r.strOr("image_fit", "fill"));
    if (const Json* p = r.find("power"); p && p->isObject()) s_.power = *p;
    s_.assignments.clear();
    if (const Json* a = r.find("monitors"); a && a->isArray()) {
        for (auto& item : a->items()) {
            if (!item.isObject()) continue;
            Assignment as;
            as.monitor = item.strOr("monitor", "all");
            as.wallpaper = item.strOr("wallpaper", "");
            as.fps = item.find("fps") ? item.find("fps")->asInt(0) : 0;
            as.quality = item.strOr("quality", "");
            s_.assignments.push_back(std::move(as));
        }
    }
    return true;
}

bool WallpaperManager::Save(std::string& error) {
    Json r = Json::Object();
    r.set("log_level", Json::Of(s_.logLevel));
    r.set("tray", Json::Of(s_.tray));
    r.set("ipc", Json::Of(s_.ipc));
    r.set("pipe_name", Json::Of(ToUtf8(s_.pipeName)));
    r.set("max_fps", Json::Of(s_.globalMaxFps));
    r.set("quality", Json::Of(s_.useGlobalQuality ? QualityName(s_.globalQuality) : std::string("package")));
    r.set("image_fit", Json::Of(std::string(ImageFitName(s_.imageFit))));
    r.set("power", s_.power.isObject() ? s_.power : Json::Object());
    Json mons = Json::Array();
    for (auto& a : s_.assignments) {
        Json m = Json::Object();
        m.set("monitor", Json::Of(a.monitor));
        m.set("wallpaper", Json::Of(a.wallpaper));
        if (a.fps) m.set("fps", Json::Of(a.fps));
        if (!a.quality.empty()) m.set("quality", Json::Of(a.quality));
        mons.push(std::move(m));
    }
    r.set("monitors", std::move(mons));
    if (!raw_.isNull()) {
        for (auto& kv : raw_.members())
            if (kv.first != "_comment" && !r.find(kv.first)) r.set(kv.first, kv.second);
    }
    r.set("_comment",
          Json::Of("quality: package|ultra|high|medium|low|battery. image_fit (image wallpapers only): "
                   "fill|fit|stretch|center|tile. monitors[] may repeat per M0/M1 or use all. "
                   "rotate: {on,interval_min,monitor,pool[]}"));
    std::string text = r.dump(2) + "\n";
    if (!WriteFileUtf8(s_.configFile, text)) {
        error = "cannot write " + ToUtf8(s_.configFile);
        return false;
    }
    Info(MOD, "config written to {}", ToUtf8(s_.configFile));
    return true;
}

bool WallpaperManager::Scan(std::vector<std::string>& skipped) {
    std::string err;
    catalog_ = DiscoverWallpapers(s_.root + L"\\Wallpapers", err, skipped);
    for (auto& s : skipped) Warn(MOD, "skipped wallpaper: {}", s);
    if (catalog_.empty()) Warn(MOD, "no wallpapers found: {}", err);
    for (auto& p : catalog_) Info(MOD, "found {}", p.summary());
    return !catalog_.empty();
}

const WallpaperPackage* WallpaperManager::find(std::string_view id) const {
    for (auto& p : catalog_)
        if (p.id() == id) return &p;
    return nullptr;
}

bool WallpaperManager::MatchMonitor(const Assignment& a, const MonitorInfo& m) const {
    if (a.monitor.empty() || a.monitor == "all" || a.monitor == "*") return true;
    if (a.monitor == m.tag) return true;
    if (a.monitor == ToUtf8(m.device)) return true;
    int idx = -1; // "0" / "1" mean the same order the log and status output use
    for (char c : a.monitor) {
        if (c < '0' || c > '9') return false;
        idx = idx < 0 ? c - '0' : idx * 10 + (c - '0');
    }
    if (m.tag.size() > 1 && m.tag[0] == 'M') {
        int mine = -1;
        for (size_t i = 1; i < m.tag.size(); ++i)
            if (m.tag[i] >= '0' && m.tag[i] <= '9') mine = mine < 0 ? m.tag[i] - '0' : mine * 10 + (m.tag[i] - '0');
        return mine == idx;
    }
    return false;
}

std::string WallpaperManager::WallpaperForMonitor(const MonitorInfo& m, const std::string& fallback) const {
    for (auto& a : s_.assignments)
        if (!a.wallpaper.empty() && MatchMonitor(a, m)) return a.wallpaper;
    for (auto& a : s_.assignments)
        if (!a.wallpaper.empty()) return a.wallpaper;
    return fallback;
}

int WallpaperManager::FpsForMonitor(const MonitorInfo& m) const {
    for (auto& a : s_.assignments)
        if (MatchMonitor(a, m) && a.fps > 0) return a.fps;
    return 0;
}

void WallpaperManager::SetAssignment(const std::string& monitor, const std::string& wallpaperId) {
    for (auto& a : s_.assignments) {
        if (a.monitor == monitor) {
            a.wallpaper = wallpaperId;
            return;
        }
    }
    s_.assignments.push_back(Assignment{monitor, wallpaperId, 0, {}});
}

void WallpaperManager::SetFpsFor(const MonitorInfo& m, int fps) {
    // Only an entry that names this monitor may be edited, otherwise a per-screen cap would land on
    // the "all" entry and change every screen.
    for (auto& a : s_.assignments) {
        if (a.monitor == m.tag || a.monitor == ToUtf8(m.device)) { a.fps = fps < 0 ? 0 : fps; return; }
    }
    s_.assignments.push_back(Assignment{m.tag, "", fps < 0 ? 0 : fps, {}});
}

void WallpaperManager::SetRotate(const Json& r) { raw_.set("rotate", r); }

void WallpaperManager::SetGlobalQuality(const std::string& q) {
    s_.globalQuality = ParseQuality(q, Quality::High);
    s_.useGlobalQuality = q != "package";
}

void WallpaperManager::SetGlobalMaxFps(int fps) { s_.globalMaxFps = fps < 0 ? 0 : fps; }

void WallpaperManager::SetImageFit(const std::string& f) { s_.imageFit = ParseImageFit(f, s_.imageFit); }

Json WallpaperManager::StatusJson() const {
    Json r = Json::Object();
    r.set("root", Json::Of(ToUtf8(s_.root)));
    r.set("max_fps", Json::Of(s_.globalMaxFps));
    r.set("quality", Json::Of(std::string(QualityName(s_.globalQuality))));
    r.set("quality_is_global", Json::Of(s_.useGlobalQuality));
    r.set("image_fit", Json::Of(std::string(ImageFitName(s_.imageFit))));
    Json mons = Json::Array();
    for (auto& a : s_.assignments) {
        Json m = Json::Object();
        m.set("monitor", Json::Of(a.monitor));
        m.set("wallpaper", Json::Of(a.wallpaper));
        m.set("fps", Json::Of(a.fps));
        mons.push(std::move(m));
    }
    r.set("assignments", std::move(mons));
    Json cat = Json::Array();
    for (auto& p : catalog_) {
        Json m = Json::Object();
        m.set("id", Json::Of(p.id()));
        m.set("name", Json::Of(p.name()));
        m.set("fps", Json::Of(p.baseFps()));
        m.set("quality", Json::Of(std::string(QualityName(p.defaultQuality()))));
        cat.push(std::move(m));
    }
    r.set("catalog", std::move(cat));
    return r;
}

} // namespace sw
