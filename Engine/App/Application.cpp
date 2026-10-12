#include "Engine/App/Application.hpp"

#include <cmath>
#include "Engine/App/IPCServer.hpp"
#include "Engine/Core/Crash.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Desktop/IconLayer.hpp"
#include "Engine/Graphics/FFmpegPlayer.hpp"
#include "Engine/System/DiskTemp.hpp"
#include "Engine/Packages/HudFramesMeta.hpp"

#include <psapi.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#include <wincodec.h>
#include <sstream>

namespace sw {
static constexpr const char* MOD = "app";
constexpr const wchar_t* kAppClass = L"SmartWallpaperApp";
// One id for the timer that re-applies the taskbar fade; see TrayTransparency.hpp for why it needs
// re-applying at all. 5 s is a compromise the eye cannot see: the shell rebuilds the bar on events
// that are rare (monitor change, its own restart), and the fade is a look, not a function.
constexpr UINT_PTR kTrayTimer = 0x57A1;

namespace {

std::wstring LogDirFor(const std::wstring& exeDir) { return exeDir + L"\\logs"; }

Level LevelFromString(const std::string& s) {
    if (s == "trace") return Level::Trace;
    if (s == "warn") return Level::Warn;
    if (s == "error") return Level::Error;
    return Level::Info;
}

struct MemSample {
    double workingSetMb = 0, privateMb = 0;
    double cpuPercent = 0;
    double cpuWindowS = 0;   // how long the window behind cpuPercent actually was
};

MemSample SampleProcess(ULONGLONG& lastWallNs, ULONGLONG& lastCpuNs) {
    MemSample m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        m.workingSetMb = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        m.privateMb = double(pmc.PagefileUsage ? pmc.PagefileUsage : pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        ULONGLONG cpu = ((ULONGLONG)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                        ((ULONGLONG)user.dwHighDateTime << 32 | user.dwLowDateTime);
        static LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER li{};
        QueryPerformanceCounter(&li);
        // Wall clock in 100 ns units, same scale as the FILETIMEs, so the ratio is real CPU percent.
        ULONGLONG wallNow = (ULONGLONG)((double)li.QuadPart * 1e7 / (double)freq.QuadPart);
        // The window must be the renderer's own, not "since whoever read the status last". The settings
        // window polls this every second, so a plain delta reports whatever burst landed in that second:
        // measured 2026-10-07, one and the same steady state read 168.9%, 29.0% and 18.0% in three
        // samples, and the number in his status bar was the 168.9 one. Only a window of at least
        // kCpuWindowNs is published; in between the last complete window is repeated, so the value
        // never silently falls to 0 for a reader that polls faster than the window.
        constexpr ULONGLONG kCpuWindowNs = 20000000ULL;   // 2 s, in 100 ns units
        static double lastPct = 0.0;
        static double lastWinS = 0.0;
        if (lastWallNs == 0) {
            lastWallNs = wallNow;
            lastCpuNs = cpu;
        } else if (wallNow > lastWallNs && cpu >= lastCpuNs && wallNow - lastWallNs >= kCpuWindowNs) {
            lastPct = double(cpu - lastCpuNs) / double(wallNow - lastWallNs) * 100.0;
            lastWinS = double(wallNow - lastWallNs) * 1e-7;
            lastWallNs = wallNow;
            lastCpuNs = cpu;
        }
        m.cpuPercent = lastPct;
        m.cpuWindowS = lastWinS;
    }
    return m;
}

} // namespace

bool Application::Start(const CommandLine& cl, std::string& error) {
    cl_ = cl;
    deferNested_ = cl.deferNested;
    std::wstring logDir = cl_.logDir.empty() ? LogDirFor(cl_.exeDir) : cl_.logDir;
    CreateDirectoryW(logDir.c_str(), nullptr);
    // Before the log itself: the whole point of this handler is to survive the cases where the logger
    // is the thing that is broken. See Engine/Core/Crash.hpp for why the process has no other way of
    // telling us where it died.
    InstallCrashHandler(logDir);
    std::wstring logFile = logDir + L"\\renderer-" + std::to_wstring(GetCurrentProcessId()) + L".log";
    std::string lvl = !cl_.logLevel.empty() ? cl_.logLevel : "info";
    if (!log::Open(logFile, LevelFromString(lvl), cl_.logToConsole)) {
        error = "cannot open log file " + ToUtf8(logFile);
        return false;
    }
    Info(MOD, "=== SmartWallpaper renderer starting, pid {} ===", GetCurrentProcessId());
    Info(MOD, "exe dir {}", ToUtf8(cl_.exeDir));

    // Two renderers on one pipe split connections and answer with different state (measured).
    instanceMutex_ = CreateMutexW(nullptr, TRUE, L"Local\\SmartWallpaper.Renderer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        error = "another WallpaperRenderer is already running on this session";
        log::Close();
        return false;
    }

    MakeProcessPerMonitorAware();

    if (!wpm_.Load(cl_.exeDir, error)) {
        Error(MOD, "config: {}", error);
        return false;
    }
    if (!cl_.logLevel.empty()) wpm_.settings().logLevel = cl_.logLevel;
    if (cl_.noTray) wpm_.settings().tray = false;
    if (cl_.noIpc) wpm_.settings().ipc = false;
    if (!cl_.pipeOverride.empty()) wpm_.settings().pipeName = ToWide(cl_.pipeOverride);
    if (cl_.fpsOverride > 0) wpm_.settings().globalMaxFps = cl_.fpsOverride;
    if (!cl_.qualityOverride.empty()) wpm_.SetGlobalQuality(cl_.qualityOverride);

    std::vector<std::string> skipped;
    if (!wpm_.Scan(skipped)) Warn(MOD, "wallpaper scan produced nothing");
    if (wpm_.catalog().empty()) {
        error = "no wallpapers in " + ToUtf8(wpm_.settings().root) + "\\Wallpapers";
        Error(MOD, "{}", error);
        return false;
    }
    LoadRotation(wpm_.raw().at("rotate"));
    // Whatever the user left the taskbar faded to, put it back before the first frame is drawn.
    trayFx_.Apply(wpm_.settings().trayAlpha);
    if (!cl_.applyWallpaper.empty()) {
        for (auto& a : wpm_.settings().assignments) a.wallpaper = cl_.applyWallpaper;
        if (!wpm_.find(cl_.applyWallpaper))
            Warn(MOD, "requested wallpaper '{}' is not in the catalog", cl_.applyWallpaper);
    }

    if (!dev_.Init(false)) {
        error = "D3D11 device init failed";
        return false;
    }
    // COM is up (dev_.Init did CoInitializeEx) and WIC needs it; the video backend needs nothing
    // process-wide any more - libavformat is opened per file and closed with the player.
    if (!renderer_.Init(dev_.dev.Get(), error)) {
        Error(MOD, "renderer: {}", error);
        return false;
    }

    // Not fatal. The card is an extra, and a desktop that cannot read its own shader or atlas should
    // still show the wallpaper; the reason stays in the log and in hud_error for the settings window.
    if (!renderer_.InitHud(dev_.dev.Get(), dev_.wic.Get(), wpm_.settings().root, error)) {
        hudError_ = error;
        Warn(MOD, "status card unavailable: {}", error);
        error.clear();
    }
    // The shortcut ring, also not fatal and also off until asked for. Built here rather than lazily
    // on the first `orbit` verb because the expensive part - asking the shell for 43 icons - should
    // not happen inside a frame, and because a desktop whose icons could not be read should say so
    // once at startup instead of drawing an empty ring every time it is switched on.
    {
        std::string ierr;
        if (!renderer_.InitOrbit(dev_.dev.Get(), wpm_.settings().root, ierr)) {
            orbitError_ = ierr;
            Warn(MOD, "icon orbit unavailable: {}", ierr);
        } else {
            std::string berr;
            if (icons_.Build(berr)) {
                iconsBuilt_ = true;
                RebuildOrbitOrder();
                std::string uerr;
                if (!renderer_.SetIconAtlas(dev_.dev.Get(), icons_.pixels().data(),
                                            icons_.atlasWidth(), icons_.atlasHeight(), uerr)) {
                    orbitError_ = uerr;
                    iconsBuilt_ = false;
                    Warn(MOD, "icon atlas not uploaded: {}", uerr);
                }
            } else {
                orbitError_ = berr;
                Warn(MOD, "no desktop shortcuts for the orbit: {}", berr);
            }
        }
    }
    monitors_.Refresh(&dev_);
    if (monitors_.count() == 0) {
        error = "no monitors";
        Error(MOD, "{}", error);
        return false;
    }

    if (cl_.noDesktop) {
        DesktopHost h{};
        h.method = HostMethod::OwnTopLevel;
        h.detail = "--window mode: normal top-level windows, desktop untouched";
        Rect vb = monitors_.virtualBounds();
        h.origin = POINT{0, 0};
        h.client = SIZE{GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        desktop_.SetOverrideHost(h);
        Warn(MOD, "{}", h.detail);
    } else if (!desktop_.Attach(error)) {
        Error(MOD, "desktop attach failed: {}", error);
        return false;
    }
    // A run that was killed rather than closed left its note behind, and the desktop is still missing
    // the icons it had. Done before any frame is drawn so the user never sees a restored-then-hidden
    // flicker, and skipped in --window mode where we have not touched the desktop at all.
    iconMarkPath_ = wpm_.settings().root + L"\\iconlayer.state";
    if (!cl_.noDesktop) HealIconLayer();

    if (!power_.Init(error)) {
        Error(MOD, "power manager: {}", error);
        return false;
    }
    power_.LoadCaps(wpm_.settings().power);
    if (!pacer_.Init(error)) {
        Error(MOD, "pacer: {}", error);
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Application::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kAppClass;
    wc.hbrBackground = nullptr;
    ATOM atom = RegisterClassExW(&wc);
    if (!atom)
        Warn(MOD, "RegisterClassExW(app) atom=0 err={} (class may already exist in this process)", GetLastError());
    else
        Info(MOD, "app window class atom {}", (unsigned)atom);
    Info(MOD, "class atom {} (err {})", (unsigned)atom, atom ? 0 : GetLastError());
    win_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, L"SmartWallpaper", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                           GetModuleHandleW(nullptr), this);
    if (!win_) {
        DWORD gle = GetLastError();
        error = std::format("app window: atom={} gle={} {}", (unsigned)atom, gle, HResultToString(HRESULT_FROM_WIN32(gle)));
        Error(MOD, "{}", error);
        return false;
    }
    // The fade lives on explorer's window, which we do not own: it is lost when the shell rebuilds
    // the bar, and that sends us no message. Poll instead of hoping.
    SetTimer(win_, kTrayTimer, 5000, nullptr);
    SetWindowLongPtrW(win_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (!BuildSlots(error)) {
        Error(MOD, "no wallpaper could start: {}", error);
        return false;
    }
    Info(MOD, "app window 0x{:X} built {} slot(s)", (uintptr_t)win_, slots_.size());

    thumbDir_ = wpm_.settings().root + L"\\cache\\thumbs";
    BuildThumbnails(true);

    if (wpm_.settings().tray) {
        std::string terr;
        if (!tray_.Install(win_, this, terr)) Warn(MOD, "tray unavailable: {}", terr);
    }
    if (wpm_.settings().ipc) {
        ipc_ = std::make_unique<IPCServer>();
        std::string ierr;
        if (!ipc_->Start(wpm_.settings().pipeName, this, ierr)) {
            Warn(MOD, "ipc pipe unavailable: {} ({} will keep running)", ierr, "the renderer");
            ipc_.reset();
        }
    }

    QueryPerformanceCounter(&qpcStart_);
    startedMs_ = GetTickCount64();
    UpdateSnapshot();
    Info(MOD, "ready: {} slot(s), host {}", slots_.size(), desktop_.host().methodName());
    return true;
}

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"SmartWallpaper.Renderer";

std::wstring ExePath() {
    wchar_t buf[4096];
    GetModuleFileNameW(nullptr, buf, 4095);
    return buf;
}
} // namespace

// Login autostart is one HKCU Run value: adding or deleting only our own named value, never the
// key itself, so it stays a "只加不改" change to his session and is off unless he turns it on.
// No error string: a missing Run key is not a failure to report, it is the state "off", and the only
// caller that could show a reason is the switch that writes it (SetAutostart below).
bool Application::IsAutostartEnabled() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[4096];
    DWORD cb = sizeof(buf), type = 0;
    const LSTATUS r = RegQueryValueExW(k, kRunValue, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    return type == REG_SZ && cb > 0; // presence of our own value is the state
}

bool Application::SetAutostart(bool on, std::string& error) {
    HKEY k = nullptr;
    const LSTATUS o = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k);
    if (o != ERROR_SUCCESS) { error = "cannot open Run key: " + std::to_string(o); return false; }
    const std::wstring path = L"\"" + ExePath() + L"\"";
    LSTATUS r = on ? RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
                                   DWORD((path.size() + 1) * sizeof(wchar_t)))
                   : RegDeleteValueW(k, kRunValue);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS && !(r == ERROR_FILE_NOT_FOUND && !on)) {
        error = on ? "RegSetValueExW failed: " + std::to_string(r) : "RegDeleteValueW failed: " + std::to_string(r);
        return false;
    }
    Info(MOD, "autostart {} -> {}", on ? "on" : "off", ToUtf8(path));
    return true;
}

namespace {
// Adds a user picture as a wallpaper package: a folder with wallpaper.json pointing at the shared
// Image.hlsl preset, so nothing has to be recompiled to show someone's own photo.
// `origin`, when the settings window says where the file came from, is written into the manifest and
// is what separates a picture he took from one this app fetched off a site.
std::string AddImagePackage(const std::wstring& root, const std::wstring& srcFile, IWICImagingFactory* wic,
                            std::string& error, const std::string& origin = {}) {
    namespace fs = std::filesystem;
    if (!fs::exists(srcFile)) { error = "file does not exist"; return {}; }
    UINT w = 0, h = 0;
    if (wic) {
        Com<IWICBitmapDecoder> dec;
        if (SUCCEEDED(wic->CreateDecoderFromFilename(srcFile.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &dec))) {
            Com<IWICBitmapFrameDecode> f;
            if (SUCCEEDED(dec->GetFrame(0, &f))) f->GetSize(&w, &h);
        }
    }
    if (!w || !h) { error = "not a decodable image"; return {}; }

    std::wstring dir;
    for (int n = 1; n < 1000; ++n) {
        wchar_t buf[32];
        swprintf(buf, 32, L"\\local_%02d", n);
        dir = root + L"\\Wallpapers" + buf;
        if (!fs::exists(dir)) break;
        dir.clear();
    }
    if (dir.empty()) { error = "too many local image packages"; return {}; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring ext = fs::path(srcFile).extension().wstring();
    const std::wstring dst = dir + L"\\image" + ext;
    if (!CopyFileW(srcFile.c_str(), dst.c_str(), FALSE)) { error = "copy failed"; return {}; }

    const std::string id = ToUtf8(fs::path(dir).filename().wstring());
    const float aspect = float(w) / float(h);
    Json j = Json::Object();
    j.set("id", Json::Of(id));
    j.set("name", Json::Of(ToUtf8(fs::path(srcFile).filename().wstring())));
    // An imported picture is not 本地图片: the browser groups by category, and a label that names the
    // wrong thing is worse than no label. `origin` is the page it came from, kept with the package
    // rather than in the settings window's own state, so the folder alone says where it came from.
    j.set("category", Json::Of(origin.empty() ? std::string("本地图片") : std::string("在线图片")));
    if (!origin.empty()) j.set("origin", Json::Of(origin));
    j.set("renderer", Json::Of(std::string("d3d11")));
    j.set("width", Json::Of((long long)w));
    j.set("height", Json::Of((long long)h));
    j.set("fps", Json::Of(30));
    j.set("quality", Json::Of(std::string("high")));
    j.set("shader", Json::Of(std::string("Image.hlsl")));
    Json tx = Json::Array();
    Json one = Json::Object();
    one.set("name", Json::Of(std::string("image")));
    one.set("file", Json::Of(std::string("image") + ToUtf8(ext)));
    tx.push(std::move(one));
    j.set("textures", std::move(tx));
    Json pr = Json::Object();
    pr.set("zoom", Json::Of(1.0));
    pr.set("src_aspect", Json::Of(double(aspect)));
    pr.set("vignette", Json::Of(0.18));
    pr.set("drift", Json::Of(0.012));
    pr.set("saturation", Json::Of(1.05));
    pr.set("brightness", Json::Of(1.0));
    j.set("params", std::move(pr));
    if (!WriteFileUtf8(dir + L"\\wallpaper.json", j.dump(2))) { error = "cannot write manifest"; return {}; }
    Info(MOD, "added local image package {} ({}x{})", id, w, h);
    return id;
}

// Adds a video as a wallpaper package. The id keeps the images' local_NN shape on purpose: the delete
// guard only accepts that form, and the settings window's import list is built from it. `origin` is the
// page it came from, exactly as in AddImagePackage.
std::string AddVideoPackage(const std::wstring& root, const std::wstring& srcFile, std::string& error,
                            const std::string& origin = {}) {
    namespace fs = std::filesystem;
    if (!fs::exists(srcFile)) { error = "file does not exist"; return {}; }

    VideoProbe probe{};
    {
        std::string perr;
        if (!FfmpegProbeVideo(srcFile, probe, perr)) { error = perr; return {}; }
    }
    const UINT w = probe.w, h = probe.h;
    const double rate = probe.fps, duration = probe.duration_s;
    const bool bt601 = probe.bt601;
    if (!w || !h) { error = "the video has no readable frame size"; return {}; }

    std::wstring dir;
    for (int n = 1; n < 1000; ++n) {
        wchar_t buf[32];
        swprintf(buf, 32, L"\\local_%02d", n);
        dir = root + L"\\Wallpapers" + buf;
        if (!fs::exists(dir)) break;
        dir.clear();
    }
    if (dir.empty()) { error = "too many imported wallpapers"; return {}; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::wstring ext = fs::path(srcFile).extension().wstring();
    if (!CopyFileW(srcFile.c_str(), (dir + L"\\video" + ext).c_str(), FALSE)) {
        error = "copy failed: " + HResultToString(GetLastError());
        return {};
    }

    const std::string id = ToUtf8(fs::path(dir).filename().wstring());
    // The manifest carries the picture's own numbers so the browser can label and lay out the card
    // without opening a decoder for every row.
    const int fps = rate > 1.0 ? int(rate + 0.5) : 30;
    Json j = Json::Object();
    j.set("id", Json::Of(id));
    j.set("name", Json::Of(ToUtf8(fs::path(srcFile).filename().wstring())));
    j.set("category", Json::Of(origin.empty() ? std::string("本地视频") : std::string("在线视频")));
    if (!origin.empty()) j.set("origin", Json::Of(origin));
    j.set("renderer", Json::Of(std::string("video")));
    j.set("video", Json::Of(std::string("video") + ToUtf8(ext)));
    j.set("loop", Json::Of(true));
    j.set("width", Json::Of((long long)w));
    j.set("height", Json::Of((long long)h));
    j.set("duration_s", Json::Of(duration));
    j.set("fps", Json::Of(fps));
    j.set("quality", Json::Of(std::string("high")));
    j.set("shader", Json::Of(std::string("Video.hlsl")));
    Json pr = Json::Object();
    pr.set("zoom", Json::Of(1.0));
    pr.set("src_aspect", Json::Of(double(w) / double(h)));
    pr.set("vignette", Json::Of(0.0));
    // No Ken Burns on a video: the clip already has a camera, and sliding the frame under it reads as
    // the wallpaper drifting rather than as motion in the picture.
    pr.set("drift", Json::Of(0.0));
    pr.set("saturation", Json::Of(1.0));
    pr.set("brightness", Json::Of(1.0));
    pr.set("bt601", Json::Of(bt601 ? 1.0 : 0.0));
    j.set("params", std::move(pr));
    if (!WriteFileUtf8(dir + L"\\wallpaper.json", j.dump(2))) { error = "cannot write manifest"; return {}; }
    Info(MOD, "added local video package {} ({}x{}, {:.2f} fps, {:.1f} s, decoded by {})", id, w, h, rate,
         duration, probe.codec);
    return id;
}
} // namespace

// Removes an imported image package - the copy we made under Wallpapers\local_NN, never the file the
// user picked. Only that shape of id is accepted, so nothing here can be pointed at another path.
std::string Application::DeleteImagePackage(const std::string& id) {
    namespace fs = std::filesystem;
    if (id.rfind("local_", 0) != 0 || id.size() > 14 ||
        !std::all_of(id.begin() + 6, id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return R"({"ok":false,"code":"not_local","error":"only imported images can be deleted"})";
    for (auto& s : slots_)
        if (s.wallpaperId == id)
            return std::format(R"({{"ok":false,"code":"in_use","monitor":"{}","error":"in use on {}","id":"{}"}})",
                                   s.window.monitor().tag, s.window.monitor().tag, id);
    for (auto& a : wpm_.settings().assignments)
        if (a.wallpaper == id)
            return std::format(R"({{"ok":false,"code":"in_use","monitor":"{}","error":"assigned to {}","id":"{}"}})",
                                   a.monitor, a.monitor, id);

    // Every handle this process holds on the package has to be closed *before* the directory goes: the
    // preview's instance keeps `video.mp4` open while it exists, and Windows answers
    // ERROR_SHARING_VIOLATION to removing a directory that contains an open file (measured 2026-10-08
    // 14:21:50 - deleting the clip the hero was showing failed exactly that way, and the same click at
    // 14:23:14 succeeded once the hero had moved off it). The teardown is deferred to the background
    // thread, so this also waits for it to have actually run.
    //
    // The preview holds one compiled instance per id and that instance owns the loaded picture.
    // An import takes the lowest free local_NN, so the next upload arrives under this same id and
    // would be drawn with the deleted file's texture - the card thumbnails come off disk and look
    // right while the big picture stays on the old one. Stopping the pass is what lets the next
    // `preview` get past the "already on" shortcut and prepare the new package.
    if (preview_.id == id) {
        StopPreview("package deleted");
        preview_.inst = nullptr;
    }
    if (auto it = preview_.cache.find(id); it != preview_.cache.end()) {
        ReapLater(std::move(it->second));
        preview_.cache.erase(it);
    }
    WaitBackgroundIdle();

    const std::wstring dir = wpm_.settings().root + L"\\Wallpapers\\" + ToWide(id);
    std::error_code ec;
    if (!fs::exists(dir, ec))
        return std::format(R"({{"ok":false,"code":"missing","error":"no such package","id":"{}"}})", id);
    fs::remove_all(dir, ec);
    if (ec) return std::format(R"({{"ok":false,"code":"io","error":"{}","id":"{}"}})", ec.message(), id);

    // The thumbnails and the motion frames are named after the id; leaving them behind would make the
    // next import into the same slot show a dead preview.
    for (auto& suffix : {std::wstring(L"_still.png"), std::wstring(L"_large.png")})
        fs::remove(thumbDir_ + L"\\" + ToWide(id) + suffix, ec);
    fs::remove_all(thumbDir_ + L"\\" + ToWide(id) + L"_frames", ec);
    thumbs_.erase(id);

    std::vector<std::string> skipped;
    wpm_.Scan(skipped);
    Info(MOD, "deleted local image package {}", id);
    return std::format(R"({{"ok":true,"deleted":"{}","catalog":{}}})", id, wpm_.catalog().size());
}

bool Application::BuildSlots(std::string& error) {
    // A shared preview instance lives inside a Slot, and slots_ is about to be cleared: stop the pass
    // first so it cannot keep drawing - or worse, holding a pointer - into a destroyed slot.
    StopPreview("slots rebuilt");
    slots_.clear();
    std::string firstFailure;
    int built = 0;
    for (auto& m : monitors_.list()) {
        slots_.emplace_back();
        Slot& s = slots_.back();
        s.window.surface().setLabel(ToWide(m.tag));
        std::string err;
        if (!s.window.Create(desktop_.host(), m, err)) {
            s.error = err;
            if (firstFailure.empty()) firstFailure = err;
            Error(MOD, "{}: window failed: {}", m.tag, err);
            continue;
        }
        std::string id = wpm_.WallpaperForMonitor(m, wpm_.catalog().front().id());
        if (!ReloadSlot(s, id, err)) {
            s.error = err;
            if (firstFailure.empty()) firstFailure = err;
            Error(MOD, "{}: wallpaper '{}' failed: {}", m.tag, id, err);
            s.window.Destroy();
            slots_.pop_back();
            continue;
        }
        ++built;
    }
    if (!built) {
        error = firstFailure.empty() ? "no monitor could be built" : firstFailure;
        return false;
    }
    return true;
}

// A clip costs 16-99 ms of container probing and 40-49 ms of decoder-plus-hardware-session, and a
// package's HLSL costs 46-104 ms of D3DCompile - none of which touches our D3D11 device. Done inline -
// which is how every switch worked until 2026-10-08 - they held the composited desktop on one picture
// for 100-180 ms, and twice that when the settings window's hero carousel landed on a video, because
// the eviction of the previous instance was also inline. That pause is what reads as 卡顿一下 when a
// clip starts from the beginning. So the worker does everything that needs no device while this thread
// keeps drawing and keeps the shell's messages moving; what is left inline is the pipeline creation,
// the plane textures and the first decoded frame, ~10 ms together.
std::unique_ptr<VideoSource> Application::WarmWhileDrawing(const WallpaperPackage& pkg, std::string& error,
                                                           bool withPreview) {
    const std::wstring root = wpm_.settings().root;
    std::unique_ptr<VideoSource> warm;
    std::atomic<bool> done{false};
    std::thread worker([&] {
        std::string serr;
        WarmPackageShader(pkg, root, serr);   // Prepare reports a shader that cannot be found or built
        if (pkg.isVideo()) warm = OpenPackageMedia(pkg, error);
        done.store(true);
    });
    while (!done.load()) { DrainMessages(); DrawDue(withPreview); Sleep(1); }
    worker.join();
    return warm;
}

void Application::PostBackground(std::function<void()> work) {
    { std::lock_guard lk(bgMtx_); bgWork_.push_back(std::move(work)); }
    bgCv_.notify_one();
}

// A player's shutdown is its decode thread's join plus libav's own teardown of the decoder and the
// hardware session, measured at 28 ms on a desktop switch and 59 ms when the settings window's cache
// evicted a 4K clip - and it happens on the thread that presents the desktop, which is the difference
// between a switch you can see and one you cannot. Nothing here touches the immediate context: COM
// Release on a resource the GPU may still be using is safe, and the instance is out of every slot and
// cache before it gets here.
void Application::ReapLater(std::unique_ptr<WallpaperInstance> outgoing) {
    if (!outgoing) return;
    // The instance is captured by the job, so it is the job's own destruction - on the background
    // thread - that runs ~WallpaperInstance. shared_ptr because std::function requires a copyable
    // target; the only other reference is the one this function drops on return.
    PostBackground([outgoing = std::shared_ptr<WallpaperInstance>(std::move(outgoing))] {});
}

void Application::BackgroundLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(bgMtx_);
            bgCv_.wait(lk, [&] { return bgStop_ || !bgWork_.empty(); });
            if (bgWork_.empty()) return;   // stopped, and the queue is drained first
            job = std::move(bgWork_.front());
            bgWork_.pop_front();
            bgBusy_ = true;
        }
        // The payload of a deferred teardown lives *inside* the job object, so it is the job's own
        // destruction that closes the file - and that has to happen while this thread is still marked
        // busy, or `WaitBackgroundIdle` can return with the handle open (measured: the delete still
        // came back as ERROR_SHARING_VIOLATION 23 ms before the player's own "closed a player" line).
        job();
        job = nullptr;
        { std::lock_guard lk(bgMtx_); bgBusy_ = false; }
    }
}

void Application::WaitBackgroundIdle() {
    const ULONGLONG until = GetTickCount64() + 1000;
    for (;;) {
        bool busy;
        { std::lock_guard lk(bgMtx_); busy = bgBusy_ || !bgWork_.empty(); }
        if (!busy || GetTickCount64() >= until) return;
        DrainMessages();
        DrawDue(false);
        Sleep(2);
    }
}

bool Application::ReloadSlot(Slot& s, const std::string& wallpaperId, std::string& error) {
    const WallpaperPackage* pkg = wpm_.find(wallpaperId);
    if (!pkg) {
        error = "wallpaper '" + wallpaperId + "' is not installed";
        return false;
    }
    // A preview that was pointed at this slot's own decoder must let go before the swap: after it, the
    // instance it holds is moved out of and its player is on another thread's teardown list.
    if (preview_.shared && preview_.inst == &s.instance) StopPreview("shared slot reloaded");
    std::unique_ptr<VideoSource> warm = WarmWhileDrawing(*pkg, error);
    if (pkg->isVideo() && !warm) return false;
    WallpaperInstance fresh;
    if (!fresh.Prepare(wpm_.settings().root, *pkg, dev_, error, std::move(warm))) return false;
    ReapLater(std::make_unique<WallpaperInstance>(std::move(s.instance)));
    s.instance = std::move(fresh);
    s.wallpaperId = wallpaperId;
    s.clock = FPSController::Slot{};
    s.clock.windowStart = pacer_.Now();
    s.lastParticleHash_ = 0;
    s.particleMovedSamples_ = s.particleStaticSamples_ = s.particleOutOfBounds_ = 0;
    s.changedSamples_ = s.staticSamples_ = 0;
    s.lastSample = 0;
    s.error.clear();
    Info(MOD, "{} now showing '{}' ({})", s.window.monitor().tag, pkg->name(), wallpaperId);
    return true;
}

Quality Application::EffectiveQuality(const Slot& s) const {
    if (wpm_.settings().useGlobalQuality) return wpm_.settings().globalQuality;
    if (const WallpaperPackage* p = wpm_.find(s.wallpaperId)) return p->defaultQuality();
    return Quality::High;
}

// How far a panel sits in from the edge of its screen: 5 physical pixels on all four sides. Pixels
// rather than a fraction of the screen height because that is the number he asked for, and it has to
// read the same on a 1080p laptop and on this 4K panel. It used to be 0.0126 in from the side and
// 0.050 down from the top, which on this screen is 27 px and 108 px - the top one is what put the card
// on top of the wallpaper's own header.
constexpr float kPanelEdgePx = 5.f;

void Application::PanelRectFor(const MonitorInfo& m, int panel,
                               float& x, float& y, float& w, float& h) const {
    const auto& s = wpm_.settings();
    if (m.px.h <= 0) { x = y = 0.f; w = h = 1.f; return; }
    const bool card = panel == kPanelCard;
    const int style = card ? s.hudFrame : s.chartFrame;
    const float H = float(m.px.h);
    const float aspect = float(m.px.w) / H;
    const int st = style >= 0 && style < kHudFrameCount ? style : 0;
    const int scalePct = card ? s.hudScalePct : s.chartScalePct;
    h = 0.150f * std::max(float(scalePct) / 100.0f, 0.2f);
    w = h * kHudFrameAspect[st];
    // A panel parked under the taskbar is a panel nobody sees, so the bottom of the screen stops where
    // the working area does. Measured off this monitor rather than assumed.
    const float inset = kPanelEdgePx / H;
    const float reserve = m.work.valid()
        ? float(m.px.y + m.px.h - m.work.y - m.work.h) / H : 0.0f;
    const float loX = inset, hiX = std::max(inset, aspect - inset - w);
    const float loY = inset, hiY = std::max(inset, 1.0f - reserve - inset - h);
    const auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const float px = card ? s.hudX : s.chartX, py = card ? s.hudY : s.chartY;
    const int corner = card ? s.hudCorner : s.chartCorner;
    if (px >= 0.f && py >= 0.f) {                // dragged somewhere: that wins, inside the screen
        x = clamp(px, loX, hiX);
        y = clamp(py, loY, hiY);
        return;
    }
    x = (corner == 1 || corner == 3) ? loX : hiX;
    y = corner >= 2 ? hiY : loY;
}

RECT Application::PanelScreenRect(const MonitorInfo& m, int panel) const {
    float x, y, w, h;
    PanelRectFor(m, panel, x, y, w, h);
    const float H = float(m.px.h);
    return RECT{ LONG(m.px.x + x * H), LONG(m.px.y + y * H),
                 LONG(m.px.x + (x + w) * H), LONG(m.px.y + (y + h) * H) };
}

bool Application::ArmDrag(int panel, bool on, std::string& error) {
    PanelDrag& d = drag_[panel];
    if (!on) {
        d.win.Destroy();
        d.monitor = -1;
        return true;
    }
    if (d.monitor >= 0) return true;
    const auto& s = wpm_.settings();
    const bool showing = panel == kPanelCard ? (s.hud && renderer_.hudReady())
                                             : (s.chart && renderer_.chartReady());
    if (!showing) {
        error = panel == kPanelCard ? "the status card is not on the desktop, so there is nothing to drag"
                                    : "the chart panel is not on the desktop, so there is nothing to drag";
        return false;
    }
    // The layer goes on the monitor the pointer is over, since that is the panel the user is reaching
    // for. The position itself is one global setting, like the size and the corner, so it moves the
    // panel on every screen at once.
    POINT cp{};
    GetCursorPos(&cp);
    const MonitorInfo* pick = monitors_.byIndex(0);
    for (auto& mm : monitors_.list())
        if (cp.x >= mm.px.x && cp.x < mm.px.x + mm.px.w &&
            cp.y >= mm.px.y && cp.y < mm.px.y + mm.px.h) pick = &mm;
    if (!pick) { error = "no monitor to drag on"; return false; }
    const MonitorInfo m = *pick;
    const RECT work{ m.px.x, m.px.y, m.px.x + m.px.w, m.px.y + m.px.h };
    if (!d.win.Create(GetModuleHandleW(nullptr), PanelScreenRect(m, panel), work,
                      panel == kPanelCard ? L"SmartWallpaper status card - drag to move"
                                          : L"SmartWallpaper chart panel - drag to move",
                      [this, panel, idx = m.index](float x, float y, bool commit) -> RECT {
        const MonitorInfo* mm = monitors_.byIndex(size_t(idx));
        if (!mm) return RECT{};
        // The layer is told where the panel may actually go, so the outline stops at a corner with it
        // instead of sliding past it under the pointer.
        if (panel == kPanelCard) wpm_.SetHudPos(x, y); else wpm_.SetChartPos(x, y);
        const RECT r = PanelScreenRect(*mm, panel);
        if (commit) {
            std::string e;
            wpm_.Save(e);                  // a dropped panel that comes back somewhere else next boot
            ArmDrag(panel, false, e);      // is not a dropped panel at all
        }
        return r;
                      }, error)) {
        Warn(MOD, "panel drag layer: {}", error);
        return false;
    }
    d.monitor = m.index;
    d.rect = PanelScreenRect(m, panel);
    Info(MOD, "{} drag layer armed on {} at {}x{}+{},{}", panel == kPanelCard ? "card" : "chart",
         m.tag, d.rect.right - d.rect.left, d.rect.bottom - d.rect.top, d.rect.left, d.rect.top);
    return true;
}

namespace {
// The three trajectories, in the order OrbitShapeName() lists them. A fourth shape means adding a
// name there and a branch in ShapePoint below, and nothing anywhere else - the shader, the click
// boxes and the drag handle all follow whatever that one function writes.
constexpr int kOrbitEllipse = 0, kOrbitFunnel = 1, kOrbitStar = 2;
constexpr float kTau = 6.28318530718f;
// The funnel's layers sit this many icon heights apart, centre to centre. It has to be more than one:
// at 1.0 the layers touch, and below that the stack reads as one spiral band rather than as layers.
constexpr float kFunnelGap = 2.0f;
// One rim's own half-height, as a fraction of the step to the next rim. The ellipse's 0.16-of-work-area
// height cannot be used here: it is several times the step, so consecutive rims interleave and the
// layering disappears - which is the whole thing this shape is for. 0.8 leaves a visible gap between
// one rim's bottom and the next one's top.
constexpr float kFunnelRim = 0.8f;
// How narrow the neck is against the mouth. The reference he sent measures 212 px of half width at the
// mouth against 64 px at the neck - 3.3:1 - so 0.30. The first version used a pentagram-ish 0.22 and
// closed up faster than the picture does.
constexpr float kFunnelNeck = 0.30f;
// How many revolutions the shortcuts wind while they descend. One turn puts every icon at its own
// angle and its own height, which draws a diagonal falling line; the picture is a vortex, and that
// needs the same angle to come round again at a lower, narrower rim.
constexpr float kFunnelTurns = 3.f;
}  // namespace

// Each further ring is this much wider than the one inside it. The step is what keeps two rings from
// reading as one blob at the default sizes: at 1.15 the gap between the front icons of adjacent rings
// is smaller than one icon, and by 0.6 the outer ring has left the work area. RingFor's `outer` and
// the per-slot radius below both come from this number, so the bounding box cannot end up smaller
// than the rings it is supposed to cover.
constexpr float kOrbitRingStep = 0.42f;

Application::RingShape Application::RingFor(const MonitorInfo& m) const {
    const float wh = m.work.h > 0 ? float(m.work.h) : 1.f;
    const float H = m.px.h > 0 ? float(m.px.h) : 1.f;
    RingShape s;
    // Radii and sizes are fractions of the work-area height, which is the unit the ring was laid out
    // in from the start: it keeps the ellipse the same shape on a 1080p laptop and on this 4K panel.
    // The two settings ride on top of those fractions as percentages of this same reference layout
    // (100 = what is written here), the way the status card's own scale does.
    const float ring = wpm_.settings().orbitScalePct / 100.f;
    const float icon = wpm_.settings().orbitIconPct / 100.f;
    s.rx = 0.30f * wh * ring;
    s.ry = 0.16f * wh * ring;    // flat, but not flatter: at 0.11 the icons at each extreme merged into
                                 // one blob. Scaled with rx so a bigger ring is the same view, further.
    s.iconPx = 0.085f * wh * icon;
    // Each further ring is 42% wider than the one inside it (see kOrbitRingStep above).
    s.outer = 1.f + float(std::max(1, wpm_.settings().orbitRings) - 1) * kOrbitRingStep;
    // The funnel's own geometry. `stack` is how far the top and bottom layers sit from the centre and
    // `ry` stops being the ring's height and becomes one rim's, deliberately flat: at the ellipse's own
    // height the rims interleave and there is no stack to see. The step wants `kFunnelGap` icon
    // heights; what it gets is that or whatever fits in 95% of the work area, whichever is smaller -
    // layers the user cannot see are not layering.
    s.stack = 0.f;
    if (wpm_.settings().orbitShape == kOrbitFunnel) {
        const float n = float(std::max(2, int(orbitCount_)));
        const float room = 0.95f * wh * 0.5f - s.iconPx * 0.5f;
        s.stack = std::min(kFunnelGap * s.iconPx * (n - 1.f) * 0.5f,
                           room / (1.f + kFunnelRim / (n - 1.f)));
        s.ry = kFunnelRim * s.stack / (n - 1.f);
    }
    // Unset means the middle of the work area, which is where the reference's figure stands. Set means
    // wherever the user dragged it, in monitor heights from this monitor's top-left - the same units
    // and the same "negative means nobody said" convention as the two panels.
    const float autoX = float(m.work.x - m.px.x) + float(m.work.w) * 0.5f;
    const float autoY = float(m.work.y - m.px.y) + float(m.work.h) * 0.5f;
    const auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    s.cx = wpm_.settings().orbitX >= 0.f ? clamp(wpm_.settings().orbitX * H, 0.f, float(m.px.w)) : autoX;
    s.cy = wpm_.settings().orbitY >= 0.f ? clamp(wpm_.settings().orbitY * H, 0.f, float(m.px.h)) : autoY;
    return s;
}

RECT Application::OrbitScreenRect(const MonitorInfo& m) const {
    const RingShape s = RingFor(m);
    const float pad = s.iconPx * 0.5f;   // the front icons stick out past the ellipse itself
    // The outermost ring's radii, not the first ring's, and on the vertical axis the funnel's layers
    // on top of that: with 环数 > 1 or 漏斗 the click layer and the drag handle have to reach the
    // icons that are actually furthest out.
    const float ex = s.rx * s.outer, ey = (s.ry + s.stack) * s.outer;
    return RECT{ LONG(m.px.x + s.cx - ex - pad), LONG(m.px.y + s.cy - ey - pad),
                 LONG(m.px.x + s.cx + ex + pad), LONG(m.px.y + s.cy + ey + pad) };
}

bool Application::ArmOrbitDrag(bool on, std::string& error) {
    PanelDrag& d = orbitDrag_;
    if (!on) {
        d.win.Destroy();
        d.monitor = -1;
        return true;
    }
    if (d.monitor >= 0) return true;
    if (!orbitOn_ || !iconsBuilt_ || !renderer_.orbitReady()) {
        error = "the shortcut ring is not on the desktop, so there is nothing to drag";
        return false;
    }
    POINT cp{};
    GetCursorPos(&cp);
    const MonitorInfo* pick = monitors_.byIndex(0);
    for (auto& mm : monitors_.list())
        if (cp.x >= mm.px.x && cp.x < mm.px.x + mm.px.w &&
            cp.y >= mm.px.y && cp.y < mm.px.y + mm.px.h) pick = &mm;
    if (!pick) { error = "no monitor to drag on"; return false; }
    const MonitorInfo m = *pick;
    const RECT work{ m.px.x, m.px.y, m.px.x + m.px.w, m.px.y + m.px.h };
    const float H = float(m.px.h);
    // The layer is the ring's bounding box, so the dashed outline is the ring's own footprint rather
    // than a rectangle invented for the handle.
    const RingShape s0 = RingFor(m);
    const float bw = (s0.rx * s0.outer + s0.iconPx * 0.5f) * 2.f / H,
                bh = ((s0.ry + s0.stack) * s0.outer + s0.iconPx * 0.5f) * 2.f / H;
    if (!d.win.Create(GetModuleHandleW(nullptr), OrbitScreenRect(m), work,
                      L"SmartWallpaper shortcut ring - drag to move",
                      [this, idx = m.index, bw, bh](float x, float y, bool commit) -> RECT {
        const MonitorInfo* mm = monitors_.byIndex(size_t(idx));
        if (!mm) return RECT{};
        // The layer reports where its top-left wants to be; the ring is stored by its centre, so the
        // two halves of the box are what turn one convention into the other.
        wpm_.SetOrbitPos(x + bw * 0.5f, y + bh * 0.5f);
        const RECT r = OrbitScreenRect(*mm);
        if (commit) {
            std::string e;
            wpm_.Save(e);
            ArmOrbitDrag(false, e);
        }
        return r;
                      }, error)) {
        Warn(MOD, "orbit drag layer: {}", error);
        return false;
    }
    d.monitor = m.index;
    d.rect = OrbitScreenRect(m);
    Info(MOD, "orbit drag layer armed on {} at {}x{}+{},{}", m.tag,
         d.rect.right - d.rect.left, d.rect.bottom - d.rect.top, d.rect.left, d.rect.top);
    return true;
}

void Application::RebuildOrbitOrder() {
    orbitOrder_.clear();
    const auto& items = icons_.items();
    for (size_t i = 0; i < items.size(); ++i) orbitOrder_.push_back(int(i));
    const auto& s = wpm_.settings();
    if (!s.orbitPicked) return;      // nobody chose: the whole desktop, in the order it was read in
    // Walk the desktop, not the pick list: that keeps the ring's order by name no matter what order
    // the ticks came in, and a shortcut the user has since deleted simply drops out instead of leaving
    // a slot that points at nothing.
    std::vector<int> kept;
    for (int i : orbitOrder_) {
        const std::string name = ToUtf8(items[size_t(i)].label);
        if (std::find(s.orbitPicks.begin(), s.orbitPicks.end(), name) != s.orbitPicks.end())
            kept.push_back(i);
    }
    orbitOrder_ = std::move(kept);
}

bool Application::ArmOrbitHit(std::string& error) {
    if (orbitHitMonitor_ >= 0) return true;
    if (!orbitOn_ || !iconsBuilt_ || !renderer_.orbitReady()) {
        error = "the shortcut ring is not on the desktop, so there is nothing to click";
        return false;
    }
    // The layer belongs to one monitor, on the same grounds as the two panel drag layers above it: the
    // ring is one global layout, and a second layer would have to be told a second position.
    const DesktopHost& host = desktop_.host();
    POINT cp{};
    GetCursorPos(&cp);
    const MonitorInfo* pick = monitors_.byIndex(0);
    for (auto& mm : monitors_.list())
        if (cp.x >= mm.px.x && cp.x < mm.px.x + mm.px.w &&
            cp.y >= mm.px.y && cp.y < mm.px.y + mm.px.h) pick = &mm;
    if (!pick) { error = "no monitor to click on"; return false; }
    const MonitorInfo m = *pick;
    const RECT area{ m.px.x, m.px.y, m.px.x + m.px.w, m.px.y + m.px.h };
    if (!iconHit_.Create(GetModuleHandleW(nullptr), host.iconHost, host.defView, area,
                         [this](int slot) { LaunchRingSlot(slot); }, error)) {
        Warn(MOD, "orbit click layer: {}", error);
        return false;
    }
    orbitHitMonitor_ = int(m.index);
    Info(MOD, "orbit click layer armed on {}, icon layer {}", m.tag, host.describe());
    // Which shortcut each slot is. The order comes from a sorted enumeration of the Desktop folders,
    // so it is not something anyone can guess from the picture, and "what would this click start" has
    // to be answerable without starting it.
    std::string names;
    for (int k = 0; k < OrbitSlotsShown(); ++k)
        names += std::format(" {}={}", k, ToUtf8(icons_.items()[size_t(orbitOrder_[k])].label));
    Info(MOD, "orbit slots{}", names);
    return true;
}

// The same line, for the moment the answer changes without the layer being re-armed: after a pick the
// log has to say what a click on slot 2 would start now, not what it would have started before.
void Application::LogOrbitSlots() {
    if (!iconHit_.active()) return;
    std::string names;
    for (int k = 0; k < OrbitSlotsShown(); ++k)
        names += std::format(" {}={}", k, ToUtf8(icons_.items()[size_t(orbitOrder_[k])].label));
    Info(MOD, "orbit slots{}", names);
}

// How many ring slots the desktop can actually fill right now: what the user asked for - one count
// per ring, times the number of rings - capped by the constant buffer's row count and by how many
// shortcuts the current selection holds. The rings fill from the inside out, so a selection smaller
// than perRing*rings leaves the outer rings short instead of dropping icons: 43 icons at 17 per ring
// and 3 rings is 17 + 17 + 9, not 17.
int Application::OrbitSlotsShown() const {
    const int want = int(orbitCount_) * std::max(1, wpm_.settings().orbitRings);
    return std::max(0, std::min(want, std::min(int(orbitOrder_.size()), kMaxOrbitIcons)));
}

void Application::LaunchRingSlot(int slot) {
    static_assert(kMaxOrbitIcons == IconHitWindow::kMaxSlots,
                  "the ring's constant buffer, its shader loop and its click boxes must all be the "
                  "same number of slots");
    const auto& items = icons_.items();
    if (slot < 0 || size_t(slot) >= orbitOrder_.size()) {
        Warn(MOD, "orbit click on slot {} but the ring is holding {} shortcuts", slot,
             orbitOrder_.size());
        return;
    }
    const ShellIcon& it = items[size_t(orbitOrder_[size_t(slot)])];
    SHELLEXECUTEINFOW si{ sizeof(si) };
    // SEE_MASK_FLAG_NO_UI: the rule for this surface is that a failure goes to the log and never in
    // front of the user as a dialog, and ShellExecute's own error box would break that.
    si.fMask = SEE_MASK_FLAG_NO_UI;
    si.lpFile = it.lnkPath.c_str();      // the .lnk, not the target: the shell resolves the working
    si.nShow = SW_SHOWNORMAL;            // directory and any arguments out of it
    if (!ShellExecuteExW(&si)) {
        Warn(MOD, "orbit click: {} '{}' did not start: {}", slot, ToUtf8(it.lnkPath), GetLastError());
        return;
    }
    Info(MOD, "orbit click: slot {} '{}' started", slot, ToUtf8(it.label));
}

bool Application::SetIconsHidden(bool hide, std::string& error) {
    const HWND list = desktop_.host().iconList;
    if (!list) { error = "this desktop has no icon list window to hide"; return false; }
    const bool ok = hide ? iconlayer::Hide(list, error) : iconlayer::Show(list, error);
    if (!ok) return false;
    iconsHidden_ = hide;
    iconListSeen_ = hide ? list : nullptr;
    // The note is what makes recovery independent of this process getting to run anything: a kill
    // leaves it, and the next start reads it before it draws a frame.
    std::error_code ec;
    if (hide) {
        std::ofstream mark(iconMarkPath_.c_str(), std::ios::trunc);
        mark << "hidden";
    } else std::filesystem::remove(std::filesystem::path(iconMarkPath_), ec);
    Info(MOD, "desktop icons {}: list=0x{:X} shell HideIcons={} {}", hide ? "hidden" : "shown",
         (uintptr_t)list, iconlayer::ShellState(), error);
    return true;
}

void Application::HealIconLayer() {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(iconMarkPath_), ec)) return;
    const HWND list = desktop_.host().iconList;
    if (!list) {
        Warn(MOD, "an earlier run noted the icons hidden but this desktop has no icon list to restore");
        return;
    }
    std::string e;
    iconlayer::Show(list, e);
    iconsHidden_ = false;
    iconListSeen_ = nullptr;
    std::filesystem::remove(std::filesystem::path(iconMarkPath_), ec);
    Info(MOD, "icons restored from the note a previous run left: shell HideIcons={} {}",
         iconlayer::ShellState(), e);
}

// The ring's own clock, once per frame for every screen at once. Held still while the pointer is on an
// icon: at a 45 s period the front of the ring travels about thirty pixels a second, which is a target
// nobody can land a click on, and a ring that stops under the hand stops being a moving target.
void Application::AdvanceOrbit(ULONGLONG nowMs) {
    if (!orbitOn_ || !iconsBuilt_) {
        orbitLastMs_ = nowMs;
        orbitHover_ = -1;
        return;
    }
    // Polled every frame whether or not the angle moves: the rectangles the layer holds came from last
    // frame, and a pointer that walks onto a stopped ring still has to light it up.
    POINT cp{};
    GetCursorPos(&cp);
    orbitHover_ = iconHit_.active() ? iconHit_.SlotAt(cp) : -1;

    const ULONGLONG dtMs = orbitLastMs_ <= nowMs ? nowMs - orbitLastMs_ : 0;
    orbitLastMs_ = nowMs;
    // A gap longer than a quarter second is not a frame - an unlock, a stalled loop - and advancing the
    // angle across it teleports the icons, which is what the frame delta already refuses to do.
    if (dtMs > 250) return;
    // Also held while the ring is being dragged: a handle that slides out from under the pointer
    // because the ring itself is moving is the jitter the card's drag layer already had to be fixed for.
    // And while the tray is holding it - which is not the same as 转一圈=0, because the period he set
    // survives the hold and the ring picks up from the angle it stopped on.
    if (orbitPeriodS_ <= 0.0 || orbitHover_ >= 0 || orbitDrag_.monitor >= 0 || orbitHeld_) return;
    orbitPhase_ += float(double(dtMs) * 0.001 * 6.28318530718 / orbitPeriodS_);
    if (orbitPhase_ > 6.28318530718f) orbitPhase_ = std::fmod(orbitPhase_, 6.28318530718f);
}

namespace {
// Where one slot of the ring sits, relative to the centre, in that ring's own radii.
// `angle` is the absolute position around the figure (the ring's phase included, so the whole figure
// turns together); `frac` is which icon this is along the path, 0 at the first and 1 just before the
// loop closes, and it is what the funnel measures its height with - an icon keeps its layer as the
// figure turns, rather than the funnel unwinding.
//
// `depth` is how far below the centre the icon sits as a fraction of the whole vertical extent, which
// is what the size ramp and the back-of-ring fade read. On the ellipse - where `stack` is 0 - it is
// sin(angle), the number the ring has been measured with since it was made, so the other two shapes
// cannot move it.
struct ShapePos { float ox = 0.f, oy = 0.f, depth = 0.f; };

ShapePos ShapePoint(int shape, float angle, float frac, float rx, float ry, float stack) {
    float r = 1.f, lift = 0.f;
    if (shape == kOrbitFunnel) {
        // A filter: wide at the top, narrowing to the neck at the bottom, each icon on its own
        // perspective ellipse at its own height.
        r = 1.f + (kFunnelNeck - 1.f) * frac;
        lift = stack * (2.f * frac - 1.f);
    } else if (shape == kOrbitStar) {
        // Ten vertices 36 degrees apart, alternating outer and inner: the five-pointed star. 0.382 is
        // 1/phi^2, the inner radius of the regular pentagram, so the arms are the straight edges of
        // the figure rather than the rounded lobes a cosine modulation would give.
        float u = angle / kTau;
        u -= std::floor(u);
        const float t = u * 10.f;
        const int i = int(t);
        const float f = t - float(i);
        const float r0 = (i % 2 == 0) ? 1.f : 0.382f;
        const float r1 = (i % 2 == 0) ? 0.382f : 1.f;
        r = r0 + (r1 - r0) * f;
    }
    const float se = std::sin(angle);
    const float oy = lift + ry * r * se;
    return { rx * r * std::cos(angle), oy, oy / (ry + stack) };
}
}  // namespace

void Application::DrawSlot(Slot& s, double nowSeconds) {
    std::string err;
    if (!s.window.EnsureSize(dev_.dev.Get(), dev_.comp.Get(), err)) {
        s.error = err;
        Warn(MOD, "{}: {}", s.window.monitor().tag, err);
        return;
    }
    RenderSurface& surf = s.window.surface();
    if (!surf.Begin(err)) {
        s.error = err;
        ++s.drawErrors;
        if (s.drawErrors <= 3 || s.drawErrors % 300 == 0) Warn(MOD, "{}: Begin failed: {}", s.window.monitor().tag, err);
        return;
    }

    const MonitorInfo& m = s.window.monitor();
    FrameCB f{};
    FillWallClock(f);
    f.time = float(nowSeconds);
    // The simulation step is the time that actually passed between two draws of this slot, not
    // 1/what the frame budget says. With the budget-derived version a wallpaper's speed followed the
    // power cap: a 10 fps cap advanced 0.1 s per frame (and the package clamped it to 0.05, so the
    // snow fell at half speed), then raising the cap to 60 made it visibly accelerate on its own.
    float dt = s.lastDrawT >= 0.0 ? float(nowSeconds - s.lastDrawT) : 0.f;
    s.lastDrawT = nowSeconds;
    // A gap longer than a quarter second is not a frame: an unlock, a stalled loop, a cap that was 0.
    // Advancing the simulation across it teleports every particle; holding still costs one frame.
    if (dt > 0.25f) dt = 0.f;
    f.delta = s.clock.effectiveFps > 0 ? dt : 0.f;
    s.lastDelta = f.delta;
    f.frame = float(s.clock.frames % 100000);
    f.targetFps = float(s.clock.effectiveFps);
    f.resX = float(surf.width());
    f.resY = float(surf.height());
    f.invX = 1.f / f.resX;
    f.invY = 1.f / f.resY;
    f.mouseX = float(cursorPx_.x - m.px.x);
    f.mouseY = float(cursorPx_.y - m.px.y);
    f.mouseNX = f.mouseX * f.invX;
    f.mouseNY = f.mouseY * f.invY;
    Quality q = EffectiveQuality(s);
    f.quality = float(int(q));
    f.pixelRatio = m.scale;
    f.monitorIndex = float(m.index);
    f.monitorCount = float(monitors_.count());
    f.fpsMeasured = float(s.clock.measuredFps);
    f.renderScale = QualityScale(q);
    f.fit = float(int(wpm_.settings().imageFit));
    // What the panels cannot know about this monitor: where they go, and which border draws them. The
    // rectangles are resolved here rather than in the shaders because the drag layers have to cover
    // exactly the same pixels, and only this side knows the monitor, its taskbar strip and where the
    // user dropped what.
    {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        PanelRectFor(m, kPanelCard, x, y, w, h);
        f.hudX = x; f.hudY = y; f.hudW = w; f.hudH = h;
        f.hudFrame = float(wpm_.settings().hudFrame);
        PanelRectFor(m, kPanelChart, x, y, w, h);
        f.chartX = x; f.chartY = y; f.chartW = w; f.chartH = h;
        f.chartType = float(wpm_.settings().chartType + 1);   // 0 means "draw nothing" in the shader
        f.chartFrame = float(wpm_.settings().chartFrame);
    }
    // The ring's geometry, computed here in this monitor's device pixels rather than in the shader.
    // Making it clickable put a second reader on the same layout - the invisible window that turns a
    // pixel into a program - and two independent trigonometries drift apart, so one side computes and
    // both the shader (f.orbitIcon) and the window (the rects below) follow what is written here.
    if (orbitOn_ && iconsBuilt_) {
        const RingShape sh = RingFor(m);
        const int n = OrbitSlotsShown();
        // Quality can shrink the surface below the monitor, and the shader's px are surface pixels. The
        // click layer and the drag layer live in monitor pixels and take the unscaled numbers.
        const float scale = m.px.h > 0 ? float(surf.height()) / float(m.px.h) : 1.f;
        const bool clickable = orbitHitMonitor_ == int(m.index);
        f.orbitCount = float(n);
        f.orbitCols = float(icons_.cols());
        f.orbitRows = float(icons_.rows());
        f.orbitFadeBack = 0.25f;
        f.orbitHover = float(clickable ? orbitHover_ : -1);
        const int shape = wpm_.settings().orbitShape;
        const bool funnel = shape == kOrbitFunnel;
        std::vector<IconHitRect> rects;
        const int perRing = std::max(1, int(orbitCount_));
        const float turns = funnel ? kFunnelTurns : 1.f;
        for (int k = 0; k < n; ++k) {
            // Which ring this slot is on and which place along it. The rings fill from the inside out,
            // so the last one is the short one when the selection does not divide evenly.
            const int g = k / perRing;
            const int inRing = std::min(perRing, n - g * perRing);
            const int j = k - g * perRing;
            const float step = kTau / float(inRing);
            // Turned by half a step of its own ring: two rings whose icons line up angle for angle
            // read as the spokes of one wheel, and the point of a second ring is to be seen as one.
            const float a = orbitPhase_ + float(j) * step * turns + float(g) * step * 0.5f;
            const float rs = 1.f + float(g) * kOrbitRingStep;
            const float rxr = sh.rx * rs, ryr = sh.ry * rs, rstack = sh.stack * rs;
            const ShapePos p = ShapePoint(shape, a, float(j) / float(inRing), rxr, ryr, rstack);
            const float px = sh.cx + p.ox, py = sh.cy + p.oy;
            const float depth = p.depth;                  // > 0 in front of the centre, < 0 behind
            // Nothing is culled: every slot is drawn, the back ones faded to a ghost by the shader, and
            // every drawn slot gets a click rectangle.
            // The artwork inside an atlas cell stops short of the cell edge (ShellIcons::kCellPad), so
            // the square the shader cuts on is that fraction larger than the icon the user sees. `half`
            // stays the visible size: it is what the click rectangle is made of, and a hit target that
            // reaches past the picture is the same dead-control bug pointing the other way.
            const float half = sh.iconPx * 0.5f * (0.55f + 0.45f * (depth + 1.f) * 0.5f);
            f.orbitIcon[k][0] = px * scale;
            f.orbitIcon[k][1] = py * scale;
            f.orbitIcon[k][2] = half / ShellIcons::kCellArt * scale;
            f.orbitIcon[k][3] = depth;
            // Which shortcut paints here. The click box above is the same slot number, so the picture,
            // the box and the program it starts all come from this one row of the table.
            f.orbitCell[k][0] = float(orbitOrder_[size_t(k)]);
            f.orbitCell[k][1] = f.orbitCell[k][2] = f.orbitCell[k][3] = 0.f;
            if (clickable)
                rects.push_back({ { m.px.x + int(px - half), m.px.y + int(py - half),
                                    m.px.x + int(px + half), m.px.y + int(py + half) }, k });
        }
        // Once a second, and only while something can actually be clicked: the probe that measures the
        // feature has to click the rectangles the engine believes it drew, and reading them off a
        // screenshot would be measuring a third hand at the same job. At a 45 s period an icon travels
        // about thirty pixels a second, so a line this old is still inside its own box.
        // This sits above the hand-off because the hand-off takes the vector by move.
        if (clickable && nowSeconds - orbitGeomLogT_ >= 1.0) {
            orbitGeomLogT_ = nowSeconds;
            std::string packed;
            for (const auto& r : rects)
                packed += std::format(" {}:{},{}+{}", r.slot, (r.px.left + r.px.right) / 2,
                                      (r.px.top + r.px.bottom) / 2, (r.px.right - r.px.left) / 2);
            Info(MOD, "orbit rects hover={} slot:cx,cy+half{}", orbitHover_, packed);
        }
        if (clickable) iconHit_.SetRects(std::move(rects));
    }
    // Keep each grab layer welded to its panel. Only when the rectangle actually moved: SetWindowPos
    // every frame would queue a message per frame for the whole time the desktop is being drawn.
    // And not while the user is holding it: that window is being moved by the pointer in the message
    // that decided where it goes, and a second mover working off last frame's rectangle is the jitter.
    for (int p = 0; p < kPanelCount; ++p) {
        PanelDrag& d = drag_[p];
        if (d.monitor != m.index || !d.win.active()) continue;
        const RECT want = PanelScreenRect(m, p);
        if (d.win.dragging()) {
            d.rect = want;              // keep the comparison honest for the frame after the drop
        } else if (want.left != d.rect.left || want.top != d.rect.top ||
                   want.right != d.rect.right || want.bottom != d.rect.bottom) {
            d.rect = want;
            d.win.Move(want);
        }
    }

    // The ring's own grab layer, welded to the ring the same way - and it does move, because the
    // bounding box follows the centre the pointer is dragging.
    {
        PanelDrag& d = orbitDrag_;
        if (d.monitor == m.index && d.win.active() && !d.win.dragging()) {
            const RECT want = OrbitScreenRect(m);
            if (want.left != d.rect.left || want.top != d.rect.top ||
                want.right != d.rect.right || want.bottom != d.rect.bottom) {
                d.rect = want;
                d.win.Move(want);
            }
        }
    }

    DrawArgs args = s.instance.MakeArgs(f, dev_.ctx.Get(), statsCB_);
    if (!renderer_.Render(dev_.ctx.Get(), surf.rtv(), surf.width(), surf.height(), args, err)) {
        s.error = err;
        surf.End(dev_.ctx.Get());
        Warn(MOD, "{}: draw failed: {}", m.tag, err);
        return;
    }
    // The card composites over the wallpaper inside this same surface, before EndDraw unlocks it -
    // one surface, one visual, one commit. The wallpaper's pass has already covered every pixel,
    // so there is nothing for the overlay to leave half-drawn.
    if (wpm_.settings().hud && renderer_.hudReady()) {
        if (!renderer_.DrawHud(dev_.ctx.Get(), surf.rtv(), surf.width(), surf.height(), err))
            Warn(MOD, "{}: status card: {}", m.tag, err);
    }
    if (wpm_.settings().chart && renderer_.chartReady()) {
        if (!renderer_.DrawChart(dev_.ctx.Get(), surf.rtv(), surf.width(), surf.height(), err))
            Warn(MOD, "{}: chart panel: {}", m.tag, err);
    }
    if (orbitOn_ && renderer_.orbitReady()) {
        if (!renderer_.DrawOrbit(dev_.ctx.Get(), surf.rtv(), surf.width(), surf.height(), err))
            Warn(MOD, "{}: icon orbit: {}", m.tag, err);
    }

    // One corner sample a second is enough to tell "animating" from "frozen", and it is the
    // only proof available when another window covers the desktop.
    if (nowSeconds - s.lastSampleT >= 1.0) {
        s.lastSampleT = nowSeconds;
        unsigned long long sum = 0;
        // A corner the overlay panels do not cover, so turning either one on cannot make a live
        // wallpaper look frozen: the sample would be reading a panel that changes once a second at
        // most, and usually not even that. It used to be "the corner diagonally opposite the card",
        // which stopped being true the day a panel could be dragged anywhere.
        UINT hudCornerSample = 0;
        {
            const auto& st = wpm_.settings();
            const float aspect = float(m.px.w) / float(m.px.h);
            const float s16 = 16.f / float(m.px.h);      // the sample box, in the panels' own units
            const bool cardOn = st.hud && renderer_.hudReady();
            const bool chartOn = st.chart && renderer_.chartReady();
            const auto covers = [&](float x, float y, float w, float h, UINT c) {
                const float cx = (c & 1) ? aspect - s16 : 0.f;
                const float cy = (c & 2) ? 1.f - s16 : 0.f;
                return x < cx + s16 && cx < x + w && y < cy + s16 && cy < y + h;
            };
            for (UINT c = 0; c < 4; ++c) {
                const bool hit = (cardOn && covers(f.hudX, f.hudY, f.hudW, f.hudH, c))
                              || (chartOn && covers(f.chartX, f.chartY, f.chartW, f.chartH, c));
                if (!hit) { hudCornerSample = c; break; }
            }
        }
        if (surf.SampleCorner(dev_.ctx.Get(), sum, 16, hudCornerSample)) {
            if (s.lastSample && sum != s.lastSample) ++s.changedSamples_;
            else if (s.lastSample) ++s.staticSamples_;
            s.lastSample = sum;
        }
        // The corner can be static while the simulation still runs (nothing on screen moved into
        // that corner), so the particle buffer is read back as a second, independent proof.
        if (s.instance.hasParticles()) {
            unsigned long long ph = 0;
            unsigned oob = 0;
            if (s.instance.particles().Sample(dev_.ctx.Get(), ph, oob, s.instance.particles().count())) {
                if (s.lastParticleHash_ && ph != s.lastParticleHash_) ++s.particleMovedSamples_;
                else if (s.lastParticleHash_) ++s.particleStaticSamples_;
                s.lastParticleHash_ = ph;
                s.particleOutOfBounds_ = oob;
            }
        }
    }
    surf.End(dev_.ctx.Get());
    s.error.clear();
}

std::string Application::LoopDebug() const {
    std::string out;
    for (auto& s : slots_)
        out += std::format("{}{}:eff={} due={} nxt={} last={}", out.empty() ? "" : " ", s.window.monitor().tag,
                           s.clock.effectiveFps, s.clock.nextDue > s.clock.windowStart ? 1 : 0,
                           s.clock.nextDue, s.clock.lastFrame);
    out += std::format(" | drawStage={}", sawDrawStage_);
    return out;
}

void Application::UpdateSnapshot() {
    Json r = Json::Object();
    r.set("ok", Json::Of(true));
    r.set("pid", Json::Of((long long)GetCurrentProcessId()));
    r.set("host", Json::Of(std::string(desktop_.host().methodName())));
    r.set("host_detail", Json::Of(desktop_.host().detail));
    // "which FFmpeg build is in this program" is a distribution obligation, not a debug detail, and it
    // has to be answerable from the running binary. An empty string means the renderer answering is
    // older than this window (the build now requires the LGPL shared libraries), so the UI says so
    // rather than claiming a build it cannot prove.
    r.set("ffmpeg", Json::Of(FfmpegBuildNote()));
    r.set("root", Json::Of(ToUtf8(wpm_.settings().root)));
    r.set("paused", Json::Of(paused_.load()));
    r.set("autostart", Json::Of(IsAutostartEnabled()));
    r.set("max_fps", Json::Of(wpm_.settings().globalMaxFps));
    // The settings window reads these two back to label the 画质档 field. Without them it always
    // falls back to "package", so picking an option changed the renderer but never the UI.
    r.set("quality", Json::Of(std::string(QualityName(wpm_.settings().globalQuality))));
    r.set("quality_is_global", Json::Of(wpm_.settings().useGlobalQuality));
    r.set("image_fit", Json::Of(std::string(ImageFitName(wpm_.settings().imageFit))));
    r.set("tray_alpha", Json::Of(wpm_.settings().trayAlpha));
    r.set("hud", Json::Of(wpm_.settings().hud));
    r.set("hud_corner", Json::Of(std::string(HudCornerName(wpm_.settings().hudCorner))));
    r.set("hud_scale", Json::Of(wpm_.settings().hudScalePct));
    r.set("hud_frame", Json::Of(std::string(HudFrameName(wpm_.settings().hudFrame))));
    // The dragged position, if there is one. null means the card is on a corner preset, which is what
    // lets the settings window show 「自定义」 only after the user has actually moved it.
    {
        Json pos = Json::Array();
        if (wpm_.settings().hudX >= 0.f && wpm_.settings().hudY >= 0.f) {
            pos.push(Json::Of(double(wpm_.settings().hudX)));
            pos.push(Json::Of(double(wpm_.settings().hudY)));
            r.set("hud_pos", std::move(pos));
        } else {
            r.set("hud_pos", Json::Null());
        }
    }
    r.set("hud_drag", Json::Of(drag_[kPanelCard].monitor >= 0));
    // The chart panel, reported the same way as the card: enabled and ready apart, and its position
    // null while it is still on a corner.
    r.set("chart", Json::Of(wpm_.settings().chart));
    r.set("chart_type", Json::Of(std::string(ChartTypeName(wpm_.settings().chartType))));
    r.set("chart_corner", Json::Of(std::string(HudCornerName(wpm_.settings().chartCorner))));
    r.set("chart_scale", Json::Of(wpm_.settings().chartScalePct));
    r.set("chart_frame", Json::Of(std::string(HudFrameName(wpm_.settings().chartFrame))));
    {
        Json pos = Json::Array();
        if (wpm_.settings().chartX >= 0.f && wpm_.settings().chartY >= 0.f) {
            pos.push(Json::Of(double(wpm_.settings().chartX)));
            pos.push(Json::Of(double(wpm_.settings().chartY)));
            r.set("chart_pos", std::move(pos));
        } else {
            r.set("chart_pos", Json::Null());
        }
    }
    r.set("chart_drag", Json::Of(drag_[kPanelChart].monitor >= 0));
    r.set("chart_ready", Json::Of(renderer_.chartReady()));
    // How much history the line can actually draw, so the settings window can say "4 s of 16 s"
    // instead of showing a line that is mostly absence and letting it look like a broken gauge.
    {
        int filled = 0;
        for (int i = 0; i < kChartSamples; ++i)
            if (chartRing_[i * 4] >= 0.f) ++filled;
        r.set("chart_history", Json::Of(filled));
    }
    // Ready is reported apart from enabled: a card the user switched on that the renderer could
    // not prepare would otherwise read as working, with nothing on the screen and no reason why.
    r.set("hud_ready", Json::Of(renderer_.hudReady()));
    // The icon layer as the two authorities see it: what we asked for, and what explorer's own
    // 显示桌面图标 value says. Reading them apart is the only way to notice the two disagreeing, which
    // is the failure this switch has to be auditable for.
    r.set("icons_hidden", Json::Of(iconsHidden_));
    r.set("icons_shell_hide", Json::Of(iconlayer::ShellState() == 1));
    r.set("icons_list_visible", Json::Of(!iconlayer::Hidden(desktop_.host().iconList)));
    // The ring, with its two readiness halves apart: the icons may have been read while the shader
    // could not be compiled, or the other way round, and a single "available" flag would let the
    // settings window show a switch that does nothing in either case.
    r.set("orbit", Json::Of(orbitOn_));
    r.set("orbit_icons", Json::Of(iconsBuilt_));
    r.set("orbit_ready", Json::Of(renderer_.orbitReady()));
    r.set("orbit_clickable", Json::Of(iconHit_.active()));
    r.set("orbit_period_s", Json::Of(int(orbitPeriodS_)));
    r.set("orbit_held", Json::Of(orbitHeld_));
    // Per ring and in total, because they are different numbers as soon as there is more than one
    // ring: `orbit_per_ring` is what the slider holds, `orbit_shown` is how many the desktop actually
    // filled after the selection and the constant buffer's 64 rows had their say.
    r.set("orbit_per_ring", Json::Of(int(orbitCount_)));
    r.set("orbit_shown", Json::Of(OrbitSlotsShown()));
    r.set("orbit_rings", Json::Of(wpm_.settings().orbitRings));
    r.set("orbit_shape", Json::Of(std::string(OrbitShapeName(wpm_.settings().orbitShape))));
    r.set("orbit_slots", Json::Of(int(icons_.countFound())));
    // Percentages of the reference layout, so the settings window can put the number it shows back
    // onto a slider instead of guessing at a default.
    r.set("orbit_scale", Json::Of(wpm_.settings().orbitScalePct));
    r.set("orbit_icon_scale", Json::Of(wpm_.settings().orbitIconPct));
    // What the picker needs. `orbit_pickable` is the honest ceiling for 环上数量 once a subset
    // exists - it is the size of the selection, not of the desktop. `orbit_icon_list` is the desktop's
    // own list with each name marked picked or not, in the order the ring would walk it, under its own
    // key: it is an array and `orbit_icons` above is the bool "the atlas was built", and the two
    // cannot share a name because the object keeps one value per key - sharing one silently deleted
    // the bool, and the settings window then read "the icons are not there" on a working ring (2026-10-11).
    r.set("orbit_pickable", Json::Of(int(orbitOrder_.size())));
    // False means "nobody picked": the ring is showing the whole desktop and the picker's title says
    // so, which is a different sentence from "you picked all 43 of them" even though the ring looks
    // identical either way.
    r.set("orbit_picked", Json::Of(wpm_.settings().orbitPicked));
    {
        const auto& items = icons_.items();
        const auto& s = wpm_.settings();
        Json list = Json::Array();
        for (size_t i = 0; i < items.size(); ++i) {
            const std::string name = ToUtf8(items[i].label);
            Json e = Json::Object();
            e.set("i", Json::Of(int(i)));
            e.set("name", Json::Of(name));
            e.set("picked", Json::Of(!s.orbitPicked || std::find(s.orbitPicks.begin(),
                                                                 s.orbitPicks.end(), name) !=
                                     s.orbitPicks.end()));
            list.push(std::move(e));
        }
        r.set("orbit_icon_list", std::move(list));
    }
    r.set("orbit_drag", Json::Of(orbitDrag_.monitor >= 0));
    if (wpm_.settings().orbitX >= 0.f && wpm_.settings().orbitY >= 0.f) {
        Json pos = Json::Array();
        pos.push(Json::Of(double(wpm_.settings().orbitX)));
        pos.push(Json::Of(double(wpm_.settings().orbitY)));
        r.set("orbit_pos", std::move(pos));
    } else {
        r.set("orbit_pos", Json::Null());
    }
    if (!orbitError_.empty()) r.set("orbit_error", Json::Of(orbitError_));
    if (!hudError_.empty()) r.set("hud_error", Json::Of(hudError_));
    r.set("power", Json::Of(power_.StatusLine()));
    {
        Json rot = Json::Object();
        rot.set("on", Json::Of(rotate_.on));
        rot.set("interval_min", Json::Of(rotate_.intervalMin));
        rot.set("monitor", Json::Of(rotate_.monitor));
        rot.set("index", Json::Of(rotate_.index));
        Json pool = Json::Array();
        for (auto& id : rotate_.pool) pool.push(Json::Of(id));
        rot.set("pool", std::move(pool));
        rot.set("next_in_s", Json::Of(rotate_.on && rotate_.nextAt > GetTickCount64()
                                                ? long long((rotate_.nextAt - GetTickCount64()) / 1000)
                                                : 0));
        r.set("rotate", std::move(rot));
    }
    r.set("power_caps", power_.SaveCaps());
    {
        Json pv = Json::Object();
        pv.set("on", Json::Of(preview_.on));
        pv.set("id", Json::Of(preview_.id));
        // How many frames the channel has written that belong to a wallpaper other than the one now
        // asked for - the two in-flight copies right after a switch. A reader that only looks at `id`
        // would call those the new picture; this field is what says "the channel is still draining".
        pv.set("draining", Json::Of((long long)preview_.draining));
        // Whether the preview instance has a picture at all yet. A video instance needs its decoder to
        // deliver before there is anything to sample, and the frames written in between are the shader
        // reading an empty plane pair - the green picture. A reader that measures those reports a
        // colour defect that does not exist, so the count has to be readable from outside.
        if (VideoSource* v = preview_.inst ? preview_.inst->video() : nullptr) {
            pv.set("video_frames", Json::Of((long long)v->delivered()));
            pv.set("video_pos", Json::Of(v->position_s()));
            // "paused" as the page experiences it: either the player is stopped, or the pass is holding
            // its picture because the player is the wallpaper's own.
            pv.set("video_paused", Json::Of(v->paused() || preview_.held));
        }
        {
            const ULONGLONG now = GetTickCount64();
            if (preview_.reportAtMs == 0) {
                preview_.reportAtMs = now;
                preview_.drewAtLastReport = preview_.drew;
            }
            if (const ULONGLONG el = now - preview_.reportAtMs; el >= 1000) {
                preview_.reportedFps = double(preview_.drew - preview_.drewAtLastReport) * 1000.0 / double(el);
                preview_.reportAtMs = now;
                preview_.drewAtLastReport = preview_.drew;
            }
        }
        pv.set("fps", Json::Of(preview_.reportedFps));
        pv.set("avg_ms", Json::Of(preview_.clock.avgMs));
        pv.set("worst_ms", Json::Of(preview_.clock.worstMs));
        pv.set("drew", Json::Of((long long)preview_.drew));
        pv.set("dropped", Json::Of((long long)preview_.dropped));
        pv.set("shm_frames", Json::Of((long long)preview_.writer.frames()));
        // ---- 临时探针（方案 1 代价实测）：每帧回读的分布 + 这一帧到底有多大 ----
        {
            std::vector<double> win;
            const unsigned n = std::min(preview_.copyIdx, Preview::kCopyWin);
            win.reserve(n);
            for (unsigned i = 0; i < n; ++i) win.push_back(preview_.copyWin[i]);
            std::sort(win.begin(), win.end());
            if (!win.empty()) {
                pv.set("copy_ms_p50", Json::Of(win[n / 2]));
                pv.set("copy_ms_p95", Json::Of(win[(n * 95) / 100]));
            }
            pv.set("copy_ms_max", Json::Of(preview_.copyMaxMs));
            pv.set("copy_ms_mean", Json::Of(preview_.copyIdx ? preview_.copySumMs / double(preview_.copyIdx)
                                                             : 0.0));
            pv.set("copy_n", Json::Of((long long)preview_.copyIdx));
            pv.set("frame_mb", Json::Of(double(preview_.w) * double(preview_.h) * 4.0 / 1048576.0));
        }
        r.set("preview", std::move(pv));
    }
    r.set("uptime_s", Json::Of(double(GetTickCount64() - startedMs_) / 1000.0));
    {
        Json cat = Json::Array();
        for (auto& p : wpm_.catalog()) {
            Json m = Json::Object();
            m.set("id", Json::Of(p.id()));
            m.set("name", Json::Of(p.name()));
            m.set("fps", Json::Of(p.baseFps()));
            m.set("quality", Json::Of(std::string(QualityName(p.defaultQuality()))));
            m.set("category", Json::Of(p.category()));
            // The page an imported picture came from, empty for a file he picked. The online page reads
            // this back instead of keeping its own ledger: "已经在库里" has to mean what the catalog
            // says it means - after a restart, and after he deletes the package by hand.
            m.set("origin", Json::Of(p.origin()));
            m.set("resolution", p.srcWidth() > 0 && p.srcHeight() > 0
                                     ? Json::Of(std::format("{} x {}", p.srcWidth(), p.srcHeight()))
                                     : Json::Of(std::string("generated")));
            m.set("params", Json::Of((long long)p.params().members().size()));
            m.set("textures", Json::Of((long long)p.textures().size()));
            m.set("particles", Json::Of((long long)p.particleCount()));
            // What kind of package this is, so the window can label a clip without opening it - and so
            // the tray knows a still has nothing to pause.
            m.set("type", Json::Of(std::string(p.isVideo() ? "video"
                                              : p.hasParticles() ? "particles"
                                              : p.isImage() ? "image" : "shader")));
            // A still picture's own file. The hero used to show it through the preview pass, which is
            // a 1920x1080 stream - so in a window wider than that the photo was softer than the file
            // sitting next to the manifest. He asked for the file instead (2026-10-12); the live pass
            // stays for the packages that actually move.
            if (p.isImage() && !p.isVideo() && !p.textures().empty())
                m.set("file", Json::Of(ToUtf8((std::filesystem::path(p.dir())
                                               / p.textures()[0].file).wstring())));
            if (p.isVideo()) {
                m.set("duration_s", Json::Of(p.srcDuration()));
                m.set("loop", Json::Of(p.videoLoop()));
            }
            auto it = thumbs_.find(p.id());
            if (it != thumbs_.end() && it->second.ok) {
                m.set("thumb", Json::Of(ToUtf8(it->second.still)));
                m.set("thumb_large", Json::Of(ToUtf8(it->second.large)));                // The picture on disk is replaced whenever the renderer rebuilds it, but a window that
                // already loaded the old bytes keeps them. The stamp is what lets the window notice.
                std::error_code ec;
                long long stampMs = 0;
                if (const auto w = std::filesystem::last_write_time(it->second.still, ec); !ec)
                    stampMs = w.time_since_epoch().count() / 10000;  // the clock's own epoch; this is
                    // only ever compared against itself, to give a rebuilt picture a new cache key
                m.set("thumb_ms", Json::Of(stampMs));
                Json fr = Json::Array();
                for (auto& f : it->second.frames) fr.push(Json::Of(ToUtf8(f)));
                m.set("frames", std::move(fr));
                // How long each frame is on screen: the strip is a sample of one second, so the
                // player has to pace itself by this and not by a rate it guesses.
                m.set("frame_ms", Json::Of(it->second.frameMs));
            } else if (it != thumbs_.end()) {
                m.set("thumb_error", Json::Of(it->second.error));
            }
            cat.push(std::move(m));
        }
        r.set("catalog", std::move(cat));
    }

    static ULONGLONG lastWallNs = 0, lastCpuNs = 0;
    MemSample mem = SampleProcess(lastWallNs, lastCpuNs);
    Json memj = Json::Object();
    memj.set("working_set_mb", Json::Of(mem.workingSetMb));
    memj.set("private_mb", Json::Of(mem.privateMb));
    memj.set("cpu_percent", Json::Of(mem.cpuPercent));
    // Named next to the number on purpose: a percent without its window is not a measurement.
    memj.set("cpu_window_s", Json::Of(mem.cpuWindowS));
    // The card's busy percent (whole machine) and this process's own share of it. Two numbers, two
    // denominators - see Engine/App/GpuMonitor.hpp. Read from the loop's one-second sample rather than
    // sampled here: GpuMonitor keeps state between calls about which readings had a window behind
    // them, and a builder invoked at the settings window's whim would disturb it.
    {
        const GpuUse g = gpuLast_;
        if (g.device >= 0.0) memj.set("gpu_percent", Json::Of(g.device));
        if (g.self >= 0.0) {
            memj.set("gpu_self_percent", Json::Of(g.self));
            memj.set("gpu_self_3d", Json::Of(g.self3d));
            memj.set("gpu_self_decode", Json::Of(g.selfDecode));
        }
    }
    r.set("process", std::move(memj));

    // The whole machine, same -1-means-no-answer rule: a field is left out rather than written as 0,
    // so `--ctl status` can be diffed against nvidia-smi and Get-Counter without a zero passing for a
    // measurement. cpu_pct and cpu_pct_pdh are one quantity from two instruments; if they ever part
    // company, one of the two paths is wrong and this is where you would see it.
    {
        const MachineStats& x = machineLast_;
        Json m = Json::Object();
        auto put = [&](const char* k, double v) { if (v >= 0.0) m.set(k, Json::Of(v)); };
        put("cpu_pct", x.cpuPct);
        put("cpu_pct_pdh", x.cpuPctPdh);
        put("cpu_user_pct", x.cpuUserPct);
        put("cpu_priv_pct", x.cpuPrivPct);
        put("cpu_queue", x.cpuQueue);
        put("mem_pct", x.memPct);
        put("mem_used_mb", x.memUsedMb);
        put("mem_total_mb", x.memTotalMb);
        put("commit_mb", x.commitMb);
        put("disk_read_bps", x.diskReadBs);
        put("disk_write_bps", x.diskWriteBs);
        put("disk_active_pct", x.diskActivePct);
        put("disk_free_gb", x.diskFreeGb);
        // The drive's own temperature and its own thresholds, so the number on the panel can be
        // checked against a second instrument without trusting the shader.
        put("disk_temp_c", x.diskTempC);
        put("disk_warn_c", x.diskWarnC);
        put("disk_crit_c", x.diskCritC);
        put("disk_sensors", x.diskSensors);
        put("mb_temp_c", x.mbTempC);
        put("disk_total_gb", x.diskTotalGb);
        put("net_recv_bps", x.netRecvBs);
        put("net_send_bps", x.netSendBs);
        put("uptime_s", x.uptimeS);
        put("process_count", x.processCount);
        put("gpu_util_pct", x.gpu.device);
        put("gpu_mem_util_pct", x.gpu.memUtilPct);
        put("gpu_mem_used_mb", x.gpu.memUsedMb);
        put("gpu_mem_reserved_mb", x.gpu.memReservedMb);
        put("gpu_mem_total_mb", x.gpu.memTotalMb);
        put("gpu_temp_c", x.gpu.tempC);
        put("gpu_power_w", x.gpu.powerW);
        put("gpu_clock_mhz", x.gpu.clockMHz);
        Json cores = Json::Array();
        for (unsigned i = 0; i < x.coreCount; ++i) cores.push(Json::Of(double(x.core[i])));
        m.set("cores", std::move(cores));
        m.set("valid", Json::Of((long long)x.valid));
        r.set("machine", std::move(m));
    }

    Json mons = Json::Array();
    for (auto& s : slots_) {
        const MonitorInfo& m = s.window.monitor();
        Json j = Json::Object();
        j.set("tag", Json::Of(m.tag));
        j.set("device", Json::Of(ToUtf8(m.device)));
        j.set("px", Json::Of(std::format("{},{},{}x{}", m.px.x, m.px.y, m.px.w, m.px.h)));
        j.set("refresh_hz", Json::Of((long long)m.refreshHz));
        j.set("dpi", Json::Of((long long)m.dpi));
        j.set("adapter", Json::Of(ToUtf8(dev_.AdapterName(m.adapterIndex))));
        j.set("wallpaper", Json::Of(s.wallpaperId));
        j.set("assign_fps", Json::Of(wpm_.FpsForMonitor(m)));
        j.set("quality", Json::Of(std::string(QualityName(EffectiveQuality(s)))));
        j.set("state", Json::Of(std::string(PowerManager::StateName(s.decision.state))));
        j.set("state_reason", Json::Of(s.decision.why));
        j.set("cap_fps", Json::Of(s.decision.cap));
        j.set("target_fps", Json::Of(s.clock.targetFps));
        j.set("effective_fps", Json::Of(s.clock.effectiveFps));
        j.set("measured_fps", Json::Of(std::format("{:.2f}", s.clock.measuredFps)));
        j.set("avg_frame_ms", Json::Of(std::format("{:.2f}", s.clock.avgMs)));
        // The step the simulation actually got, in seconds. Its product with measured_fps is the
        // speed factor: 1.0 means the wallpaper runs at wall-clock speed whatever the cap says, and
        // 0.5 is the old budget-derived behaviour at a 10 fps cap.
        j.set("sim_delta_s", Json::Of(std::format("{:.5f}", s.lastDelta)));
        j.set("speed_factor", Json::Of(std::format("{:.3f}",
                        s.clock.measuredFps > 0.05 ? double(s.lastDelta) * s.clock.measuredFps : 0.0)));
        j.set("worst_frame_ms", Json::Of(std::format("{:.2f}", s.clock.worstMs)));
        // Late since the budget last moved: this is the one that counts hitches, because the two
        // numbers above keep a stale maximum and an average that follows the cap, not the delivery.
        j.set("late_frames", Json::Of((long long)s.clock.lateFrames));
        j.set("frames", Json::Of((long long)s.clock.frames));
        j.set("draw_errors", Json::Of((long long)s.drawErrors));
        // How often the budget actually moved. This is the number the debounce is judged by: the raw
        // classifier changed 24 times in 280 s before it existed.
        j.set("state_switches", Json::Of((long long)s.stateSwitches_));
        j.set("corner_samples_changed", Json::Of((long long)s.changedSamples_));
        j.set("corner_samples_static", Json::Of((long long)s.staticSamples_));
        j.set("particles", Json::Of((long long)s.instance.particles().count()));
        j.set("particle_moved_samples", Json::Of((long long)s.particleMovedSamples_));
        j.set("particle_static_samples", Json::Of((long long)s.particleStaticSamples_));
        j.set("particle_out_of_bounds", Json::Of((long long)s.particleOutOfBounds_));
        j.set("particle_positions_hash", Json::Of(std::format("{:016x}", s.lastParticleHash_)));
        // The video instrument: frames_delivered climbing at the clip's own rate is the direct proof
        // that a decoder is running, and video_output says which path this machine actually gave us.
        if (VideoSource* v = s.instance.video()) {
            j.set("video_pos", Json::Of(v->position_s()));
            j.set("video_duration", Json::Of(v->duration_s()));
            j.set("video_fps", Json::Of(v->fps()));
            j.set("video_size", Json::Of(std::format("{}x{}", v->width(), v->height())));
            j.set("video_paused", Json::Of(v->paused()));
            j.set("video_ended", Json::Of(v->ended()));
            j.set("video_frames_delivered", Json::Of((long long)v->delivered()));
            j.set("video_output", Json::Of(v->note()));
        }
        j.set("error", Json::Of(s.error));
        mons.push(std::move(j));
    }
    r.set("monitors", std::move(mons));

    std::string text = r.dump(1);
    unsigned long long particles = 0, moved = 0, statics = 0, oob = 0;
    for (auto& s : slots_) {
        particles += s.instance.particles().count();
        moved += s.particleMovedSamples_;
        statics += s.particleStaticSamples_;
        oob += s.particleOutOfBounds_;
    }
    // A particle wallpaper that reports 0 moved samples is the failure this line exists to catch:
    // the buffer is bound, the dispatch returns, and nothing on the GPU actually changed.
    std::string line = std::format("host={} slots={} paused={} ws={:.1f}MB cpu={:.2f}% {} particles={} simMoved={} "
                                   "simStatic={} outOfBounds={}",
                                   desktop_.host().methodName(), slots_.size(), paused_.load(), mem.workingSetMb,
                                   mem.cpuPercent, power_.StatusLine(), particles, moved, statics, oob);
    {
        std::lock_guard lk(mtx_);
        snapshot_ = text;
        statusLine_ = line;
    }
    if (tray_.installed()) {
        std::string tip = std::format("Wallpaper - {}", statusLine_.empty() ? line : statusLine_);
        if (tip.size() > 120) tip.resize(120);
        tray_.SetTip(tip);
    }
}

void Application::BuildThumbnails(bool onlyMissing) {
    for (auto& p : wpm_.catalog()) {
        if (onlyMissing) {
            auto it = thumbs_.find(p.id());
            if (it != thumbs_.end() && it->second.ok) continue;
        }
        std::string err;
        Thumbnailer th;
        // A video card parks the decoder on a named moment, and reaching that moment from the previous
        // keyframe costs several hundred milliseconds of decode. That wait is inside the grab, on this
        // thread, so it gets handed the same pump the switch uses: the desktop keeps drawing while the
        // card is built instead of holding one picture.
        th.setIdleHook([this] { DrainMessages(); DrawDue(); });
        // Previews are judged against the desktop, so they are told what the desktop is.
        if (const MonitorInfo* m = monitors_.byIndex(0)) th.setDesignSize(m->px.w, m->px.h);
        // Reuse the compiled instance when this wallpaper is what a monitor is already showing;
        // otherwise compile a throwaway one, which is what a fresh install has to do anyway.
        // A video on screen used to be excluded here, because grabbing its card called ShowFrameAt on
        // the live player - that parks the player and nothing but a later seek clears it, so the
        // desktop froze on a still picture at every start (measured 2026-10-07: delivered stuck at 84,
        // the thread parked in cv_.wait at 0% CPU, one `--ctl video seek 0.5` brought it back). The
        // rule is carried by the flag now instead: a live instance is grabbed as it plays, which also
        // avoids the throwaway decoder (the second 4K player we measured under the deleted Media
        // Foundation backend cost ~900 MB of surface pool for the few seconds of the grab, and the
        // process never gave that memory back).
        WallpaperInstance* use = nullptr;
        for (auto& s : slots_)
            if (s.wallpaperId == p.id()) use = &s.instance;
        th.setLiveSource(use != nullptr);
        // A refused grab must not take the tile down with it. Thumbnailer leaves the previous picture on
        // disk when it refuses a uniform frame ("let the previous card stand"), but the catalog only
        // advertises a card while the record says ok - so the window got no `thumb` at all, its Image
        // bound to "", and the octagon mask underneath painted the card white (measured 2026-10-09:
        // local_05 after a rebuild that came back luma 0..0). Stand the surviving files back up.
        auto keepPrevious = [this](const std::string& id, ThumbSet& t) {
            std::error_code ec;
            if (t.still.empty() || t.large.empty() || !std::filesystem::exists(t.still, ec)
                || !std::filesystem::exists(t.large, ec)) return;
            t.ok = true;
            Info(MOD, "thumbnail for {}: the rebuild was refused, keeping the previous card on disk", id);
        };
        WallpaperInstance temp;
        if (use) {
            if (!th.Build(dev_, renderer_, *use, thumbDir_, p.id(), thumbs_[p.id()], err)) {
                Warn(MOD, "thumbnail for {} failed: {}", p.id(), err);
                keepPrevious(p.id(), thumbs_[p.id()]);
            }
        } else if (temp.Prepare(wpm_.settings().root, p, dev_, err)) {
            if (!th.Build(dev_, renderer_, temp, thumbDir_, p.id(), thumbs_[p.id()], err)) {
                Warn(MOD, "thumbnail for {} failed: {}", p.id(), err);
                keepPrevious(p.id(), thumbs_[p.id()]);
            }
        } else {
            thumbs_[p.id()].error = err;
            Warn(MOD, "thumbnail for {} could not prepare: {}", p.id(), err);
        }
    }
}

namespace {
// 640x360 is what the saved strip already was, so the big picture loses nothing it had before; the
// cost scales with the bytes, and at 960x540 (2 MB a frame) the settings window could only finish
// 17-19 of the 30 produced frames per second.
constexpr UINT kPreviewW = 3840, kPreviewH = 2160;
constexpr int kPreviewFps = 30;
// The settings window sends a beat once a second while it shows the preview. If it dies, closes or
// crashes, the second render pass has to stop by itself rather than burn GPU forever.
constexpr ULONGLONG kPreviewBeatTimeoutMs = 3000;
} // namespace

bool Application::StartPreview(const std::string& id, std::string& error) {
    const WallpaperPackage* pkg = wpm_.find(id);
    if (!pkg) {
        error = "no such wallpaper: " + id;
        return false;
    }
    Preview& p = preview_;
    if (p.on && p.id == id) {
        p.lastBeatMs = GetTickCount64();
        return true;
    }
    // The wallpaper being replaced has to stop its media before the new one starts, and a video's warm
    // copy has to go with it. A parked video instance keeps its decoder thread group, its frame buffers
    // and two plane textures - measured at about 200 MB for a 4K clip - and browsing through a dozen
    // took the process to 2.5 GB with 154 threads while only one of them was ever being drawn. Pictures
    // and shaders stay cached, because re-preparing those is the hitch this cache exists to avoid.
    if (p.inst && !p.shared) {
        p.inst->SetMediaActive(false);
        if (p.inst->hasVideo()) {
            for (auto it = p.cache.begin(); it != p.cache.end(); ++it)
                if (it->second.get() == p.inst) {
                    p.cache.erase(it);
                    break;
                }
        }
    }
    // When the big picture is the wallpaper already on a screen, draw *that* instance into the preview
    // surface instead of opening a second decoder for the same file. Measured 2026-10-07 on the 4K60
    // clip this desktop runs: a second h264_cuvid for the same clip costs +79 points of one core
    // (15.3% -> 94.7%) and +176 MB of working set (310 -> 486 MB), and both passes are driven off the
    // same clock, so the picture would have been identical anyway. Particles are excluded because the
    // draw path steps the simulation, and doing that twice per frame would run it at double speed.
    p.shared = false;
    for (auto& s : slots_) {
        if (s.wallpaperId != id || !s.instance.hasVideo() || s.instance.hasParticles()) continue;
        p.inst = &s.instance;
        p.shared = true;
        break;
    }
    if (!p.writer.open() && !p.writer.OpenWriter(error)) return false;

    if (!p.rt || p.w != kPreviewW || p.h != kPreviewH) {
        p.rt = nullptr;
        p.rtv = nullptr;
        for (int i = 0; i < 2; ++i) { p.stg[i] = nullptr; p.q[i] = nullptr; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kPreviewW;
        td.Height = kPreviewH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_.dev->CreateTexture2D(&td, nullptr, &p.rt))) { error = "preview render target"; return false; }
        if (FAILED(dev_.dev->CreateRenderTargetView(p.rt.Get(), nullptr, &p.rtv))) { error = "preview rtv"; return false; }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < 2; ++i) {
            if (FAILED(dev_.dev->CreateTexture2D(&td, nullptr, &p.stg[i]))) { error = "preview staging texture"; return false; }
            D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
            if (FAILED(dev_.dev->CreateQuery(&qd, &p.q[i]))) { error = "preview query"; return false; }
        }
        p.w = kPreviewW;
        p.h = kPreviewH;
        p.slotState[0] = p.slotState[1] = Preview::Free;
        p.slotHash[0] = p.slotHash[1] = 0;
    }

    if (!p.shared && (!p.inst || p.inst->id() != id)) {
        auto it = p.cache.find(id);
        if (it == p.cache.end()) {
            // Bounded: a library of imported pictures must not grow the cache without limit. The
            // instance in use is dropped with the rest, and re-prepared on the next look.
            if (p.cache.size() >= 12) {
                for (auto& entry : p.cache) ReapLater(std::move(entry.second));
                p.cache.clear();
                p.inst = nullptr;
            }
            // The hero switching is the same warm-up, and it is the surface he watches most: the
            // carousel lands on another wallpaper every few seconds while the settings window is open.
            // The preview itself is not drawn while this waits - `p.inst` is about to be replaced, and
            // drawing it in that state is the "missing rtv or shader" failure.
            std::unique_ptr<VideoSource> warm = WarmWhileDrawing(*pkg, error, false);
            if (pkg->isVideo() && !warm) { error = "preview prepare: " + error; return false; }
            auto fresh = std::make_unique<WallpaperInstance>();
            if (!fresh->Prepare(wpm_.settings().root, *pkg, dev_, error, std::move(warm))) {
                error = "preview prepare: " + error;
                return false;
            }
            Info(MOD, "preview prepared {} (cache {} of 12)", id, p.cache.size() + 1);
            it = p.cache.emplace(id, std::move(fresh)).first;
        }
        p.inst = it->second.get();
    }
    p.id = id;
    p.idHash = IdHash(id.c_str());
    p.error.clear();
    p.on = true;
    // A new target is never the picture the page stopped on. Re-beating the same id keeps the pause
    // (that path returns before this line).
    p.held = false;
    p.lastBeatMs = GetTickCount64();
    // A shared instance is already active because it is on a screen; asking again would be harmless
    // but asking it to stop later (StopPreview) is not, so that side checks p.shared.
    if (!p.shared) p.inst->SetMediaActive(true);
    pacer_.ConfigureSlot(p.clock, kPreviewFps, 0);
    Info(MOD, "preview on: {} ({}x{} at {} fps{})", id, kPreviewW, kPreviewH, kPreviewFps,
         p.shared ? ", sharing the live decoder" : "");
    return true;
}

void Application::StopPreview(const char* why) {
    if (!preview_.on) return;
    preview_.on = false;
    // Never park a shared instance: it is on a screen, and parking it is exactly the bug that made
    // the desktop freeze on a still picture (see the live-source note in BuildThumbnails).
    if (preview_.inst && !preview_.shared) preview_.inst->SetMediaActive(false);
    // A pause the page put on the pass dies with the pass. The player itself is only ever stopped by
    // this window when it belongs to the preview alone, and such an instance is reaped just below.
    preview_.held = false;
    pacer_.ConfigureSlot(preview_.clock, 0, 0);
    // The textures and the compiled instance stay: the browse page comes back to the same wallpaper
    // seconds later, and re-preparing a shader mid-interaction is a visible hitch for 6 MB of VRAM.
    // Video instances are the exception - each one parked holds a decoder thread, its frame buffers
    // and two plane textures (measured: tens of MB and a worker-thread group per 4K clip), and
    // browsing through a dozen of them is what took the process to 2.5 GB. Re-opening one costs the
    // next look at that clip, not the page.
    for (auto it = preview_.cache.begin(); it != preview_.cache.end();) {
        if (it->second->hasVideo()) {
            ReapLater(std::move(it->second));
            it = preview_.cache.erase(it);
        } else ++it;
    }
    preview_.inst = nullptr;
    preview_.shared = false;
    Info(MOD, "preview off ({}): drew={} dropped={}", why, preview_.drew, preview_.dropped);
}

void Application::DrawPreview(double nowSeconds) {
    Preview& p = preview_;
    if (!p.on || !p.inst || !p.rtv) return;
    std::string err;

    // Pick up any copy whose GPU work has finished since the last loop. Map() on a staging texture
    // waits for the GPU, and doing that mid-loop would stall the desktop wallpaper behind a 2 MB
    // readback - so each copy is fenced with an event query and collected whenever it is ready.
    // Collecting only "one frame later" dropped 59 of 150 frames: the driver is routinely a frame
    // behind, so a copy that was not done on the first look is kept, not thrown away.
    int freeSlot = -1;
    for (int i = 0; i < 2; ++i) {
        if (p.slotState[i] != Preview::CopyPending) {
            freeSlot = i;
            continue;
        }
        BOOL done = FALSE;
        const HRESULT hr = dev_.ctx->GetData(p.q[i].Get(), &done, sizeof(done), 0);
        if (FAILED(hr) || !done) continue;
        D3D11_MAPPED_SUBRESOURCE mp{};
        // ---- 临时探针：一次回读的真实毫秒（Map 会等 GPU，所以这段不是纯 memcpy）----
        static const LARGE_INTEGER pf = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER t0{};
        QueryPerformanceCounter(&t0);
        if (SUCCEEDED(dev_.ctx->Map(p.stg[i].Get(), 0, D3D11_MAP_READ, 0, &mp))) {
            // The slot's own hash, not p.idHash: this frame was rendered by whichever instance was
            // current when the copy was issued, which after a switch is the wallpaper the reader is
            // supposed to be able to refuse.
            p.writer.Write(p.w, p.h, static_cast<const BYTE*>(mp.pData), mp.RowPitch, GetTickCount64(),
                           p.slotHash[i]);
            if (p.slotHash[i] != p.idHash) ++p.draining;
            dev_.ctx->Unmap(p.stg[i].Get(), 0);
        }
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        {
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(pf.QuadPart);
            p.copyWin[p.copyIdx % Preview::kCopyWin] = ms;
            ++p.copyIdx;
            p.copySumMs += ms;
            if (ms > p.copyMaxMs) p.copyMaxMs = ms;
        }
        p.slotState[i] = Preview::Free;
        freeSlot = i;
    }

    // The page asked for a pause on a player that is also running his wallpaper. Nothing is drawn and
    // nothing is published, so the section keeps holding the frame he stopped on - and this has to sit
    // *after* the collection loop above, or the two staging slots would never be freed and every frame
    // after resuming would find nowhere to copy to.
    if (p.held) return;

    FrameCB f{};
    FillWallClock(f);
    // Same clock as the desktop slots, so the preview is the wallpaper at the same moment rather
    // than a second copy running at its own phase.
    f.time = float(nowSeconds);
    f.delta = 1.f / float(kPreviewFps);
    f.frame = float(p.clock.frames % 100000);
    f.targetFps = float(kPreviewFps);
    f.resX = float(p.w);
    f.resY = float(p.h);
    f.invX = 1.f / f.resX;
    f.invY = 1.f / f.resY;
    f.mouseNX = .5f;
    f.mouseNY = .5f;
    f.quality = 3.f;   // high: the preview should show the look the package asks for
    f.fit = float(int(wpm_.settings().imageFit));
    f.pixelRatio = 1.f;
    f.renderScale = 1.f;
    // Same rule as the thumbnails: pixel-sized features shrink with the surface, so the preview is
    // the same picture as the desktop rather than a six-times-bigger-snow version of it.
    if (const MonitorInfo* m = monitors_.byIndex(0); m && m->px.w > 0 && m->px.h > 0)
        f.sizeScale = std::min(float(p.w) / float(m->px.w), float(p.h) / float(m->px.h));

    DrawArgs args = p.inst->MakeArgs(f, dev_.ctx.Get(), statsCB_);
    // Until the decoder's first picture lands, a video instance's planes hold the video-black fill that
    // Nv12Uploader::Ensure gave them, so the draw comes out all black. On the desktop that is the lesser
    // evil. The preview is a *switch*, and the window is holding the frame he was looking at, so
    // publishing that black frame is the blink he calls 闪烁: measured 2026-10-08 by reading the section
    // across a re-target - one frame with mean RGB (1,0,1) and 100 % of its pixels near black, at
    // 199/203 ms into the switch, on every re-target that had to open a decoder, never on a clip whose
    // player was already live. Not publishing costs nothing: the next frame carries the picture, and
    // until then the section keeps the picture he was looking at.
    //
    // `args.srvCount` cannot be the test. MakeArgs binds video_->FrameY(), and Ensure builds the plane
    // textures before the decode thread starts (deliberately - it is what stopped a fresh plane from
    // reading as green), so srvCount is 2 from the very first draw and a zero-SRV state does not exist.
    // The decoder's own count is the only thing that knows whether a picture has been delivered.
    // Re-measured after this change: 0 empty-plane frames across 7 switches onto two clips, first frame
    // of the not-applied clip at 233-274 ms (144 ms of that is the container plus the decoder open).
    if (p.inst->hasVideo() && p.inst->video()->delivered() == 0) {
        if (!p.waitingFirstFrame) {
            p.waitingFirstFrame = true;
            Info(MOD, "preview {}: no decoded frame yet, publishing held back", p.id);
        }
        return;
    }
    p.waitingFirstFrame = false;
    if (!renderer_.Render(dev_.ctx.Get(), p.rtv.Get(), p.w, p.h, args, err)) {
        p.error = err;
        Warn(MOD, "preview draw failed: {}", err);
        return;
    }
    dev_.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    const int i = freeSlot;
    if (i < 0) {
        // Both copies are still in flight: the preview runs one frame behind for a moment rather
        // than the frame loop waiting on the GPU.
        ++p.dropped;
        p.error.clear();
        return;
    }
    dev_.ctx->Begin(p.q[i].Get());
    dev_.ctx->CopyResource(p.stg[i].Get(), p.rt.Get());
    dev_.ctx->End(p.q[i].Get());
    p.slotHash[i] = p.idHash;   // what is in this copy, as of the frame that produced it
    p.slotState[i] = Preview::CopyPending;
    ++p.drew;
    p.error.clear();
}

void Application::ProcessQueued() {
    // One command at a time, and never from inside another one.
    //
    // `preview` warms a clip on a worker thread and keeps the desktop alive meanwhile by pumping
    // messages (WarmWhileDrawing) - and that pump delivers WM_IPC_WAKE, which lands back here with a
    // second command running on top of the first one's half-finished state. Measured on 2026-10-11 at
    // 22:32:48: the outer command was StartPreview, which had just reaped the cached instances and left
    // `preview_.inst` pointing at one of them; the nested one built a status snapshot, which reads
    // `preview_.inst->video()`, and the VideoSource was already gone - an access violation reading
    // 0x6EEF at Application.cpp:1381, with StartPreview -> WarmWhileDrawing -> DrainMessages -> WndProc
    // -> ProcessQueued -> UpdateSnapshot below it.
    //
    // Returning is enough: Run() drains the queue on every iteration, so the outer command finishes and
    // the next frame takes what is left. A frame is 7-21 ms; a warm-up is 50-150 ms.
    if (inCommand_) {
        Warn(MOD, "command queue re-entered while running; {} deferred", queued_.size());
        if (deferNested_) return;
    }
    inCommand_ = true;
    struct Releaser {
        bool& f;
        ~Releaser() { f = false; }
    } rel{inCommand_};
    std::vector<Queued> todo;
    {
        std::lock_guard lk(mtx_);
        todo.swap(queued_);
    }
    for (auto& q : todo) {
        const std::string r = DoCommandLocal(q.action, q.arg, q.arg2);
        // Queued commands answer "queued" to the client, so without this a failure would be
        // invisible: the caller never sees the real result.
        if (r.find(R"("ok":false)") != std::string::npos) Warn(MOD, "command {}({},{}) -> {}", q.action, q.arg, q.arg2, r);
        else Info(MOD, "command {}({},{}) -> {}", q.action, q.arg, q.arg2, r);
    }
}

void Application::DrainMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quit_ = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Draw everything the pacer says is due and commit one composition. This is the whole presenting step,
// and it is a function rather than loop body because `ReloadSlot` has to keep doing it while a video
// file is opened on a worker thread - the alternative, which is what shipped until 2026-10-08, held the
// composited desktop on one picture for 100-180 ms at every switch to a video.
void Application::DrawDue(bool withPreview) {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    const double t = double(li.QuadPart - qpcStart_.QuadPart) / double(freq.QuadPart);

    bool drewAny = false;
    ++sawDrawStage_;
    for (auto& s : slots_) {
        if (!pacer_.SlotDue(s.clock)) { pacer_.SlotIdle(s.clock); continue; }
        DrawSlot(s, t);
        pacer_.SlotRendered(s.clock);
        ++drewTotal_;
        drewAny = true;
    }
    if (withPreview && preview_.on) {
        if (pacer_.SlotDue(preview_.clock)) {
            DrawPreview(t);
            pacer_.SlotRendered(preview_.clock);
        } else {
            pacer_.SlotIdle(preview_.clock);
        }
    }
    if (drewAny) {
        HRESULT hr = dev_.comp->Commit();
        if (FAILED(hr)) Warn(MOD, "composition commit: {}", HResultToString(hr));
    }
}

int Application::Run() {
    ULONGLONG startedAt = GetTickCount64();
    background_ = std::thread(&Application::BackgroundLoop, this);
    std::vector<FPSController::Slot*> clocks;
    UINT logCounter = 0;

    while (!quit_) {
        DrainMessages();
        if (quit_) break;
        ProcessQueued();

        ULONGLONG nowMs = GetTickCount64();
        power_.Poll(nowMs);
        TickRotation(nowMs);
        AdvanceOrbit(nowMs);
        if (nowMs - machineAtMs_ >= 1000) {
            machineAtMs_ = nowMs;
            // The drive's temperature is a device query rather than a counter read, and it moves on a
            // scale of minutes, so it is taken every five seconds and carried in between. It lands in
            // the same struct the chart reads, one field per sample, rather than being polled at the
            // frame rate by a shader that cannot tell the two apart.
            if (nowMs - diskTempAtMs_ >= 5000) {
                diskTempAtMs_ = nowMs;
                diskTemp_ = ReadDiskTemp(wpm_.settings().root);
                if (diskTemp_.tempC < 0.f && diskTemp_.why != diskTempWhy_)
                    Warn(MOD, "drive temperature unavailable: {}", diskTemp_.why);
                diskTempWhy_ = diskTemp_.why;
            }
            gpuLast_ = gpu_.Sample();
            machineLast_ = machine_.Sample(gpuLast_);
            machineLast_.diskTempC = diskTemp_.tempC;
            machineLast_.diskWarnC = diskTemp_.warnC;
            machineLast_.diskCritC = diskTemp_.critC;
            machineLast_.diskSensors = diskTemp_.sensors;
            FillStatsCB(machineLast_, statsCB_);
            PushChartSeries(chartRing_.data(), machineLast_);
            memcpy(statsCB_.series, chartRing_.data(), sizeof statsCB_.series);
        }
        if (preview_.on && nowMs - preview_.lastBeatMs > kPreviewBeatTimeoutMs)
            StopPreview("settings window stopped beating");

        if (nowMs - lastHostCheckMs_ > 5000) {
            lastHostCheckMs_ = nowMs;
            POINT cp{};
            GetCursorPos(&cp);
            cursorPx_ = cp;
            if (!cl_.noDesktop) {
                desktop_.Recheck();
                // Two ways the icons can come back without us, and the switch has to follow both
                // rather than silently disagree with the screen. explorer rebuilding the layer (its own
                // restart, a theme change) hands back a different window, so the hide is re-applied;
                // the user's own 显示桌面图标 click re-shows the same window, and that wins - our
                // switch goes off and the crash note goes away, because overruling a man who just
                // asked for his icons back is not a thing a wallpaper should do.
                if (iconsHidden_) {
                    const HWND list = desktop_.host().iconList;
                    std::string e;
                    if (list && list != iconListSeen_) {
                        SetIconsHidden(true, e);
                    } else if (list && !iconlayer::Hidden(list)) {
                        Info(MOD, "the icon layer came back without us (the shell's own menu?) - "
                                  "switch turned off");
                        iconsHidden_ = false;
                        iconListSeen_ = nullptr;
                        std::error_code ec;
                        std::filesystem::remove(std::filesystem::path(iconMarkPath_), ec);
                    }
                }
                for (auto& s : slots_) {
                    std::string e;
                    s.window.Rebind(desktop_.host(), e);
                }
            }
        }

        clocks.clear();
        for (auto& s : slots_) {
            const MonitorInfo& m = s.window.monitor();
            int want = 0;
            if (!paused_) {
                want = s.instance.package().baseFps();
                if (int byMonitor = wpm_.FpsForMonitor(m)) want = std::min(want, byMonitor);
                want = std::min(want, wpm_.settings().globalMaxFps);
                Quality q = EffectiveQuality(s);
                if (q == Quality::Battery) want = std::min(want, 30);
                if (q == Quality::Low) want = std::min(want, 30);
                if (q == Quality::Medium) want = std::min(want, 60);
            }
            const PowerManager::Decision raw = power_.Decide(m);
            if (raw.state != s.decision.state) {
                if (raw.state != s.candState) {
                    s.candState = raw.state;
                    s.candSinceMs = nowMs;
                }
                // A locked session or a powered-down monitor takes effect immediately: waiting a
                // second before stopping would mean drawing behind a lock screen.
                const bool urgent = raw.state == PowerState::Locked || raw.state == PowerState::DisplayOff;
                if (urgent || nowMs - s.candSinceMs >= Slot::kStateHoldMs) {
                    s.decision = raw;
                    s.decision.why += std::format("（保持 {:.0f} ms 后换档，本槽第 {} 次）",
                                                  double(nowMs - s.candSinceMs), ++s.stateSwitches_);
                }
            }
            if (s.decision.state != raw.state)
                s.decision.why += std::format("（原始判定 {}，只持续了 {:.0f} ms，未到 {:.0f} ms 不改预算）",
                                              PowerManager::StateName(raw.state),
                                              double(nowMs - s.candSinceMs), double(Slot::kStateHoldMs));
            if (cl_.noPower && s.decision.cap < want) {
                s.decision.why += " (caps bypassed by --no-power)";
                s.decision.cap = want;
            }
            const int budget = want == 0 ? 0 : std::min(want, s.decision.cap);
            pacer_.ConfigureSlot(s.clock, budget, m.refreshHz);
            // A video wallpaper that is not drawing must also stop decoding: left running it burns a
            // hardware decode session and a GPU slot behind a fullscreen game that asked for zero frames.
            s.instance.SetMediaActive(budget > 0);
            clocks.push_back(&s.clock);
        }
        // The preview has its own budget on purpose: while a fullscreen app has the desktop capped at
        // zero frames, the settings window is the foreground window and its preview must keep moving.
        if (preview_.on) clocks.push_back(&preview_.clock);

        if (cl_.selftestSeconds > 0 && nowMs - startedAt >= ULONGLONG(cl_.selftestSeconds) * 1000) {
            // Checked before any continue below: a paused wallpaper (fullscreen game) must still
            // honour its own deadline.
            Stop("self-test finished");
            continue;
        }
        LONGLONG due = pacer_.EarliestDue(clocks);
        ++loopIters_;
        if (nowMs - spinWindowStart_ > 1000) {
            Info(MOD, "loop: {} iters/s drew={} due={} now={} slots=[{}]", loopIters_, drewTotal_, due,
                 pacer_.Now(), LoopDebug());
            loopIters_ = 0;
            spinWindowStart_ = nowMs;
        }
        if (!due) {
            if (nowMs - lastSnapshotMs_ > 2000) {
                lastSnapshotMs_ = nowMs;
                UpdateSnapshot();
            }
            // A plain Sleep here would not service the queue at all: with a fullscreen app up,
            // Explorer's messages to our window would then wait the whole 200 ms.
            pacer_.WaitSlice(0.2); // nothing may draw; keep reacting to unlock within a fifth of a second
            continue;
        }
        if (pacer_.WaitUntil(due)) continue;

        DrawDue();

        if (nowMs - lastSnapshotMs_ > 1000) {
            lastSnapshotMs_ = nowMs;
            UpdateSnapshot();
            if (++logCounter % 5 == 0) {
                std::lock_guard lk(mtx_);
                Info(MOD, "{}", statusLine_);
                for (auto& s : slots_)
                    Info(MOD, "  {}", FPSController::Describe(s.clock, s.window.monitor().tag + " " + s.wallpaperId +
                                                                 " " + PowerManager::StateName(s.decision.state)));
            }
        }

    }
    UpdateSnapshot();
    {
        std::lock_guard lk(mtx_);
        Info(MOD, "final snapshot: {}", snapshot_);
    }
    return 0;
}

std::string Application::Command(const std::string& line) {
    std::string err;
    auto j = Json::Parse(line, err);
    if (!j || !j->isObject()) return std::format(R"({{"ok":false,"error":"bad json: {}" }})", err);
    const Json& r = *j;
    std::string cmd = r.strOr("cmd", "");
    std::string arg = r.strOr("arg", "");
    std::string arg2 = r.strOr("arg2", "");

    if (cmd == "status") {
        std::lock_guard lk(mtx_);
        return snapshot_.empty() ? R"({"ok":true,"snapshot":"none"})" : snapshot_;
    }
    if (cmd == "list") {
        std::string out = wpm_.StatusJson().dump(1);
        return out;
    }
    if (cmd == "quit") {
        Stop("ipc quit");
        return R"({"ok":true,"result":"quitting"})";
    }
    {
        std::lock_guard lk(mtx_);
        queued_.push_back(Queued{cmd, arg, arg2});
    }
    if (win_) PostMessageW(win_, WM_IPC_WAKE, 0, 0);
    return std::format(R"({{"ok":true,"queued":"{}"}})", cmd);
}

std::string Application::Snapshot() const {
    std::lock_guard lk(mtx_);
    return snapshot_;
}

std::string Application::DoCommandLocal(const std::string& action, const std::string& arg, const std::string& arg2) {
    std::string result;
    if (action == "apply") {
        std::string monitor = arg2.empty() ? "all" : arg2;
        wpm_.SetAssignment(monitor, arg);
        // The text is this thread's own state and costs microseconds; the file is 8-43 ms of disk on a
        // machine with something scanning every write, which was a fifth of the held picture at a switch.
        // What the deferral buys back costs a window of a few seconds in which a crash loses the last
        // applied wallpaper - the queue is drained on a clean quit, so only a crash can drop it.
        std::string cfg = wpm_.ConfigText();
        PostBackground([this, text = std::move(cfg)] {
            std::string e;
            if (!wpm_.WriteConfigText(text, e)) Warn(MOD, "config save failed: {}", e);
        });
        int applied = 0;
        for (auto& s : slots_) {
            MonitorInfo m = s.window.monitor();
            if (monitor != "all" && monitor != m.tag && monitor != std::to_string(m.index)) continue;
            std::string e;
            if (ReloadSlot(s, arg, e)) ++applied;
            else {
                s.error = e;
                result = std::format(R"({{"ok":false,"error":"{}"}})", e);
            }
        }
        if (result.empty()) result = std::format(R"({{"ok":true,"applied":{},"wallpaper":"{}"}})", applied, arg);
    } else if (action == "pause") {
        paused_ = true;
        result = R"({"ok":true,"paused":true})";
    } else if (action == "resume") {
        paused_ = false;
        result = R"({"ok":true,"paused":false})";
    } else if (action == "quality") {
        wpm_.SetGlobalQuality(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"quality":"{}"}})", arg);
    } else if (action == "fit") {
        // An unrecognised name keeps the current mode, so the reply echoes what is now in force
        // rather than what was asked for.
        wpm_.SetImageFit(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"image_fit":"{}"}})", ImageFitName(wpm_.settings().imageFit));
    } else if (action == "taskbar") {
        // A percentage of transparency, 0 handing the bar back to Windows. The reply carries both
        // numbers because the two are not the same thing: 0..100 is what the user set, the window
        // alpha is what explorer was told, and the mapping has a floor of 1 on purpose.
        const int pct = arg == "off" ? 0 : std::atoi(arg.c_str());
        wpm_.SetTrayAlpha(pct);
        std::string e;
        wpm_.Save(e);
        trayFx_.Apply(wpm_.settings().trayAlpha);
        result = std::format(R"({{"ok":true,"tray_alpha":{},"window_alpha":{}}})",
                             wpm_.settings().trayAlpha, TrayTransparency::ToAlpha(wpm_.settings().trayAlpha));
    } else if (action == "hud") {
        // On/off only. `arg` is read the way the tray reads it - "on", "true", "1" - because a
        // command line user types one of those and a config editor writes the other.
        const bool on = arg == "on" || arg == "true" || arg == "1";
        wpm_.SetHud(on);
        std::string e;
        wpm_.Save(e);
        // Switching the card off while its drag layer is up would leave a click target standing over
        // a desktop with nothing in it - the same thing arming refuses to do, just from the other end.
        if (!on) ArmDrag(kPanelCard, false, e);
        result = std::format(R"({{"ok":true,"hud":{}}})", wpm_.settings().hud ? "true" : "false");
    } else if (action == "hudpos") {
        // An unrecognised corner keeps the one in force, so the reply says what the desktop is doing
        // rather than what was asked for.
        const int corner = wpm_.SetHudCorner(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"hud_corner":"{}"}})", HudCornerName(corner));
    } else if (action == "hudscale") {
        wpm_.SetHudScale(std::atoi(arg.c_str()));
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"hud_scale":{}}})", wpm_.settings().hudScalePct);
    } else if (action == "hudframe") {
        // An unrecognised name keeps the style in force, so the reply says what is on the screen.
        const int style = wpm_.SetHudFrame(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"hud_frame":"{}"}})", HudFrameName(style));
    } else if (action == "huddrag") {
        // The drag switch is deliberately not saved: an armed layer that never got dropped is a
        // topmost window over someone's desktop, and it should not come back next boot.
        const bool on = arg == "on" || arg == "true" || arg == "1";
        std::string e;
        if (!ArmDrag(kPanelCard, on, e)) {
            result = std::format(R"({{"ok":false,"error":"{}"}})", e);
        } else {
            result = std::format(R"({{"ok":true,"hud_drag":{}}})",
                                 drag_[kPanelCard].monitor >= 0 ? "true" : "false");
        }
    } else if (action == "hudmove") {
        // The same units the card is laid out in: screen heights from the monitor's top-left. The
        // engine clamps it, so a client that asks for the middle of the next monitor gets the edge.
        float x = std::strtof(arg.c_str(), nullptr), y = std::strtof(arg2.c_str(), nullptr);
        if (x < 0.f || y < 0.f) {
            result = R"({"ok":false,"error":"hudmove needs x y, both >= 0"})";
        } else {
            wpm_.SetHudPos(x, y);
            std::string e;
            wpm_.Save(e);
            result = std::format(R"({{"ok":true,"hud_pos":[{},{}]}})",
                                 wpm_.settings().hudX, wpm_.settings().hudY);
        }
    } else if (action == "hudhome") {
        wpm_.ClearHudPos();
        std::string e;
        wpm_.Save(e);
        ArmDrag(kPanelCard, false, e);
        result = std::format(R"({{"ok":true,"hud_corner":"{}"}})",
                             HudCornerName(wpm_.settings().hudCorner));
    } else if (action == "chart") {
        const bool on = arg == "on" || arg == "true" || arg == "1";
        wpm_.SetChart(on);
        std::string e;
        wpm_.Save(e);
        if (!on) ArmDrag(kPanelChart, false, e);
        result = std::format(R"({{"ok":true,"chart":{}}})", wpm_.settings().chart ? "true" : "false");
    } else if (action == "charttype") {
        const int t = wpm_.SetChartType(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"chart_type":"{}"}})", ChartTypeName(t));
    } else if (action == "chartpos") {
        const int corner = wpm_.SetChartCorner(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"chart_corner":"{}"}})", HudCornerName(corner));
    } else if (action == "chartscale") {
        wpm_.SetChartScale(std::atoi(arg.c_str()));
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"chart_scale":{}}})", wpm_.settings().chartScalePct);
    } else if (action == "chartframe") {
        const int style = wpm_.SetChartFrame(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"chart_frame":"{}"}})", HudFrameName(style));
    } else if (action == "chartdrag") {
        const bool on = arg == "on" || arg == "true" || arg == "1";
        std::string e;
        if (!ArmDrag(kPanelChart, on, e)) {
            result = std::format(R"({{"ok":false,"error":"{}"}})", e);
        } else {
            result = std::format(R"({{"ok":true,"chart_drag":{}}})",
                                 drag_[kPanelChart].monitor >= 0 ? "true" : "false");
        }
    } else if (action == "chartmove") {
        float x = std::strtof(arg.c_str(), nullptr), y = std::strtof(arg2.c_str(), nullptr);
        if (x < 0.f || y < 0.f) {
            result = R"({"ok":false,"error":"chartmove needs x y, both >= 0"})";
        } else {
            wpm_.SetChartPos(x, y);
            std::string e;
            wpm_.Save(e);
            result = std::format(R"({{"ok":true,"chart_pos":[{},{}]}})",
                                 wpm_.settings().chartX, wpm_.settings().chartY);
        }
    } else if (action == "charthome") {
        wpm_.ClearChartPos();
        std::string e;
        wpm_.Save(e);
        ArmDrag(kPanelChart, false, e);
        result = std::format(R"({{"ok":true,"chart_corner":"{}"}})",
                             HudCornerName(wpm_.settings().chartCorner));
    } else if (action == "icons") {
        // The shell's own icon layer, which the ring stands in for. Not persisted for the same reason
        // the ring is not: a desktop that lost its shortcuts because this program was killed should not
        // lose them again by its own choice on the next start.
        const bool hide = arg == "on" || arg == "true" || arg == "1" || arg == "hide";
        std::string ierr;
        const bool ok = SetIconsHidden(hide, ierr);
        result = std::format(R"({{"ok":{},"icons_hidden":{},"shell_hideicons":{},"error":"{}"}})",
                             ok ? "true" : "false", iconsHidden_ ? "true" : "false",
                             iconlayer::ShellState(), ierr);
    } else if (action == "orbitdrag") {
        // Arming the layer is the whole switch: like the card's, it exists only while the user is
        // holding it, so the desktop's own clicks are unaffected the rest of the time.
        const bool on = arg == "on" || arg == "true" || arg == "1";
        std::string e;
        if (on) ArmOrbitDrag(true, e); else ArmOrbitDrag(false, e);
        result = std::format(R"({{"ok":true,"orbit_drag":{},"error":"{}"}})",
                             orbitDrag_.monitor >= 0 ? "true" : "false", e);
    } else if (action == "orbithome") {
        wpm_.ClearOrbitPos();
        std::string e;
        wpm_.Save(e);
        ArmOrbitDrag(false, e);
        result = R"({"ok":true,"orbit_custom":false})";
    } else if (action == "orbit") {
        // Not persisted, on the same grounds as the drag switch: a ring of the user's own shortcuts
        // sitting over their desktop, and an invisible window over their icon layer, should not come
        // back after a crash without being asked for.
        orbitOn_ = arg == "on" || arg == "true" || arg == "1";
        std::string herr;
        if (orbitOn_) ArmOrbitHit(herr); else { iconHit_.Destroy(); orbitHitMonitor_ = -1;
                                                 std::string de; ArmOrbitDrag(false, de); }
        result = std::format(R"({{"ok":true,"orbit":{},"icons_ready":{},"icons_drawn":{},"on_screen":{},"clickable":{},"hover":{}}})",
                             orbitOn_ ? "true" : "false", iconsBuilt_ ? "true" : "false",
                             icons_.countIconsDrawn(),
                             (orbitOn_ && iconsBuilt_ && renderer_.orbitReady()) ? "true" : "false",
                             iconHit_.active() ? "true" : "false", orbitHover_);
        if (orbitOn_ && !iconsBuilt_)
            Warn(MOD, "orbit asked for but no icon atlas: {}", orbitError_);
        if (orbitOn_ && !iconHit_.active())
            Warn(MOD, "orbit drawn but not clickable: {}", herr);
    } else if (action == "orbitdump") {
        // The atlas as the GPU actually holds it. Every icon-atlas bug so far - the rainbow stripes
        // from a flat copy into a 2D image, the 32 px ceiling, the ghost outlines that bleed in from
        // the neighbouring cell - was guessable from a screenshot and obvious from this file, and
        // reading it back costs one build rather than one build per hypothesis.
        std::string derr;
        if (!iconsBuilt_) {
            derr = orbitError_.empty() ? "no atlas built" : orbitError_;
        } else {
            std::vector<BYTE> px = icons_.pixels();
            const std::wstring out = wpm_.settings().root + L"\\cache\\orbit-atlas.png";
            if (!WriteImage(dev_.wic.Get(), out, icons_.atlasWidth(), icons_.atlasHeight(), px, derr))
                Warn(MOD, "atlas dump failed: {}", derr);
            else
                Info(MOD, "atlas dumped: {} ({}x{}, {} icons, {} px cells)", ToUtf8(out),
                     icons_.atlasWidth(), icons_.atlasHeight(), icons_.countFound(), ShellIcons::kCell);
        }
        result = std::format(R"({{"ok":{},"error":"{}"}})", derr.empty() ? "true" : "false", derr);
    } else if (action == "orbitlayer") {
        // The click layer on its own, for the case where it was refused at switch-on (the desktop host
        // is found a few seconds after startup, and arming is otherwise only reachable from `orbit`).
        std::string herr;
        const bool ok = arg == "off" ? (iconHit_.Destroy(), orbitHitMonitor_ = -1, true)
                                     : ArmOrbitHit(herr);
        result = std::format(R"({{"ok":{},"clickable":{},"error":"{}"}})", ok ? "true" : "false",
                             iconHit_.active() ? "true" : "false", herr);
    } else if (action == "orbitspeed") {
        // Seconds per revolution; 0 holds the ring still. Clamped at the slow end because a period
        // below a couple of seconds makes each icon flicker in and out of the centre object faster
        // than the eye can follow.
        const double p = std::atof(arg.c_str());
        orbitPeriodS_ = (p <= 0.0) ? 0.0 : std::max(3.0, p);
        result = std::format(R"({{"ok":true,"orbit_period_s":{}}})", orbitPeriodS_);
    } else if (action == "orbithold") {
        // Not persisted, on the same grounds as the ring itself: a ring the desktop is not showing
        // after a restart should not come back frozen. The period is left exactly as it was, so this
        // row only ever stops and starts the turning.
        orbitHeld_ = arg == "on" || arg == "true" || arg == "1";
        result = std::format(R"({{"ok":true,"orbit_held":{},"orbit_period_s":{}}})",
                             orbitHeld_ ? "true" : "false", orbitPeriodS_);
    } else if (action == "orbitcount") {
        // Clamped rather than refused: the ring's cost is the per-pixel loop over this many icons, so
        // the ceiling is a performance decision and belongs here, not with the caller. It is also
        // kMaxOrbitIcons divided by the rings, because that many per ring is all the constant buffer
        // and the shader's loop have rows for - and because a per-ring count the drawn total cannot
        // reach would leave the slider able to ask for something the ring never shows.
        const int n = std::atoi(arg.c_str());
        const int cap = kMaxOrbitIcons / std::max(1, wpm_.settings().orbitRings);
        orbitCount_ = (UINT)std::max(1, std::min(cap, n));
        result = std::format(R"({{"ok":true,"orbit_per_ring":{},"orbit_shown":{}}})",
                             orbitCount_, OrbitSlotsShown());
    } else if (action == "orbitrings") {
        // Saved, unlike the period and the count: how many rings the shortcuts are spread over is how
        // the user wants the ring to look next time. Taking the rings up also pulls the per-ring count
        // down when the two together would not fit, so the number the slider shows is the number drawn.
        wpm_.SetOrbitRings(std::atoi(arg.c_str()));
        const int cap = kMaxOrbitIcons / wpm_.settings().orbitRings;
        if (int(orbitCount_) > cap) orbitCount_ = (UINT)cap;
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"orbit_rings":{},"orbit_per_ring":{},"orbit_shown":{}}})",
                             wpm_.settings().orbitRings, orbitCount_, OrbitSlotsShown());
    } else if (action == "orbitshape") {
        // By name: `star`, `funnel`, `ellipse`. Saved with the rings for the same reason, and the
        // reply carries the name now in force so a misspelling echoes what is on the screen.
        const int idx = wpm_.SetOrbitShape(arg);
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"orbit_shape":"{}","orbit_shown":{}}})",
                             OrbitShapeName(idx), OrbitSlotsShown());
    } else if (action == "orbitscale" || action == "orbiticonscale") {
        // Saved, unlike the period and the count: these two are how the user wants the ring to look
        // next time, not a knob to try out.
        const bool ring = action == "orbitscale";
        if (ring) wpm_.SetOrbitScale(std::atoi(arg.c_str()));
        else wpm_.SetOrbitIconScale(std::atoi(arg.c_str()));
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"{}":{}}})", ring ? "orbit_scale" : "orbit_icon_scale",
                             ring ? wpm_.settings().orbitScalePct : wpm_.settings().orbitIconPct);
    } else if (action == "orbitpicks") {
        // `all` hands the ring back to the whole desktop, `none` is an empty ring the user chose, and
        // anything else is the list itself separated by '|'. Names rather than indexes: the desktop is
        // enumerated in display-name order, so adding or deleting a shortcut next door would slide an
        // index onto somebody else's program, while a name just drops out. Saved, like the sizes.
        if (arg == "all") {
            wpm_.ClearOrbitPicks();
        } else {
            std::vector<std::string> picks;
            if (arg != "none") {
                size_t p = 0;
                while (p < arg.size()) {
                    const size_t q = arg.find('|', p);
                    const std::string one = arg.substr(p, q == std::string::npos ? q : q - p);
                    if (!one.empty()) picks.push_back(one);
                    if (q == std::string::npos) break;
                    p = q + 1;
                }
            }
            wpm_.SetOrbitPicks(std::move(picks));
        }
        RebuildOrbitOrder();
        LogOrbitSlots();
        std::string e;
        wpm_.Save(e);
        result = std::format(R"({{"ok":true,"orbit_picked":{},"orbit_pickable":{}}})",
                             wpm_.settings().orbitPicked ? "true" : "false",
                             int(orbitOrder_.size()));
    } else if (action == "powercap") {
        const int value = std::atoi(arg2.c_str());
        if (!power_.SetCap(arg, value)) {
            result = std::format(R"({{"ok":false,"error":"unknown power key '{}'"}})", arg);
        } else {
            wpm_.settings().power = power_.SaveCaps();
            std::string e;
            wpm_.Save(e);
            result = std::format(R"({{"ok":true,"key":"{}","value":{}}})", arg, value);
        }
    } else if (action == "fps") {
        const int value = std::atoi(arg.c_str());
        if (!arg2.empty()) { // per-monitor cap: arg2 is the monitor tag
            const MonitorInfo* m = nullptr;
            for (auto& mm : monitors_.list())
                if (mm.tag == arg2 || std::to_string(mm.index) == arg2) m = &mm;
            if (!m) {
                result = std::format(R"({{"ok":false,"error":"no monitor {}"}})", arg2);
            } else {
                wpm_.SetFpsFor(*m, value);
                std::string e;
                wpm_.Save(e);
                result = std::format(R"({{"ok":true,"monitor":"{}","fps":{}}})", m->tag, value);
            }
        } else {
            wpm_.SetGlobalMaxFps(value);
            std::string e;
            wpm_.Save(e);
            result = std::format(R"({{"ok":true,"max_fps":{}}})", wpm_.settings().globalMaxFps);
        }
    } else if (action == "rotate") {
        if (arg == "on" || arg == "off") {
            rotate_.on = arg == "on";
            if (rotate_.on) {
                if (rotate_.pool.empty())
                    for (auto& p : wpm_.catalog()) rotate_.pool.push_back(p.id());
                if (!arg2.empty()) rotate_.monitor = arg2;
                rotate_.nextAt = GetTickCount64() + ULONGLONG(rotate_.intervalMin) * 60000;
            }
            SaveRotation();
            result = std::format(R"({{"ok":true,"on":{},"interval_min":{},"pool":{},"monitor":"{}"}})",
                                 rotate_.on ? "true" : "false", rotate_.intervalMin, rotate_.pool.size(),
                                 rotate_.monitor);
        } else if (arg == "interval") {
            rotate_.intervalMin = std::max(1, std::atoi(arg2.c_str()));
            if (rotate_.on) rotate_.nextAt = GetTickCount64() + ULONGLONG(rotate_.intervalMin) * 60000;
            SaveRotation();
            result = std::format(R"({{"ok":true,"interval_min":{}}})", rotate_.intervalMin);
        } else if (arg == "pool") {
            rotate_.pool.clear();
            std::stringstream ss(arg2);
            std::string item;
            while (std::getline(ss, item, ','))
                if (!item.empty()) rotate_.pool.push_back(item);
            rotate_.index = 0;
            SaveRotation();
            result = std::format(R"({{"ok":true,"pool":{}}})", rotate_.pool.size());
        } else if (arg == "next" || arg == "prev") {
            // Manual stepping works whether or not the timer is on. A running timer gets its whole
            // interval back, otherwise a hand pick would be overwritten on the next tick.
            const std::string applied = RotateStep(arg == "next" ? 1 : -1);
            if (rotate_.pool.empty()) {
                result = R"({"ok":false,"error":"the rotation pool is empty"})";
            } else if (applied.empty()) {
                result = std::format(R"({{"ok":false,"error":"'{}' could not be shown on '{}'"}})",
                                     rotate_.pool[size_t(rotate_.index)], rotate_.monitor);
            } else {
                if (rotate_.on) rotate_.nextAt = GetTickCount64() + ULONGLONG(rotate_.intervalMin) * 60000;
                result = std::format(R"({{"ok":true,"wallpaper":"{}","index":{},"pool":{},"monitor":"{}"}})",
                                     applied, rotate_.index, rotate_.pool.size(), rotate_.monitor);
            }
        } else {
            result = std::format(
                R"({{"ok":true,"on":{},"interval_min":{},"pool":{},"monitor":"{}","next_in_s":{}}})",
                rotate_.on ? "true" : "false", rotate_.intervalMin, rotate_.pool.size(), rotate_.monitor,
                rotate_.on ? (rotate_.nextAt > GetTickCount64() ? (rotate_.nextAt - GetTickCount64()) / 1000 : 0) : 0);
        }
    } else if (action == "addimage") {
        std::string err;
        // `arg2` is the page the picture came from, empty for a file he picked himself. See
        // AddImagePackage: it is what makes the imported package say where it came from.
        const std::string id = AddImagePackage(wpm_.settings().root, ToWide(arg), dev_.wic.Get(), err, arg2);
        if (id.empty()) {
            result = std::format(R"({{"ok":false,"error":"{}"}})", err);
        } else {
            std::vector<std::string> skipped;
            wpm_.Scan(skipped);
            BuildThumbnails(true);
            result = std::format(R"({{"ok":true,"id":"{}"}})", id);
        }
    } else if (action == "addvideo") {
        std::string err;
        const std::string id = AddVideoPackage(wpm_.settings().root, ToWide(arg), err, arg2);
        if (id.empty()) {
            result = std::format(R"({{"ok":false,"error":"{}"}})", err);
        } else {
            std::vector<std::string> skipped;
            wpm_.Scan(skipped);
            BuildThumbnails(true);
            result = std::format(R"({{"ok":true,"id":"{}"}})", id);
        }
    } else if (action == "video") {
        // Per-clip transport, distinct from the global `pause`: that one stops the wallpaper drawing
        // (and idles the decoder with it), this one freezes the picture while the budget stays.
        //
        // `arg2` says *which* player, because the settings window pauses the picture he is looking at
        // and must not stop the wallpaper behind it: "preview", a monitor tag ("M0"), or empty for
        // every player - which is what the command line does when nobody says. `seek` keeps arg2 as
        // its number of seconds, so the one transport that needs a value is unaffected.
        const bool known = arg == "pause" || arg == "resume" || arg == "seek";
        const std::string target = (arg == "seek") ? std::string() : arg2;
        const auto hits = [&target](const std::string& who) { return target.empty() || target == who; };
        bool any = false, touched = false;
        Json list = Json::Array();
        for (auto& s : slots_) {
            VideoSource* v = s.instance.video();
            if (!v) continue;
            any = true;
            if (!hits(s.window.monitor().tag)) continue;
            touched = true;
            if (arg == "pause") v->Pause();
            else if (arg == "resume") v->Resume();
            else if (arg == "seek") v->Seek(std::atof(arg2.c_str()));
            Json j = Json::Object();
            j.set("where", Json::Of(s.window.monitor().tag));
            j.set("wallpaper", Json::Of(s.instance.id()));
            j.set("pos", Json::Of(v->position_s()));
            j.set("duration", Json::Of(v->duration_s()));
            j.set("paused", Json::Of(v->paused()));
            j.set("frames", Json::Of((long long)v->delivered()));
            list.push(std::move(j));
        }
        if (VideoSource* v = preview_.inst ? preview_.inst->video() : nullptr) {
            any = true;
            if (hits("preview")) {
                touched = true;
                if (arg == "seek") v->Seek(std::atof(arg2.c_str()));
                // The pause belongs to the pass, in both cases: the page stops receiving pictures, so
                // the window keeps painting the frame it stopped on. Stopping the *player* as well is
                // only possible when the preview owns it - a shared instance is the player his wallpaper
                // runs on, and pausing it there is the bug he reported ("有时候桌面的壁纸也被暂停了").
                else if (arg == "pause") { preview_.held = true; if (!preview_.shared) v->Pause(); }
                else if (arg == "resume") { preview_.held = false; if (!preview_.shared) v->Resume(); }
            }
            Json j = Json::Object();
            j.set("where", Json::Of(std::string("preview")));
            j.set("wallpaper", Json::Of(preview_.id));
            j.set("pos", Json::Of(v->position_s()));
            j.set("duration", Json::Of(v->duration_s()));
            j.set("paused", Json::Of(v->paused() || preview_.held));
            j.set("frames", Json::Of((long long)v->delivered()));
            list.push(std::move(j));
        }
        if (!known)
            result = std::format(R"({{"ok":false,"error":"video command is pause | resume | seek <seconds>, not '{}'"}})", arg);
        else if (!any)
            result = R"({"ok":false,"error":"no video wallpaper is loaded"})";
        else if (!touched)
            result = std::format(R"({{"ok":false,"error":"no video player is on '{}'"}})", target);
        else
            result = std::format(R"({{"ok":true,"players":{}}})", list.dump(0));
    } else if (action == "delimage") {
        result = DeleteImagePackage(arg);
    } else if (action == "preview") {
        std::string err;
        result = StartPreview(arg, err)
                     ? std::format(R"({{"ok":true,"preview":"on","id":"{}","size":"{}x{}","fps":{}}})",
                                   preview_.id, preview_.w, preview_.h, kPreviewFps)
                     : std::format(R"({{"ok":false,"error":"{}"}})", err);
    } else if (action == "previewoff") {
        StopPreview("ipc");
        result = R"({"ok":true,"preview":"off"})";
    } else if (action == "previewbeat") {
        preview_.lastBeatMs = GetTickCount64();
        result = std::format(R"({{"ok":true,"preview":{},"drew":{}}})",
                             preview_.on ? "true" : "false", preview_.drew);
    } else if (action == "autostart") {
        std::string err;
        if (arg == "get") {
            result = std::format(R"({{"ok":true,"autostart":{}}})", IsAutostartEnabled() ? "true" : "false");
        } else {
            const bool want = arg == "on" || arg == "true" || arg == "1";
            if (!SetAutostart(want, err)) result = std::format(R"({{"ok":false,"error":"{}"}})", err);
            else result = std::format(R"({{"ok":true,"autostart":{}}})", want ? "true" : "false");
        }
    } else if (action == "reload") {
        std::vector<std::string> skipped;
        wpm_.Scan(skipped);
        std::string e;
        BuildSlots(e);
        BuildThumbnails(true);
        result = std::format(R"({{"ok":true,"catalog":{},"slots":{}}})", wpm_.catalog().size(), slots_.size());
    } else if (action == "screenshottest") {
        result = R"({"ok":false,"error":"not implemented"})";
    } else {
        result = std::format(R"({{"ok":false,"error":"unknown command '{}'"}})", action);
    }
    UpdateSnapshot();
    return result;
}

void Application::OnMonitorsChanged() {
    Info(MOD, "display change detected - rebuilding slots");
    monitors_.Refresh(&dev_);
    std::string err;
    desktop_.Recheck();
    BuildSlots(err);
    UpdateSnapshot();
}

LRESULT CALLBACK Application::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Application* self = reinterpret_cast<Application*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) {
        if (m != WM_NCCREATE) return DefWindowProcW(h, m, w, l);
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<Application*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self->OnWindowMessage(h, m, w, l);
}

LRESULT Application::OnWindowMessage(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY:
            quit_ = true;
            return 0;
        case WM_DISPLAYCHANGE:
            OnMonitorsChanged();
            return 0;
        case WM_SETTINGCHANGE:
            if (l) {
                std::wstring area = reinterpret_cast<const wchar_t*>(l);
                if (area == L"DPIScaleFactor" || area == L"InteractiveSettings") OnMonitorsChanged();
            }
            return 0;
        case WM_TIMER:
            // Off is the shell's own look, so there is nothing to keep alive and nothing to poll.
            if (w == static_cast<WPARAM>(kTrayTimer) && wpm_.settings().trayAlpha > 0)
                trayFx_.Apply(wpm_.settings().trayAlpha);
            return 0;
        case WM_IPC_WAKE:
            ProcessQueued();
            return 0;
        case WM_APP_QUIT:
            Stop("window message quit");
            return 0;
        case WM_TRAYICON: {
            // NOTIFYICON_VERSION_4 packs the event into the high word of lParam.
            UINT ev = LOWORD(l);
            if (HIWORD(l) == 1) ev = LOWORD(l);
            if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU || ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK)
                tray_.ShowMenu(h);
            return 0;
        }
        case WM_COMMAND:
            if (tray_.HandleCommand(LOWORD(w))) return 0;
            break;
        default:
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

void Application::Stop(const std::string& why) {
    if (quit_) return;
    Info(MOD, "stopping: {}", why);
    quit_ = true;
    if (win_) PostMessageW(win_, WM_CLOSE, 0, 0);
}

void Application::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    // First thing, before anything else is torn down: this process borrowed the desktop's icons and
    // they go back on the way out. If it does not get to run this at all, the note in
    // HealIconLayer() puts them back on the next start, and the desktop's own context menu puts them
    // back without us.
    if (iconsHidden_) { std::string e; SetIconsHidden(false, e); }
    DestroySlots();
    if (ipc_) {
        ipc_->Stop();
        ipc_.reset();
    }
    tray_.Remove();
    power_.Shutdown();
    pacer_.Shutdown();
    UpdateSnapshot();
    {
        std::lock_guard lk(mtx_);
        Info(MOD, "final: {}", statusLine_);
    }
    // The preview keeps one compiled instance per id and an instance owns its decoder, so those have
    // to go before the device does: ~FFmpegPlayer joins the decode thread and frees the codec context.
    preview_.cache.clear();
    // The background thread may be holding an instance whose decoder it has not freed yet, and
    // ~FFmpegPlayer joins that decoder's thread, so it has to finish before the device goes away. The
    // queue drains first, so a config write posted just before quitting still lands.
    {
        std::lock_guard lk(bgMtx_);
        bgStop_ = true;
    }
    bgCv_.notify_all();
    if (background_.joinable()) background_.join();
    dev_.Shutdown();
    if (win_) {
        DestroyWindow(win_);
        win_ = nullptr;
    }
    log::Close();
}

Application::~Application() { Shutdown(); }

void Application::LoadRotation(const Json& j) {
    if (!j.isObject()) return;
    rotate_.on = j.boolOr("on");
    rotate_.intervalMin = std::max(1, j.intOr("interval_min", 60));
    rotate_.monitor = j.strOr("monitor", "all");
    rotate_.index = j.intOr("index", 0);
    rotate_.pool.clear();
    if (const Json* p = j.find("pool"); p && p->isArray())
        for (auto& e : p->items())
            if (e.isString()) rotate_.pool.push_back(e.asString());
    rotate_.nextAt = rotate_.on ? GetTickCount64() + ULONGLONG(rotate_.intervalMin) * 60000 : 0;
    Info(MOD, "rotation loaded: on={} every {} min over {} wallpaper(s) on {}", rotate_.on, rotate_.intervalMin,
         rotate_.pool.size(), rotate_.monitor);
}

void Application::SaveRotation() {
    Json r = Json::Object();
    r.set("on", Json::Of(rotate_.on));
    r.set("interval_min", Json::Of(rotate_.intervalMin));
    r.set("monitor", Json::Of(rotate_.monitor));
    r.set("index", Json::Of(rotate_.index));
    Json pool = Json::Array();
    for (auto& id : rotate_.pool) pool.push(Json::Of(id));
    r.set("pool", std::move(pool));
    wpm_.raw();
    std::string e;
    wpm_.SetRotate(r);
    wpm_.Save(e);
}

// Moves the rotation pointer by delta and shows what it lands on. The pointer starts from what is
// actually on screen rather than from the saved index: "设为壁纸" changes the screen without
// touching the rotation, and an index left behind by that would jump the next pick.
std::string Application::RotateStep(int delta) {
    if (rotate_.pool.empty())
        for (auto& p : wpm_.catalog()) rotate_.pool.push_back(p.id());
    if (rotate_.pool.empty()) return {};
    const int n = int(rotate_.pool.size());
    int from = rotate_.index;
    for (auto& s : slots_) {
        if (rotate_.monitor != "all" && s.window.monitor().tag != rotate_.monitor) continue;
        for (int i = 0; i < n; ++i)
            if (rotate_.pool[size_t(i)] == s.wallpaperId) { from = i; break; }
        break;
    }
    rotate_.index = ((from + delta) % n + n) % n;
    const std::string id = rotate_.pool[size_t(rotate_.index)];
    std::string applied;
    for (auto& s : slots_) {
        if (rotate_.monitor != "all" && s.window.monitor().tag != rotate_.monitor) continue;
        std::string e;
        if (ReloadSlot(s, id, e)) applied = id;
        else Warn(MOD, "rotation could not apply {}: {}", id, e);
    }
    if (!applied.empty()) {
        wpm_.SetAssignment(rotate_.monitor, applied);
        std::string e;
        wpm_.Save(e);
    }
    SaveRotation();
    UpdateSnapshot();
    return applied;
}

void Application::TickRotation(ULONGLONG nowMs) {
    if (!rotate_.on || rotate_.pool.empty()) return;
    if (!rotate_.nextAt || nowMs < rotate_.nextAt) return;
    rotate_.nextAt = nowMs + ULONGLONG(rotate_.intervalMin) * 60000;
    RotateStep(1);
}

void Application::DestroySlots() {
    for (auto& s : slots_) s.window.Destroy();
    slots_.clear();
}

} // namespace sw
