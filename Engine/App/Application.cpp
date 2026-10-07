#include "Engine/App/Application.hpp"
#include "Engine/App/IPCServer.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Graphics/VideoPlayer.hpp"

#include <psapi.h>
#include <shellapi.h>
#include <filesystem>
#include <algorithm>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
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
        if (lastWallNs && wallNow > lastWallNs && cpu > lastCpuNs)
            m.cpuPercent = double(cpu - lastCpuNs) / double(wallNow - lastWallNs) * 100.0;
        lastWallNs = wallNow;
        lastCpuNs = cpu;
    }
    return m;
}

} // namespace

bool Application::Start(const CommandLine& cl, std::string& error) {
    cl_ = cl;
    std::wstring logDir = cl_.logDir.empty() ? LogDirFor(cl_.exeDir) : cl_.logDir;
    CreateDirectoryW(logDir.c_str(), nullptr);
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
    // After CoInitializeEx inside dev_.Init, and before any wallpaper asks for a decoder.
    if (FAILED(MFStartup(MF_VERSION))) {
        error = "Media Foundation startup failed (video wallpapers cannot decode)";
        Error(MOD, "{}", error);
        return false;
    }
    if (!renderer_.Init(dev_.dev.Get(), error)) {
        Error(MOD, "renderer: {}", error);
        return false;
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
bool Application::IsAutostartEnabled(std::string& error) {
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
std::string AddImagePackage(const std::wstring& root, const std::wstring& srcFile, IWICImagingFactory* wic,
                            std::string& error) {
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
    j.set("category", Json::Of(std::string("本地图片")));
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
// guard only accepts that form, and the settings window's import list is built from it.
std::string AddVideoPackage(const std::wstring& root, const std::wstring& srcFile, std::string& error) {
    namespace fs = std::filesystem;
    if (!fs::exists(srcFile)) { error = "file does not exist"; return {}; }

    UINT w = 0, h = 0;
    double rate = 0.0, duration = 0.0;
    bool bt601 = false;
    {
        Com<IMFSourceReader> rd;
        const HRESULT prh = MFCreateSourceReaderFromURL(srcFile.c_str(), nullptr, &rd);
        if (FAILED(prh)) {
            error = "Media Foundation cannot open this file: " + HResultToString(prh);
            return {};
        }
        Com<IMFMediaType> mt;
        const HRESULT mhr = rd->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &mt);
        if (FAILED(mhr)) {
            error = "no video stream in this file: " + HResultToString(mhr);
            return {};
        }
        UINT32 ww = 0, hh = 0;
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &ww, &hh);
        w = UINT(ww);
        h = UINT(hh);
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den)) && den)
            rate = double(num) / double(den);
        // Which matrix the encoder used. This SDK has no enum name for the primaries values, so the
        // numbers are the ones MF defines: 1 BT.709, 2 unspecified, 4..7 the BT.601 family
        // (625-line, 525-line, SMPTE-240M, SMPTE-C). Only a declared 601 family switches the shader.
        UINT32 prim = 0;
        if (SUCCEEDED(mt->GetUINT32(MF_MT_VIDEO_PRIMARIES, &prim)))
            bt601 = (prim >= 4 && prim <= 7);
        PROPVARIANT var{};
        PropVariantInit(&var);
        if (SUCCEEDED(rd->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8)
            duration = double(var.ulVal) / 1e7;
        PropVariantClear(&var);
    }
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
    j.set("category", Json::Of(std::string("本地视频")));
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
    Info(MOD, "added local video package {} ({}x{}, {:.2f} fps, {:.1f} s)", id, w, h, rate, duration);
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

    // The preview holds one compiled instance per id and that instance owns the loaded picture.
    // An import takes the lowest free local_NN, so the next upload arrives under this same id and
    // would be drawn with the deleted file's texture - the card thumbnails come off disk and look
    // right while the big picture stays on the old one. Stopping the pass is what lets the next
    // `preview` get past the "already on" shortcut and prepare the new package.
    if (preview_.id == id) {
        StopPreview("package deleted");
        preview_.inst = nullptr;
    }
    preview_.cache.erase(id);

    std::vector<std::string> skipped;
    wpm_.Scan(skipped);
    Info(MOD, "deleted local image package {}", id);
    return std::format(R"({{"ok":true,"deleted":"{}","catalog":{}}})", id, wpm_.catalog().size());
}

bool Application::BuildSlots(std::string& error) {
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

bool Application::ReloadSlot(Slot& s, const std::string& wallpaperId, std::string& error) {
    const WallpaperPackage* pkg = wpm_.find(wallpaperId);
    if (!pkg) {
        error = "wallpaper '" + wallpaperId + "' is not installed";
        return false;
    }
    WallpaperInstance fresh;
    if (!fresh.Prepare(wpm_.settings().root, *pkg, dev_, error)) return false;
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

    DrawArgs args = s.instance.MakeArgs(f, dev_.ctx.Get());
    if (!renderer_.Render(dev_.ctx.Get(), surf.rtv(), surf.width(), surf.height(), args, err)) {
        s.error = err;
        surf.End(dev_.ctx.Get());
        Warn(MOD, "{}: draw failed: {}", m.tag, err);
        return;
    }
    // One corner sample a second is enough to tell "animating" from "frozen", and it is the
    // only proof available when another window covers the desktop.
    if (nowSeconds - s.lastSampleT >= 1.0) {
        s.lastSampleT = nowSeconds;
        unsigned long long sum = 0;
        if (surf.SampleCorner(dev_.ctx.Get(), sum)) {
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
    r.set("root", Json::Of(ToUtf8(wpm_.settings().root)));
    r.set("paused", Json::Of(paused_.load()));
    { std::string ae; r.set("autostart", Json::Of(IsAutostartEnabled(ae))); }
    r.set("max_fps", Json::Of(wpm_.settings().globalMaxFps));
    // The settings window reads these two back to label the 画质档 field. Without them it always
    // falls back to "package", so picking an option changed the renderer but never the UI.
    r.set("quality", Json::Of(std::string(QualityName(wpm_.settings().globalQuality))));
    r.set("quality_is_global", Json::Of(wpm_.settings().useGlobalQuality));
    r.set("image_fit", Json::Of(std::string(ImageFitName(wpm_.settings().imageFit))));
    r.set("tray_alpha", Json::Of(wpm_.settings().trayAlpha));
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
            m.set("resolution", p.srcWidth() > 0 && p.srcHeight() > 0
                                     ? Json::Of(std::format("{} x {}", p.srcWidth(), p.srcHeight()))
                                     : Json::Of(std::string("generated")));
            m.set("params", Json::Of((long long)p.params().members().size()));
            m.set("textures", Json::Of((long long)p.textures().size()));
            m.set("particles", Json::Of((long long)p.particleCount()));
            // What kind of package this is, so the window can label a clip without opening it.
            m.set("type", Json::Of(std::string(p.isVideo() ? "video"
                                              : p.hasParticles() ? "particles" : "shader")));
            if (p.isVideo()) {
                m.set("duration_s", Json::Of(p.srcDuration()));
                m.set("loop", Json::Of(p.videoLoop()));
            }
            auto it = thumbs_.find(p.id());
            if (it != thumbs_.end() && it->second.ok) {
                m.set("thumb", Json::Of(ToUtf8(it->second.still)));
                m.set("thumb_large", Json::Of(ToUtf8(it->second.large)));
                // The picture on disk is replaced whenever the renderer rebuilds it, but a window that
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
    r.set("process", std::move(memj));

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
        if (VideoPlayer* v = s.instance.video()) {
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
        // Previews are judged against the desktop, so they are told what the desktop is.
        if (const MonitorInfo* m = monitors_.byIndex(0)) th.setDesignSize(m->px.w, m->px.h);
        // Reuse the compiled instance when this wallpaper is what a monitor is already showing;
        // otherwise compile a throwaway one, which is what a fresh install has to do anyway.
        // A video wallpaper must not be the live one: grabbing its card parks that player on the
        // frame it was asked to show (ShowFrameAt arms holding_), and nothing clears that flag except
        // a later seek - so reusing the live instance froze the desktop on a still picture at every
        // start. Measured 2026-10-07: delivered stuck at 84, the thread parked in cv_.wait at 0% CPU,
        // and one `--ctl video seek 0.5` brought it back.
        WallpaperInstance* use = nullptr;
        for (auto& s : slots_)
            if (s.wallpaperId == p.id() && !s.instance.hasVideo()) use = &s.instance;
        WallpaperInstance temp;
        if (use) {
            if (!th.Build(dev_, renderer_, *use, thumbDir_, p.id(), thumbs_[p.id()], err))
                Warn(MOD, "thumbnail for {} failed: {}", p.id(), err);
        } else if (temp.Prepare(wpm_.settings().root, p, dev_, err)) {
            if (!th.Build(dev_, renderer_, temp, thumbDir_, p.id(), thumbs_[p.id()], err))
                Warn(MOD, "thumbnail for {} failed: {}", p.id(), err);
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
constexpr UINT kPreviewW = 1280, kPreviewH = 720;
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
    if (p.inst) {
        p.inst->SetMediaActive(false);
        if (p.inst->hasVideo()) {
            for (auto it = p.cache.begin(); it != p.cache.end(); ++it)
                if (it->second.get() == p.inst) {
                    p.cache.erase(it);
                    break;
                }
            p.inst = nullptr;
        }
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
    }

    if (!p.inst || p.inst->id() != id) {
        auto it = p.cache.find(id);
        if (it == p.cache.end()) {
            // Bounded: a library of imported pictures must not grow the cache without limit. The
            // instance in use is dropped with the rest, and re-prepared on the next look.
            if (p.cache.size() >= 12) {
                p.cache.clear();
                p.inst = nullptr;
            }
            auto fresh = std::make_unique<WallpaperInstance>();
            if (!fresh->Prepare(wpm_.settings().root, *pkg, dev_, error)) {
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
    p.lastBeatMs = GetTickCount64();
    p.inst->SetMediaActive(true);
    pacer_.ConfigureSlot(p.clock, kPreviewFps, 0);
    Info(MOD, "preview on: {} ({}x{} at {} fps)", id, kPreviewW, kPreviewH, kPreviewFps);
    return true;
}

void Application::StopPreview(const char* why) {
    if (!preview_.on) return;
    preview_.on = false;
    if (preview_.inst) preview_.inst->SetMediaActive(false);
    pacer_.ConfigureSlot(preview_.clock, 0, 0);
    // The textures and the compiled instance stay: the browse page comes back to the same wallpaper
    // seconds later, and re-preparing a shader mid-interaction is a visible hitch for 6 MB of VRAM.
    // Video instances are the exception - each one parked holds a decoder thread, its frame buffers
    // and two plane textures (measured: a 4K clip is tens of MB and several MF worker threads), and
    // browsing through a dozen of them is what took the process to 2.5 GB. Re-opening one costs the
    // next look at that clip, not the page.
    for (auto it = preview_.cache.begin(); it != preview_.cache.end();) {
        if (it->second->hasVideo()) it = preview_.cache.erase(it);
        else ++it;
    }
    preview_.inst = nullptr;
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
        if (SUCCEEDED(dev_.ctx->Map(p.stg[i].Get(), 0, D3D11_MAP_READ, 0, &mp))) {
            p.writer.Write(p.w, p.h, static_cast<const BYTE*>(mp.pData), mp.RowPitch, GetTickCount64(),
                           p.idHash);
            dev_.ctx->Unmap(p.stg[i].Get(), 0);
        }
        p.slotState[i] = Preview::Free;
        freeSlot = i;
    }

    FrameCB f{};
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

    DrawArgs args = p.inst->MakeArgs(f, dev_.ctx.Get());
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
    p.slotState[i] = Preview::CopyPending;
    ++p.drew;
    p.error.clear();
}

void Application::ProcessQueued() {
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

int Application::Run() {
    ULONGLONG startedAt = GetTickCount64();
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    std::vector<FPSController::Slot*> clocks;
    UINT logCounter = 0;

    while (!quit_) {
        DrainMessages();
        if (quit_) break;
        ProcessQueued();

        ULONGLONG nowMs = GetTickCount64();
        power_.Poll(nowMs);
        TickRotation(nowMs);
        if (preview_.on && nowMs - preview_.lastBeatMs > kPreviewBeatTimeoutMs)
            StopPreview("settings window stopped beating");

        if (nowMs - lastHostCheckMs_ > 5000) {
            lastHostCheckMs_ = nowMs;
            POINT cp{};
            GetCursorPos(&cp);
            cursorPx_ = cp;
            if (!cl_.noDesktop) {
                desktop_.Recheck();
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

        LARGE_INTEGER li{};
        QueryPerformanceCounter(&li);
        double t = double(li.QuadPart - qpcStart_.QuadPart) / double(freq.QuadPart);

        bool drewAny = false;
        ++sawDrawStage_;
        for (auto& s : slots_) {
            if (!pacer_.SlotDue(s.clock)) { pacer_.SlotIdle(s.clock); continue; }
            DrawSlot(s, t);
            pacer_.SlotRendered(s.clock);
            ++drewTotal_;
            drewAny = true;
        }
        if (preview_.on) {
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
        std::string err;
        wpm_.Save(err);
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
        const std::string id = AddImagePackage(wpm_.settings().root, ToWide(arg), dev_.wic.Get(), err);
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
        const std::string id = AddVideoPackage(wpm_.settings().root, ToWide(arg), err);
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
        const bool known = arg == "pause" || arg == "resume" || arg == "seek";
        bool any = false;
        Json list = Json::Array();
        for (auto& s : slots_) {
            VideoPlayer* v = s.instance.video();
            if (!v) continue;
            any = true;
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
        if (VideoPlayer* v = preview_.inst ? preview_.inst->video() : nullptr) {
            any = true;
            if (arg == "pause") v->Pause();
            else if (arg == "resume") v->Resume();
            else if (arg == "seek") v->Seek(std::atof(arg2.c_str()));
            Json j = Json::Object();
            j.set("where", Json::Of(std::string("preview")));
            j.set("wallpaper", Json::Of(preview_.id));
            j.set("pos", Json::Of(v->position_s()));
            j.set("duration", Json::Of(v->duration_s()));
            j.set("paused", Json::Of(v->paused()));
            j.set("frames", Json::Of((long long)v->delivered()));
            list.push(std::move(j));
        }
        if (!known)
            result = std::format(R"({{"ok":false,"error":"video command is pause | resume | seek <seconds>, not '{}'"}})", arg);
        else if (!any)
            result = R"({"ok":false,"error":"no video wallpaper is loaded"})";
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
            result = std::format(R"({{"ok":true,"autostart":{}}})", IsAutostartEnabled(err) ? "true" : "false");
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
    // The preview keeps one compiled instance per id and an instance owns its decoder, so those have to
    // go while Media Foundation is still up; ~VideoPlayer joins the decode thread.
    preview_.cache.clear();
    MFShutdown();
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
