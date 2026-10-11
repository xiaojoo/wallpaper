#pragma once
// Engine/App/Application.hpp - owns the device, the per-monitor slots, pacing, tray and IPC.
#include "Engine/App/PreviewStream.hpp"
#include "Engine/App/GpuMonitor.hpp"
#include "Engine/App/Tray.hpp"
#include "Engine/System/MachineMetrics.hpp"
#include "Engine/Desktop/DesktopWindow.hpp"
#include "Engine/Desktop/HudDragWindow.hpp"
#include "Engine/Desktop/IconHitWindow.hpp"
#include "Engine/System/DiskTemp.hpp"
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Desktop/TrayTransparency.hpp"
#include "Engine/Desktop/WorkerW.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Graphics/D3D11Renderer.hpp"
#include "Engine/Desktop/ShellIcons.hpp"
#include "Engine/Performance/FPSController.hpp"
#include "Engine/Performance/PowerManager.hpp"
#include "Engine/Packages/WallpaperInstance.hpp"
#include "Engine/Packages/Thumbnailer.hpp"
#include "Engine/Packages/WallpaperManager.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <array>
#include <vector>

namespace sw {

struct CommandLine {
    std::wstring exeDir;
    std::wstring logDir;
    int selftestSeconds = 0;
    bool noTray = false;
    bool noIpc = false;
    bool noDesktop = false;   // render into normal windows (development, does not touch the desktop)
    bool noPower = false;     // development: ignore visibility caps so rendering can be measured
    std::string logLevel;
    bool logToConsole = false;
    std::string applyWallpaper;
    std::string qualityOverride;
    int fpsOverride = 0;
    std::string pipeOverride;
};

class IPCServer;

class Application {
public:
    bool Start(const CommandLine& cl, std::string& error);
    int Run();
    void Shutdown();
    void Stop(const std::string& why);
    ~Application();

    // Called from the IPC thread and the tray; mutating commands are queued for the render thread.
    std::string Command(const std::string& line);
    std::string Snapshot() const;
    bool Paused() const { return paused_.load(); }

private:
    struct Slot {
        DesktopWindow window;
        WallpaperInstance instance;
        FPSController::Slot clock{};
        std::string wallpaperId;
        PowerManager::Decision decision{};
        std::string error;
        UINT drawErrors = 0;
        double lastSampleT = -1.0;
        // The simulation step handed to the shader: the time that actually passed between two draws of
        // this slot, plus the value last given out, which is what makes "did the cap change the speed"
        // readable from the pipe instead of something to eyeball on the desktop.
        double lastDrawT = -1.0;
        float lastDelta = 0.f;
        unsigned long long lastSample = 0;
        unsigned changedSamples_ = 0, staticSamples_ = 0;
        // Particle wallpapers get a second, independent proof: the corner can be static while the
        // simulation is still advancing, and vice versa.
        unsigned long long lastParticleHash_ = 0;
        unsigned particleMovedSamples_ = 0, particleStaticSamples_ = 0;
        unsigned particleOutOfBounds_ = 0;
        bool visible = true;
        // A power state has to hold this long before it is allowed to move the frame budget. The raw
        // classifier follows every foreground-window change, and each step 60 -> 30 -> 15 -> 60 changes
        // how much motion one drawn frame carries, which is what reads as a hitch on screen. Measured
        // without this: 24 state changes in 280 s, six of them within two seconds of each other.
        static constexpr ULONGLONG kStateHoldMs = 1000;
        PowerState candState = PowerState::Desktop;
        ULONGLONG candSinceMs = 0;
        unsigned stateSwitches_ = 0;   // counted for status: how often the budget actually moved
    };

    struct Queued {
        std::string action, arg, arg2;
    };

    // The settings window's live preview: the same wallpaper drawn a second time offscreen, at its
    // own frame budget, with the pixels handed over through a shared-memory section.
    struct Preview {
        bool on = false;
        std::string id;
        UINT64 idHash = 0;   // stamped into every frame so the window never shows the old wallpaper
        std::string error;
        // A fresh instance per wallpaper, kept in a cache: preparing one compiles HLSL, and doing
        // that inside the frame loop stalled every screen for ~100 ms each time the selection moved.
        std::map<std::string, std::unique_ptr<WallpaperInstance>> cache;
        WallpaperInstance* inst = nullptr;
        // True when `inst` is a live desktop slot's instance being drawn a second time into the
        // preview surface, rather than one this pass owns. Shared instances must not be parked,
        // erased from the cache, or left pointing at a slot that has since been rebuilt.
        bool shared = false;
        Com<ID3D11Texture2D> rt;
        Com<ID3D11RenderTargetView> rtv;
        // Two staging textures and an event query each: the copy is picked up a frame later so the
        // frame loop never sits in Map waiting for the GPU.
        Com<ID3D11Texture2D> stg[2];
        Com<ID3D11Query> q[2];
        // Each staging slot is either free or has a copy the GPU has not finished. A frame whose
        // copy is still in flight is collected on a later loop rather than dropped.
        enum SlotState : UINT8 { Free = 0, CopyPending };
        UINT8 slotState[2] = { Free, Free };
        // Set while a freshly re-targeted video has no decoded picture yet, so the "publishing held
        // back" line is logged once per re-target instead of thirty times a second.
        bool waitingFirstFrame = false;
        // The browse page's pause when the preview is drawing a *shared* desktop instance. That player
        // cannot be paused without stopping his wallpaper too, so the pause is taken here instead: the
        // pass stops publishing and the window keeps painting the frame it holds.
        bool held = false;
        // ... and each slot remembers which wallpaper it was drawn from, because that is not the same
        // question as "which one is asked for now": with two slots and the driver a frame behind, the
        // first frames collected after a switch were rendered from the wallpaper the user just left.
        // Stamping those with the current id is what made the window, and any external ruler reading
        // the section, believe a picture of one wallpaper was a picture of another.
        UINT64 slotHash[2] = { 0, 0 };
        unsigned draining = 0;      // frames written that belong to a wallpaper other than the request
        UINT w = 0, h = 0;
        FPSController::Slot clock{};
        ULONGLONG lastBeatMs = 0;
        // The pacer's own measuredFps is unreliable here: it alternates 15 and 30 while the
        // frame counter says 21. These two give the honest rate, sampled once a second.
        unsigned drewAtLastReport = 0;
        ULONGLONG reportAtMs = 0;
        double reportedFps = 0;
        unsigned drew = 0, dropped = 0;
        PreviewStream writer;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnWindowMessage(HWND h, UINT m, WPARAM w, LPARAM l);

    bool BuildSlots(std::string& error);
    // The whole JSON answer for the delimage command, so the guards and the message stay in one place.
    std::string DeleteImagePackage(const std::string& id);
    void DestroySlots();
    bool ReloadSlot(Slot& s, const std::string& wallpaperId, std::string& error);
    void DrawSlot(Slot& s, double nowSeconds);
    // Draw every slot and (unless told not to) the preview, then commit one composition.
    void DrawDue(bool withPreview = true);
    // Runs a package's device-free preparation (HLSL compile, clip container and decoder) on a worker
    // thread and keeps the desktop drawing while it runs. See Application.cpp for the measured reason a
    // switch may not stop to pay for it inline. `withPreview` is false when the caller is the preview
    // switch itself: its instance is mid-replacement there, and drawing it failed with
    // "missing rtv or shader" 12-39 times a session before that was noticed.
    std::unique_ptr<VideoSource> WarmWhileDrawing(const WallpaperPackage& pkg, std::string& error,
                                                  bool withPreview = true);
    // Hand an outgoing wallpaper to the background thread instead of destroying it here: a video player's
    // shutdown measured 28-59 ms on the thread that presents the desktop, which is most of what a switch
    // still costs after the file open moved off it.
    void ReapLater(std::unique_ptr<WallpaperInstance> outgoing);
    // One background thread for the two jobs that are too expensive for the presenting thread and need
    // no device: tearing down an outgoing wallpaper, and writing config.json (8-43 ms for this file).
    void PostBackground(std::function<void()> work);
    // Block until the background queue has run dry, keeping the desktop drawing while waiting. Needed
    // before anything that depends on a deferred teardown having actually happened - a package's
    // video.mp4 is still open until its player is destroyed, and a directory holding an open file
    // cannot be removed.
    void WaitBackgroundIdle();
    void BackgroundLoop();
    std::thread background_;
    std::mutex bgMtx_;
    std::condition_variable bgCv_;
    std::deque<std::function<void()>> bgWork_;
    bool bgStop_ = false;
    bool bgBusy_ = false;
    void ProcessQueued();
    void UpdateSnapshot();
    void DrainMessages();
    void OnMonitorsChanged();
    void BuildThumbnails(bool onlyMissing);
    bool StartPreview(const std::string& id, std::string& error);
    void StopPreview(const char* why);
    void DrawPreview(double nowSeconds);
    void TickRotation(ULONGLONG nowMs);
    // Where one of the engine's overlay panels sits on a monitor, in its own units: screen heights
    // from the monitor's top-left, as x, y, width, height. One function decides it for both the shader
    // and the drag window, so the outline the user grabs cannot drift off the panel it stands in for -
    // and `panel` is kPanelCard or kPanelChart, because the two differ only in which settings they
    // read and would otherwise be the same forty lines twice.
    enum Panel { kPanelCard = 0, kPanelChart = 1, kPanelCount = 2 };
    void PanelRectFor(const MonitorInfo& m, int panel, float& x, float& y, float& w, float& h) const;
    // The same rectangle in desktop pixels, for placing the drag window.
    RECT PanelScreenRect(const MonitorInfo& m, int panel) const;
    // Turns a panel's draggable layer on and off. Turning it on with the panel hidden would leave a
    // click target over a desktop with nothing in it, so this refuses unless that panel is showing.
    bool ArmDrag(int panel, bool on, std::string& error);
    // The same arming, for the shortcut ring's click layer, and for the ring's own clock.
    bool ArmOrbitHit(std::string& error);
    void AdvanceOrbit(ULONGLONG nowMs);
    void LaunchRingSlot(int slot);
    // Hiding the shell's own icons and giving them back, including the crash note that makes the
    // second one happen even when this process did not get to run it.
    bool SetIconsHidden(bool hide, std::string& error);
    void HealIconLayer();
    // Moves the rotation pointer by delta and shows the result. Returns the id that got on screen.
    std::string RotateStep(int delta);
    static bool IsAutostartEnabled(std::string& error);
    static bool SetAutostart(bool on, std::string& error);
    std::string DoCommandLocal(const std::string& action, const std::string& arg, const std::string& arg2);
    Quality EffectiveQuality(const Slot& s) const;
    std::string LoopDebug() const;

    CommandLine cl_;
    WallpaperManager wpm_;
    D3D11Device dev_;
    D3D11Renderer renderer_;
    MonitorManager monitors_;
    WorkerW desktop_;
    TrayTransparency trayFx_;
    PowerManager power_;
    FPSController pacer_;
    Tray tray_;
    std::unique_ptr<IPCServer> ipc_;
    Preview preview_;
    // The status line's GPU number. A member rather than a per-call load because NVML's init is what
    // enumerates the device; sampling afterwards costs under 0.05 ms.
    GpuMonitor gpu_;
    // Whole-machine readings, and the last result of each. Sampled on the loop's own one-second tick
    // rather than from the status builder: PDH's rate counters answer nothing until a window has
    // passed between two collects, and a builder that is called on demand - which the settings window
    // is, at whatever rate it feels like - would sometimes read a window that has not elapsed. The
    // cached copy is also what a wallpaper's per-frame upload reads, so no frame pays for a query.
    MachineMetrics machine_;
    MachineStats machineLast_;
    // The wallpaper-facing copy, rebuilt on the same tick. The draw path reads this rather than
    // MachineStats so a frame never touches PDH, NVML or iphlpapi.
    StatsCB statsCB_;
    GpuUse gpuLast_;
    ULONGLONG machineAtMs_ = 0;
    std::map<std::string, ThumbSet> thumbs_;
    std::wstring thumbDir_;

    std::deque<Slot> slots_;
    HWND win_ = nullptr;
    std::atomic<bool> quit_{false};
    std::atomic<bool> paused_{false};
    bool shutdown_ = false;
    HANDLE instanceMutex_ = nullptr;
    struct Rotation {
        bool on = false;
        int intervalMin = 60;
        int index = 0;
        std::vector<std::string> pool;
        std::string monitor = "all";
        ULONGLONG nextAt = 0;
    } rotate_;
    void LoadRotation(const Json& j);
    void SaveRotation();

    mutable std::mutex mtx_;
    std::vector<Queued> queued_;
    std::string snapshot_;
    std::string statusLine_;

    LARGE_INTEGER qpcStart_{};
    ULONGLONG startedMs_ = 0;
    ULONGLONG lastSnapshotMs_ = 0;
    ULONGLONG lastHostCheckMs_ = 0;
    ULONGLONG lastTrayCheckMs_ = 0;
    POINT cursorPx_{};
    bool committed_ = false;
    unsigned loopIters_ = 0, sawDrawStage_ = 0, drewTotal_ = 0;
    ULONGLONG spinWindowStart_ = 0;
    std::string lastError_;
    std::string hudError_;   // why the status card could not be prepared, empty when it can
    std::string orbitError_; // the same, for the shortcut ring

    // The desktop shortcut ring. Off by default and deliberately never persisted: an orbit of the
    // user's own icons sitting on their desktop is not something a crash should bring back. Toggled by
    // the `orbit` control verb.
    bool orbitOn_ = false;
    ShellIcons icons_;
    bool iconsBuilt_ = false;
    // How many of the desktop's shortcuts the ring draws. The atlas holds all of them; the ring is
    // capped so the per-pixel loop in IconOrbit.hlsl stays cheap on a 4K panel at 48 fps, and by
    // kMaxOrbitIcons because that is how many rows the constant buffer has.
    UINT orbitCount_ = 12;
    // Seconds for one full revolution. 0 stops the ring.
    double orbitPeriodS_ = 45.0;
    // The ring's angle, accumulated in Run() rather than read off a clock. Deriving it from absolute
    // time was fine while nothing could be pointed at: once the pointer can hold the ring still to
    // aim, an absolute phase would make it jump back to where wall-clock time says it should be the
    // moment the pointer leaves. Accumulated instead, so a pause is a pause.
    float orbitPhase_ = 0.f;
    ULONGLONG orbitLastMs_ = 0;
    // The slot the pointer is over, -1 for none. The click layer holds the same answer for the button,
    // and this is polled from the frame loop rather than pushed by mouse messages because the ring
    // moves under a pointer that is not: no message arrives when a highlight should change.
    int orbitHover_ = -1;
    IconHitWindow iconHit_;
    // Which monitor's ring is clickable, or -1. One, on the same grounds as the two panel drag layers:
    // a layer per screen would need a per-screen position, and this is one global ring.
    int orbitHitMonitor_ = -1;
    // The desktop's own icon layer, hidden while the ring stands in for it. Deliberately not a
    // persisted setting like the ring itself: `iconlayer.state` in the app directory is written when
    // we hide the icons and removed when we give them back, so a process that was killed rather than
    // closed leaves a note for the next start to clean up - and a user who hid his own icons from the
    // desktop menu before we ever ran is never touched, because no note exists.
    bool iconsHidden_ = false;
    HWND iconListSeen_ = nullptr;   // the list window we applied to, to notice explorer replacing it
    std::wstring iconMarkPath_;
    // When the ring's rectangles were last written to the log, in the frame loop's own seconds.
    double orbitGeomLogT_ = 0.0;
    // The monitor whose panel is being dragged, or -1. The layer exists only while this is >= 0, which
    // is why the desktop's own clicks are unaffected the rest of the time.
    struct PanelDrag {
        HudDragWindow win;
        int monitor = -1;
        RECT rect{};      // the rectangle the layer actually sits in, so it is only moved when the
                          // panel's own rect changed rather than every frame
    };
    PanelDrag drag_[kPanelCount];
    // The ring's own drag layer, kept apart from the two panels on purpose: a panel's stored position
    // is its top-left corner and the ring's is its centre, and folding the two into one array would
    // have made every reader of that array ask "which convention is this one" again.
    PanelDrag orbitDrag_;
    // Everything about where the ring sits and how big it is, in device pixels relative to this
    // monitor's top-left, computed once. The shader's icon boxes, the click layer's rectangles and
    // the drag layer's outline all come out of this, so they cannot disagree about where the ring is.
    struct RingShape {
        float cx = 0.f, cy = 0.f;   // the centre
        float rx = 0.f, ry = 0.f;   // the two radii
        float iconPx = 0.f;         // one icon's full size at the front of the ring
        float occX = 0.f, occY = 0.f;   // the middle object's half-width and half-height
    };
    RingShape RingFor(const MonitorInfo& m) const;
    RECT OrbitScreenRect(const MonitorInfo& m) const;
    bool ArmOrbitDrag(bool on, std::string& error);
    // The chart panel's history, one row of four per sampled second. Every slot starts at -1 rather
    // than at zero, because a zero on a gauge is a claim ("this metric read 0%") and an absence is not.
    // `= { -1.f }` would only have set the first one, which is the whole reason this is a fill.
    DiskTemp diskTemp_;
    ULONGLONG diskTempAtMs_ = 0;
    std::string diskTempWhy_;
    std::array<float, 4 * kChartSamples> chartRing_ = [] {
        std::array<float, 4 * kChartSamples> r;
        r.fill(-1.f);
        return r;
    }();
};

} // namespace sw
