#pragma once
// Engine/App/Application.hpp - owns the device, the per-monitor slots, pacing, tray and IPC.
#include "Engine/App/PreviewStream.hpp"
#include "Engine/App/Tray.hpp"
#include "Engine/Desktop/DesktopWindow.hpp"
#include "Engine/Desktop/MonitorManager.hpp"
#include "Engine/Desktop/TrayTransparency.hpp"
#include "Engine/Desktop/WorkerW.hpp"
#include "Engine/Graphics/D3D11Device.hpp"
#include "Engine/Graphics/D3D11Renderer.hpp"
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
};

} // namespace sw
