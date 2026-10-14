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
    // The engine's own status card, composited over whichever wallpaper is on this monitor. It is a
    // renderer setting rather than a package one because that is the whole point of it: the readout
    // describes the machine, and the machine is the same behind a video, a shader or a photograph.
    bool hud = false;
    int hudCorner = 0;            // index into HudCornerName(): 0 top-right, 1 top-left,
                                  // 2 bottom-right, 3 bottom-left
    // Where the user dragged the card instead. Both are in the same units the card is laid out in -
    // screen heights from the monitor's top-left, the card's own top-left corner - and both are
    // negative until a drag happens, because "no free position" has to survive a config that only
    // ever names a corner.
    float hudX = -1.f, hudY = -1.f;
    int hudScalePct = 100;        // the card's height, as a percentage of its reference size
    int hudFrame = 0;           // which of Shaders/HudFrames.hlsl's shapes draws its border
    // The chart panel, which is a second overlay rather than another row on the card: it needs a
    // different shape to be readable (a ring wants a square, a history wants width), and switching it
    // on must not move anything the user positioned on the card.
    bool chart = false;
    int chartType = 0;            // 0 bars, 1 donut rings, 2 line - see ChartTypeName()
    // Same four corners and the same indices as hudCorner, but it defaults to the opposite one:
    // both panels starting in the top-right means switching the chart on hides the card.
    int chartCorner = 3;
    float chartX = -1.f, chartY = -1.f;
    int chartScalePct = 100;
    int chartFrame = 0;
    // The shortcut ring's centre, in the same units as hudX/hudY: monitor heights from this monitor's
    // top-left, both negative until the user drags it. Negative is not "the middle of the screen" -
    // the middle is computed from the work area and is what a config that never mentions a position
    // gets - it is "nobody has said where", which is the difference that lets a wallpaper change
    // re-centre the ring instead of leaving it where it happened to land.
    float orbitX = -1.f, orbitY = -1.f;
    // The ring's two sizes, on the same terms as `hudScalePct`: a percentage of the layout the ring
    // was measured at, so 100 is what the reference gives and the number the slider shows.
    // `orbitScalePct` moves both ellipse radii together - a ring that only got wider would stop being
    // the same perspective view - and `orbitIconPct` sizes one icon at the front of the ring.
    int orbitScalePct = 100;
    int orbitIconPct = 100;
    // How many concentric rings the shortcuts are spread over, and which path each of them walks.
    // Both are manual: the count of rings and the count per ring (below) are the two halves of one
    // decision, and an automatic split would have to guess which of the two he cares about.
    // The shape applies to every ring at once - a set of rings that each walked a different path
    // would be four pictures on the desktop rather than one layout.
    int orbitRings = 1;
    int orbitShape = 0;
    // Which shortcuts the ring may paint, as UTF-8 display names. `orbitPicked` is the difference
    // between "nobody chose" - the ring takes the whole desktop, which is what it did before there was
    // a picker - and "the user unchecked everything", which is an empty ring. Both are an empty list
    // and only one of them should survive a restart as itself.
    std::vector<std::string> orbitPicks;
    bool orbitPicked = false;
    std::vector<Assignment> assignments;    Json power = Json::Object();
};

// The four corners a card can sit in. Kept as strings in config.json and on the pipe so a hand-edited
// config reads as "top-left" rather than as a number nobody can guess the meaning of.
const char* HudCornerName(int index);
int ParseHudCorner(std::string_view s, int def = 0);
// The border styles, named by what distinguishes them: a picker that offered "3" would
// make nobody guess what it draws.
const char* HudFrameName(int index);
int ParseHudFrame(std::string_view s, int def = 0);
int HudFrameCount();

// The three chart shapes, named the way the references he sent are usually named.
const char* ChartTypeName(int index);
int ParseChartType(std::string_view s, int def = 0);
int ChartTypeCount();

// The trajectories the shortcuts can be laid out on: the ellipse the ring started with, a funnel that
// stacks those circles one below the next, and a five-pointed star. Named, not numbered, for the same
// reason the corners are: a hand-edited "2" means nothing to anybody who has not read this file.
const char* OrbitShapeName(int index);
int ParseOrbitShape(std::string_view s, int def = 0);
int OrbitShapeCount();

class WallpaperManager {
public:
    // Loads, or writes a first-run config that points at the first wallpaper it finds.
    bool Load(const std::wstring& exeDir, std::string& error);
    bool Save(std::string& error);
    // The two halves of `Save`, split so the presenting thread can build the text it owns and let a
    // worker take the 8-43 ms file write. `WriteConfigText` serialises itself against other writers.
    std::string ConfigText() const;
    bool WriteConfigText(const std::string& text, std::string& error);

    Settings& settings() { return s_; }
    const Json& raw() const { return raw_; }
    const Settings& settings() const { return s_; }

    std::vector<WallpaperPackage>& catalog() { return catalog_; }
    const WallpaperPackage* find(std::string_view id) const;
    bool Scan(std::vector<std::string>& skipped);

    // A clip the settings window is about to show but never imported. `find()` resolves these so the
    // preview pass can start one; `catalog()` keeps hiding them, so the card row and the status JSON
    // never show a wallpaper the user did not add. Holds one at a time because the big picture does.
    bool AddEphemeral(const std::wstring& dir, std::string& error);
    void DropEphemeral();
    // The pointer is only good until the next AddEphemeral, which reallocates the vector.
    const WallpaperPackage* ephemeral() const { return ephemeral_.empty() ? nullptr : &ephemeral_.front(); }

    // Which package this monitor should show, honouring per-monitor then "all" assignments.
    std::string WallpaperForMonitor(const MonitorInfo& m, const std::string& fallback) const;
    int FpsForMonitor(const MonitorInfo& m) const;
    void SetAssignment(const std::string& monitor, const std::string& wallpaperId);
    // Per-monitor frame cap; 0 clears it so the package's own fps wins again.
    void SetFpsFor(const MonitorInfo& m, int fps);
    void SetGlobalQuality(const std::string& q);
    void SetImageFit(const std::string& f);
    void SetTrayAlpha(int pct);
    void SetHud(bool on);
    // Returns the corner now in force, so an unrecognised name echoes what the desktop is doing
    // rather than what was asked for. Choosing a corner also drops a dragged position: the last thing
    // the user did wins, and a card that is in a corner cannot also be somewhere else.
    int SetHudCorner(const std::string& name);
    // Where the user dropped the card, in screen heights from the monitor's top-left. Clamped by the
    // caller, which is the only place that knows the card's size and this monitor's taskbar strip.
    void SetOrbitPos(float x, float y);
    void ClearOrbitPos();
    void SetHudPos(float x, float y);
    void ClearHudPos();
    void SetHudScale(int pct);
    // Returns the style now in force, so an unrecognised name echoes what is on the screen.
    int SetHudFrame(const std::string& name);
    // The shortcut ring's two sizes, in the same percentage-of-reference units as the card's scale.
    void SetOrbitScale(int pct);
    void SetOrbitIconScale(int pct);
    void SetOrbitPicks(std::vector<std::string> names);
    void ClearOrbitPicks();
    // How many rings, and which path they walk. The shape returns what is now in force so an
    // unrecognised name echoes back the one on the screen instead of the one that was asked for.
    void SetOrbitRings(int n);
    int SetOrbitShape(const std::string& name);
    // The chart panel's six, mirroring the card's: the same corner/dragged-position rule, the same
    // "echo what is on the screen" contract on the name-taking ones.
    void SetChart(bool on);
    int SetChartType(const std::string& name);
    int SetChartCorner(const std::string& name);
    void SetChartPos(float x, float y);
    void ClearChartPos();
    void SetChartScale(int pct);
    int SetChartFrame(const std::string& name);
    void SetGlobalMaxFps(int fps);
    void SetRotate(const Json& r);

    Json StatusJson() const;

private:
    bool MatchMonitor(const Assignment& a, const MonitorInfo& m) const;
    Settings s_;
    Json raw_;                       // the config as loaded, so extra sections survive a save
    std::vector<WallpaperPackage> catalog_;
    std::vector<WallpaperPackage> ephemeral_;
};

} // namespace sw
