#pragma once
// Engine/Packages/WallpaperManager.hpp - config file, package catalog and per-monitor assignments.
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Packages/WallpaperPackage.hpp"
#include "Engine/Core/Json.hpp"

namespace sw {

// How an image wallpaper covers a screen of a different shape. Fill is what the engine has always
// done (scale up, crop what overflows), so it stays the default and nothing changes on its own.
// The numbers are what the shaders see in uPerf.w - Image.hlsl branches on them - so adding a
// mode means adding one here and one case there, in order.
enum class ImageFit { Fill = 0, Fit = 1, Stretch = 2, Center = 3, Tile = 4 };

ImageFit ParseImageFit(std::string_view s, ImageFit def = ImageFit::Fill);
const char* ImageFitName(ImageFit f);

struct Assignment {
    std::string monitor = "all";  // "M0", "\\.\DISPLAY1" or "all"
    std::string wallpaper;
    int fps = 0;                  // 0 = whatever the package asks for
    std::string quality;          // empty = package default
};

struct Settings {
    std::wstring root;            // directory holding Wallpapers/ and Shaders/
    std::wstring configFile;
    std::string logLevel = "info";
    bool tray = true;
    bool ipc = true;
    std::wstring pipeName = L"\\\\.\\pipe\\SmartWallpaper.Renderer";
    int globalMaxFps = 60;
    Quality globalQuality = Quality::High;
    bool useGlobalQuality = false; // when false each package's own quality wins
    ImageFit imageFit = ImageFit::Fill; // how image wallpapers cover the screen
    // 0 = the taskbar keeps Windows' own material; 1..100 = how far past it to fade the bar.
    int trayAlpha = 0;
    std::vector<Assignment> assignments;
    Json power = Json::Object();
};

class WallpaperManager {
public:
    // Loads, or writes a first-run config that points at the first wallpaper it finds.
    bool Load(const std::wstring& exeDir, std::string& error);
    bool Save(std::string& error);

    Settings& settings() { return s_; }
    const Json& raw() const { return raw_; }
    const Settings& settings() const { return s_; }

    std::vector<WallpaperPackage>& catalog() { return catalog_; }
    const WallpaperPackage* find(std::string_view id) const;
    bool Scan(std::vector<std::string>& skipped);

    // Which package this monitor should show, honouring per-monitor then "all" assignments.
    std::string WallpaperForMonitor(const MonitorInfo& m, const std::string& fallback) const;
    int FpsForMonitor(const MonitorInfo& m) const;
    void SetAssignment(const std::string& monitor, const std::string& wallpaperId);
    // Per-monitor frame cap; 0 clears it so the package's own fps wins again.
    void SetFpsFor(const MonitorInfo& m, int fps);
    void SetGlobalQuality(const std::string& q);
    void SetImageFit(const std::string& f);
    void SetTrayAlpha(int pct);
    void SetGlobalMaxFps(int fps);
    void SetRotate(const Json& r);

    Json StatusJson() const;

private:
    bool MatchMonitor(const Assignment& a, const MonitorInfo& m) const;
    Settings s_;
    Json raw_;                       // the config as loaded, so extra sections survive a save
    std::vector<WallpaperPackage> catalog_;
};

} // namespace sw
