#include "Engine/Packages/WallpaperManager.hpp"
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Core/Log.hpp"

#include <filesystem>
#include <mutex>
#include <algorithm>

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
    s_.trayAlpha = r.find("tray_alpha") ? r.find("tray_alpha")->asInt(0) : 0;
    s_.hud = r.find("hud") ? r.find("hud")->asBool(false) : false;
    s_.hudCorner = r.find("hud_corner") ? ParseHudCorner(r.find("hud_corner")->asString(), 0) : 0;
    // A dragged position, if there is one. Anything malformed leaves both negative, which is what
    // "no dragged position" means and puts the card back on its corner.
    if (const Json* p = r.find("hud_pos"); p && p->isArray() && p->items().size() == 2) {
        const double x = p->items()[0].asNumber(-1.0), y = p->items()[1].asNumber(-1.0);
        if (x >= 0.0 && y >= 0.0) { s_.hudX = float(x); s_.hudY = float(y); }
    }
    s_.hudScalePct = r.find("hud_scale") ? r.find("hud_scale")->asInt(100) : 100;
    if (s_.hudScalePct < 60) s_.hudScalePct = 60;
    if (s_.hudScalePct > 250) s_.hudScalePct = 250;
    s_.hudFrame = r.find("hud_frame") ? ParseHudFrame(r.find("hud_frame")->asString(), 0) : 0;
    s_.chart = r.find("chart") ? r.find("chart")->asBool(false) : false;
    s_.chartType = r.find("chart_type") ? ParseChartType(r.find("chart_type")->asString(), 0) : 0;
    s_.chartCorner = r.find("chart_corner") ? ParseHudCorner(r.find("chart_corner")->asString(), 3) : 3;
    if (const Json* p = r.find("chart_pos"); p && p->isArray() && p->items().size() == 2) {
        const double x = p->items()[0].asNumber(-1.0), y = p->items()[1].asNumber(-1.0);
        if (x >= 0.0 && y >= 0.0) { s_.chartX = float(x); s_.chartY = float(y); }
    }
    s_.chartScalePct = r.find("chart_scale") ? r.find("chart_scale")->asInt(100) : 100;
    s_.chartScalePct = std::clamp(s_.chartScalePct, 60, 250);
    s_.chartFrame = r.find("chart_frame") ? ParseHudFrame(r.find("chart_frame")->asString(), 0) : 0;
    if (s_.trayAlpha < 0) s_.trayAlpha = 0;
    if (s_.trayAlpha > 100) s_.trayAlpha = 100;
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

bool WallpaperManager::Save(std::string& error) { return WriteConfigText(ConfigText(), error); }

std::string WallpaperManager::ConfigText() const {
    Json r = Json::Object();
    r.set("log_level", Json::Of(s_.logLevel));
    r.set("tray", Json::Of(s_.tray));
    r.set("ipc", Json::Of(s_.ipc));
    r.set("pipe_name", Json::Of(ToUtf8(s_.pipeName)));
    r.set("max_fps", Json::Of(s_.globalMaxFps));
    r.set("quality", Json::Of(s_.useGlobalQuality ? QualityName(s_.globalQuality) : std::string("package")));
    r.set("image_fit", Json::Of(std::string(ImageFitName(s_.imageFit))));
    r.set("tray_alpha", Json::Of(s_.trayAlpha));
    r.set("hud", Json::Of(s_.hud));
    r.set("hud_corner", Json::Of(std::string(HudCornerName(s_.hudCorner))));
    if (s_.hudX >= 0.f && s_.hudY >= 0.f) {
        Json pos = Json::Array();
        pos.push(Json::Of(double(s_.hudX)));
        pos.push(Json::Of(double(s_.hudY)));
        r.set("hud_pos", std::move(pos));
    }
    r.set("hud_scale", Json::Of(s_.hudScalePct));
    r.set("hud_frame", Json::Of(std::string(HudFrameName(s_.hudFrame))));
    r.set("chart", Json::Of(s_.chart));
    r.set("chart_type", Json::Of(std::string(ChartTypeName(s_.chartType))));
    r.set("chart_corner", Json::Of(std::string(HudCornerName(s_.chartCorner))));
    if (s_.chartX >= 0.f && s_.chartY >= 0.f) {
        Json pos = Json::Array();
        pos.push(Json::Of(double(s_.chartX)));
        pos.push(Json::Of(double(s_.chartY)));
        r.set("chart_pos", std::move(pos));
    }
    r.set("chart_scale", Json::Of(s_.chartScalePct));
    r.set("chart_frame", Json::Of(std::string(HudFrameName(s_.chartFrame))));
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
                   "rotate: {on,interval_min,monitor,pool[]}. hud: true|false, hud_corner: "
                   "top-right|top-left|bottom-right|bottom-left, hud_pos: [x,y] in screen heights from "
                   "the monitor's top-left - where the card was dragged to, and it wins over "
                   "hud_corner until a corner is picked again."));
    return r.dump(2) + "\n";
}

// The file write on its own, so the thread that presents the desktop can serialise its own state (a
// microsecond) and hand the disk - measured 8-43 ms for this file on this machine - to a worker. The
// mutex is what keeps a deferred write and an inline `Save` from another command landing on the same
// file at the same time.
bool WallpaperManager::WriteConfigText(const std::string& text, std::string& error) {
    static std::mutex writeMtx;
    std::lock_guard lk(writeMtx);
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

void WallpaperManager::SetTrayAlpha(int pct) {
    s_.trayAlpha = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

void WallpaperManager::SetHud(bool on) { s_.hud = on; }

int WallpaperManager::SetHudCorner(const std::string& name) {
    s_.hudCorner = ParseHudCorner(name, s_.hudCorner);
    s_.hudX = s_.hudY = -1.f;      // a corner and a dragged position cannot both be the answer
    return s_.hudCorner;
}

void WallpaperManager::SetHudPos(float x, float y) { s_.hudX = x, s_.hudY = y; }

void WallpaperManager::ClearHudPos() { s_.hudX = s_.hudY = -1.f; }

void WallpaperManager::SetHudScale(int pct) {
    s_.hudScalePct = pct < 60 ? 60 : (pct > 250 ? 250 : pct);
}

const char* HudCornerName(int index) {
    static const char* const kNames[] = {"top-right", "top-left", "bottom-right", "bottom-left"};
    return kNames[index >= 0 && index <= 3 ? index : 0];
}

// By name, not by index, in config.json and on the pipe: a hand-edited "1" would silently mean
// top-left to nobody who had not read this file first.
int ParseHudCorner(std::string_view s, int def) {
    for (int i = 0; i <= 3; ++i)
        if (s == HudCornerName(i)) return i;
    return def;
}

int WallpaperManager::SetHudFrame(const std::string& name) {
    s_.hudFrame = ParseHudFrame(name, s_.hudFrame);
    return s_.hudFrame;
}

const char* HudFrameName(int index) {
    // Each name describes one measured frame: tools/extract-frames.py reads them off the reference
    // sheets, and index 0 is the status card's own reference. They replaced an earlier set that all
    // drew the same silhouette, which is why the labels changed with them - and why a stored name
    // this list does not recognise falls back to 0 rather than to whatever index it lands on.
    static const char* const kNames[] = {
        "notch",     // the card's reference: a left notch, a hollow bar on top, a heavy right edge
        "basetab",   // a wide tab sunk into the bottom line, ticks and a bar along the top right
        "ticks",     // ticks at both ends of the top line, a tab on the left, a block bottom right
        "topbar",    // a heavy band across the top left, tabs on both short edges
        "band",      // a band across the top right, a block below it on the right
        "split",     // a header strip drawn as its own part above the body
    };
    return kNames[index >= 0 && index < HudFrameCount() ? index : 0];
}

int HudFrameCount() { return 6; }

int ParseHudFrame(std::string_view s, int def) {
    for (int i = 0; i < HudFrameCount(); ++i)
        if (s == HudFrameName(i)) return i;
    return def;
}

void WallpaperManager::SetChart(bool on) { s_.chart = on; }

int WallpaperManager::SetChartType(const std::string& name) {
    s_.chartType = ParseChartType(name, s_.chartType);
    return s_.chartType;
}

int WallpaperManager::SetChartCorner(const std::string& name) {
    s_.chartCorner = ParseHudCorner(name, s_.chartCorner);
    s_.chartX = s_.chartY = -1.f;
    return s_.chartCorner;
}

void WallpaperManager::SetChartPos(float x, float y) { s_.chartX = x, s_.chartY = y; }

void WallpaperManager::ClearChartPos() { s_.chartX = s_.chartY = -1.f; }

void WallpaperManager::SetChartScale(int pct) {
    s_.chartScalePct = pct < 60 ? 60 : (pct > 250 ? 250 : pct);
}

int WallpaperManager::SetChartFrame(const std::string& name) {
    s_.chartFrame = ParseHudFrame(name, s_.chartFrame);
    return s_.chartFrame;
}

const char* ChartTypeName(int index) {
    // The three shapes he sent references for. "donut" is four concentric rings rather than one ring
    // split into slices: a slice would mean "this metric's share of the sum of four percentages",
    // which is a number nobody can check, while a ring sweeping its own value is the same reading the
    // card prints.
    static const char* const kNames[] = { "bar", "donut", "line" };
    return kNames[index >= 0 && index < ChartTypeCount() ? index : 0];
}

int ChartTypeCount() { return 3; }

int ParseChartType(std::string_view s, int def) {
    for (int i = 0; i < ChartTypeCount(); ++i)
        if (s == ChartTypeName(i)) return i;
    return def;
}

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
