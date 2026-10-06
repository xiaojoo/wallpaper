#include "Wallpaper/Bridge.hpp"
#include "Engine/App/IPCClient.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Core/Platform.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTimer>

#include <QDesktopServices>

namespace sw {
namespace {

struct Entry {
    const char* key;
    const char* zh;
    const char* en;
};

// Chinese is the default; the header's language switch is the only way to English, and every
// string in the window comes from this table so nothing can be half-translated.
const Entry kStrings[] = {
    {"preview_motion", "动态预览", "Motion preview"},
    {"close", "关闭", "Close"},
    {"prev", "上一张", "Previous"},
    {"next", "下一张", "Next"},
    {"delete_image", "删除", "Delete"},
    {"confirm_delete", "确认删除", "Confirm delete"},
    {"deleted_image", "已删除", "Deleted"},
    {"delete_failed", "删除失败", "Delete failed"},
    {"delete_in_use", "这张正在用作壁纸，先换成别的", "This one is still showing on a screen - switch first"},
    {"no_local_images", "还没有导入过本地图片", "No local images imported yet"},
    {"clear_search", "清空搜索", "Clear search"},
    {"carousel_pause", "暂停轮播", "Pause carousel"},
    {"carousel_play", "继续轮播", "Resume carousel"},
    {"favorite_add", "收藏这张", "Add to favorites"},
    {"favorite_drop", "取消收藏", "Remove from favorites"},
    {"autostart", "开机自启", "Start at sign-in"},
    {"on", "开", "on"},
    {"off", "关", "off"},
    {"monitor_fps_cap", "本屏帧率上限（0=跟随壁纸）", "This screen's FPS cap (0 = follow wallpaper)"},
    {"home", "首页", "Home"},
    {"my_wallpapers", "我的壁纸", "In use"},
    {"favorites", "收藏夹", "Favorites"},
    {"local_images", "本地图片", "Local images"},
    {"auto_rotate", "自动更换", "Auto rotate"},
    {"categories", "壁纸分类", "Categories"},
    {"all", "全部", "All"},
    {"search_placeholder", "搜索壁纸、分类…", "Search wallpapers, categories…"},
    {"set_as_wallpaper", "设为壁纸", "Set as wallpaper"},
    {"apply_mode", "应用方式", "Apply mode"},
    {"mode_current", "设为当前桌面壁纸", "Set on the current desktop"},
    {"mode_all", "应用到所有显示器", "Apply to every monitor"},
    {"mode_random", "随机切换（自动更换）", "Rotate automatically"},
    {"interval", "切换间隔", "Interval"},
    {"enable_auto_rotate", "启用自动更换", "Enable auto rotate"},
    {"next_switch", "下次切换", "Next switch"},
    {"add_image", "添加本地图片", "Add a local image"},
    {"added_image", "已添加", "Added"},
    {"generated", "着色器生成", "generated"},
    {"settings", "设置", "Settings"},
    {"language", "语言", "Language"},
    {"wallpaper_count", "张壁纸", "wallpapers"},
    {"renderer_section", "渲染器", "Renderer"},
    {"performance", "性能与功耗", "Performance & power"},
    {"back", "返回", "Back"},
    {"app_title", "SmartWallpaper 壁纸设置", "SmartWallpaper settings"},
    {"subtitle", "渲染器通过命名管道控制，关掉本窗口不影响壁纸", "The renderer is driven over a named pipe; closing this window leaves the wallpaper running"},
    {"lang", "语言", "Language"},
    {"renderer", "渲染器", "Renderer"},
    {"running", "运行中", "running"},
    {"stopped", "未运行", "not running"},
    {"start", "启动渲染器", "Start renderer"},
    {"quit", "退出渲染器", "Quit renderer"},
    {"reload", "重载壁纸", "Reload wallpapers"},
    {"pause", "暂停渲染", "Pause rendering"},
    {"resume", "恢复渲染", "Resume rendering"},
    {"monitors", "显示器", "Monitors"},
    {"all_monitors", "全部显示器", "All monitors"},
    {"target", "操作目标", "Target"},
    {"wallpapers", "壁纸", "Wallpapers"},
    {"tab_general", "通用", "General"},
    {"tab_power", "省电", "Power saving"},
    {"current", "正在显示", "showing"},
    {"resolution", "分辨率", "Resolution"},
    {"refresh", "刷新率", "Refresh"},
    {"dpi", "缩放", "Scale"},
    {"fps_cap", "帧率上限", "FPS cap"},
    {"quality", "画质档", "Quality"},
    {"quality_package", "跟随壁纸", "Per wallpaper"},
    {"image_fit", "壁纸铺展", "Wallpaper fit"},
    {"image_fit_hint", "只影响图片壁纸（本地图片）；程序化壁纸按屏幕尺寸生成画面",
     "Applies to image wallpapers (local images); generated ones draw at the screen's own size"},
    {"fit_fill", "裁剪填充", "Fill (crop)"},
    {"fit_fit", "适应", "Fit"},
    {"fit_stretch", "拉伸", "Stretch"},
    {"fit_center", "居中", "Center"},
    {"fit_tile", "平铺", "Tile"},
    {"state", "状态", "State"},
    {"measured", "实测帧率", "Measured"},
    {"effective", "允许帧率", "Allowed"},
    {"avg_frame", "平均帧时", "Avg frame"},
    {"worst_frame", "最差帧时", "Worst frame"},
    {"frames", "累计帧数", "Frames"},
    {"draw_errors", "绘制错误", "Draw errors"},
    {"corner_changed", "画面变化采样", "Frame-change samples"},
    {"corner_static", "画面静止采样", "Frame-static samples"},
    {"error", "错误", "Error"},
    {"none", "无", "none"},
    {"host_layer", "桌面层级", "Desktop layer"},
    {"cpu", "进程 CPU", "Process CPU"},
    {"working_set", "工作集", "Working set"},
    {"uptime", "已运行", "Uptime"},
    {"status", "实时状态", "Live status"},
    {"power", "降功耗档位（帧率上限）", "Power caps (FPS limits)"},
    {"power_hint", "桌面被遮挡或全屏应用时按这些值降帧，0 表示完全停止渲染", "Limits applied when the desktop is covered or a fullscreen app runs; 0 stops rendering entirely"},
    {"cap_desktop_fps", "桌面可见", "Desktop visible"},
    {"cap_background_fps", "其他程序在前台", "Another app focused"},
    {"cap_covered_fps", "被窗口遮挡", "Covered by window"},
    {"cap_fullscreen_fps", "全屏应用", "Fullscreen app"},
    {"cap_locked_fps", "锁屏", "Session locked"},
    {"cap_display_off_fps", "屏幕关闭", "Display off"},
    {"cap_battery_fps", "电池供电", "On battery"},
    {"cap_battery_saver_fps", "省电模式", "Battery saver"},
    {"params", "参数", "Parameters"},
    {"particles", "GPU 粒子", "GPU particles"},
    {"textures", "贴图", "Textures"},
    {"base_fps", "壁纸请求帧率", "Requested by wallpaper"},
    {"log_folder", "打开日志目录", "Open log folder"},
    {"no_catalog", "渲染器里没有可用壁纸：检查 Wallpapers/ 目录", "The renderer has no wallpapers: check its Wallpapers/ folder"},
    {"need_renderer", "连不上渲染器管道，先启动它", "Cannot reach the renderer pipe - start it first"},
    {"applied", "已切换", "Switched"},
    {"set_ok", "已设置", "Set"},
    {"failed", "失败", "Failed"},
    {"unknown", "未知", "unknown"},
    {"state_desktop", "桌面可见", "desktop"},
    {"state_background", "前台是别的程序", "background"},
    {"state_covered", "被窗口遮挡", "covered"},
    {"state_fullscreen", "全屏应用", "fullscreen"},
    {"state_locked", "已锁屏", "locked"},
    {"state_display_off", "屏幕已关闭", "display off"},
    {"reason", "判定依据", "Why"},
    {"adapter", "显卡", "Adapter"},
    {"device", "设备", "Device"},
    {"wallpaper", "壁纸", "Wallpaper"},
    {"not_connected_hint", "渲染器没在跑。点“启动渲染器”，或自己运行 WallpaperRenderer.exe。", "The renderer is not running. Press Start renderer, or run WallpaperRenderer.exe yourself."},
    // The window is frameless, so its own title-bar buttons need words.
    {"win_min", "最小化", "Minimize"},
    {"win_max", "最大化", "Maximize"},
    {"win_restore", "还原", "Restore"},
};

QVariant toVariant(const Json& j) {
    switch (j.kind()) {
        case Json::Kind::Null: return {};
        case Json::Kind::Bool: return j.asBool();
        case Json::Kind::Number: return j.asNumber();
        case Json::Kind::String: return QString::fromStdString(j.asString());
        case Json::Kind::Array: {
            QVariantList l;
            for (auto& e : j.items()) l.push_back(toVariant(e));
            return l;
        }
        case Json::Kind::Object: {
            QVariantMap m;
            for (auto& kv : j.members()) m.insert(QString::fromStdString(kv.first), toVariant(kv.second));
            return m;
        }
    }
    return {};
}

} // namespace

// The settings window draws its own title bar. Three roles have to be pulled apart to get both
// flush edges and the platform's maximize animation:
//   Qt's geometry maths  - Qt::FramelessWindowHint, so Qt sizes its render surface as the whole
//                          window. Left to itself it also subtracts "the frame my styles ask for",
//                          which is what cost the previous attempt a 16x39 px unpainted strip.
//   Windows' styles      - WS_OVERLAPPEDWINDOW put back here, because that is what DWM looks at to
//                          decide whether to zoom-animate the maximize.
//   The visible frame    - chromeProc answers WM_NCCALCSIZE with "client = the whole window", so the
//                          caption those styles imply is never carved out of the client.
void Bridge::showCommand(int cmd) {
    if (chromeHwnd_) ShowWindow(reinterpret_cast<HWND>(chromeHwnd_), cmd);
}

namespace {
WNDPROC qtProc = nullptr;

LRESULT CALLBACK chromeProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCALCSIZE && w == TRUE) {
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
        if (IsZoomed(h)) {
            // A maximized window sits one border-width outside the monitor on the sides it touches;
            // taking that back is what keeps the content off the taskbar.
            const int bx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            const int by = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += bx;   p->rgrc[0].top += by;
            p->rgrc[0].right -= bx;  p->rgrc[0].bottom -= by;
        }
        return 0;   // client area = the whole window: nothing left for a frame to be drawn in
    }
    return qtProc ? CallWindowProcW(qtProc, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}
} // namespace

void Bridge::adoptChromeWindow(qint64 hwnd) {
    auto* h = reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd));
    if (!h || chromeHwnd_) return;          // one window, installed once
    chromeHwnd_ = reinterpret_cast<quintptr>(h);
    // Qt made this a WS_POPUP window; a popup has no caption for DWM to animate, so the overlapped
    // styles go back before the frame is recalculated.
    SetWindowLongW(h, GWL_STYLE, (GetWindowLongW(h, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    qtProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC,
                                                         reinterpret_cast<LONG_PTR>(&chromeProc)));
    if (!qtProc) {
        qWarning("chrome: no previous window procedure to chain to");
        chromeHwnd_ = 0;
        return;
    }
    // Qt had already laid the window out before the hook went in, so WM_NCCALCSIZE would not arrive
    // again on its own - and the styles above mean the next one would shrink the client on us.
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

Bridge::Bridge(QObject* parent) : QObject(parent) {
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    uiSettingsPath_ = s.fileName();
    lang_ = s.value("lang", "zh").toString();
    favorites_ = s.value("favorites").toStringList();
    target_ = s.value("target", "all").toString();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &Bridge::tick);
    timer_->start(1000);
    // 33 ms is the same cadence LivePreview polls at; previewBeat turns that into one heartbeat
    // every ~700 ms.
    previewTimer_ = new QTimer(this);
    connect(previewTimer_, &QTimer::timeout, this, &Bridge::previewBeat);
    refresh();
}

Bridge::~Bridge() = default;

QStringList Bridge::languages() const { return {"zh", "en"}; }

QStringList Bridge::qualityLevels() const {
    return {"package", "ultra", "high", "medium", "low", "battery"};
}

// The order is the number the shaders branch on (Contract.hlsl, uPerf.w); adding one here means
// adding a case in Image.hlsl and an Engine/Wallpaper/WallpaperManager.hpp enum member, in order.
QStringList Bridge::fitLevels() const { return {"fill", "fit", "stretch", "center", "tile"}; }

QString Bridge::t(const QString& key) const {
    const bool en = lang_ == "en";
    for (const auto& e : kStrings)
        if (key.compare(QLatin1String(e.key), Qt::CaseInsensitive) == 0)
            return QString::fromUtf8(en ? e.en : e.zh);
    return key; // a missing entry shows the key rather than silently rendering nothing
}

void Bridge::setLang(const QString& code) {
    if (code == lang_) return;
    lang_ = code;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("lang", lang_);
    emit langChanged();
    emit stateChanged(); // labels derived from t() refresh too
}

void Bridge::setTargetMonitor(const QString& tag) {
    if (tag == target_) return;
    target_ = tag;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("target", target_);
    emit targetChanged();
}

QString Bridge::pipeName() const { return QStringLiteral(R"(\\.\pipe\SmartWallpaper.Renderer)"); }

bool Bridge::request(const QString& body, int timeoutMs, Json* out) {
    std::string err;
    IpcReply r = IPCCall(ToWide(pipeName().toStdString()), body.toStdString(), DWORD(timeoutMs));
    if (!r.connected) {
        lastError_ = QString::fromStdString(r.error);
        return false;
    }
    if (out) {
        auto j = Json::Parse(r.response, err);
        if (!j) {
            lastError_ = QString::fromStdString(err);
            return false;
        }
        *out = std::move(*j);
    }
    return true;
}

void Bridge::tick() { refresh(); }

void Bridge::refresh() {
    Json status, list;
    // Each of these blocks the GUI thread for its whole timeout while no renderer is listening, and
    // 400 ms of every second was more than the strip fallback can afford: its motion clock is a
    // 42 ms Timer and it measured only ~2 ticks a second with the renderer gone. So the wait is
    // short and the poll backs off until the pipe answers.
    const int timeout = connected_ ? 400 : 150;
    const bool okStatus = request(R"({"cmd":"status"})", timeout, &status);
    if (okStatus) request(R"({"cmd":"list"})", timeout, &list);

    if (!okStatus) {
        if (++misses_ >= 2 && connected_) {
            connected_ = false;
            emit stateChanged();
        }
        if (!connected_ && timer_->interval() != 2000) timer_->start(2000);
        return;
    }
    if (!connected_ || timer_->interval() != 1000) timer_->start(1000);
    misses_ = 0;
    connected_ = true;
    ingest(status, list);
    emit stateChanged();
}

void Bridge::ingest(const Json& status, const Json& list) {
    uptime_ = status.numOr("uptime_s");
    host_ = QString::fromStdString(status.strOr("host"));
    root_ = QString::fromStdString(status.strOr("root"));
    paused_ = status.boolOr("paused");
    autostart_ = status.boolOr("autostart");
    if (const Json* r = status.find("rotate"); r && r->isObject()) {
        rotateOn_ = r->boolOr("on");
        rotateMin_ = r->intOr("interval_min", 60);
        rotateNext_ = r->intOr("next_in_s", 0);
    }
    maxFps_ = status.intOr("max_fps", 60);
    // The second pass can end without this window asking - a lost heartbeat, a renderer restart -
    // and showPreview only sends when the id changes, so nothing would bring it back until the user
    // switched wallpaper. The once-a-second snapshot already says whether it is running.
    if (!previewId_.isEmpty()) {
        if (const Json* p = status.find("preview"); p && p->isObject() && !p->boolOr("on")) {
            const QString id = previewId_;
            previewId_.clear();
            showPreview(id);
        }
    }
    qualityIsGlobal_ = status.boolOr("quality_is_global");
    quality_ = qualityIsGlobal_ ? QString::fromStdString(status.strOr("quality", "high"))
                                : QStringLiteral("package");
    imageFit_ = QString::fromStdString(status.strOr("image_fit", "fill"));
    if (const Json* p = status.find("process"); p && p->isObject()) {
        cpu_ = p->numOr("cpu_percent");
        ws_ = p->numOr("working_set_mb");
    }
    monitors_.clear();
    if (const Json* m = status.find("monitors"); m && m->isArray()) {
        for (auto& e : m->items()) {
            QVariantMap row;
            for (auto& kv : e.members())
                row.insert(QString::fromStdString(kv.first), toVariant(kv.second));
            std::string wid = e.strOr("wallpaper");
            row.insert("wallpaperName", QString::fromStdString(wid));
            if (const Json* c = list.find("catalog"); c && c->isArray()) {
                for (auto& w : c->items())
                    if (w.strOr("id") == wid) row.insert("wallpaperName", QString::fromStdString(w.strOr("name", wid)));
            }
            monitors_.push_back(row);
        }
        if (!target_.isEmpty() && target_ != "all") {
            bool stillThere = false;
            for (auto& v : monitors_)
                if (v.toMap().value("tag").toString() == target_) stillThere = true;
            if (!stillThere) setTargetMonitor("all");
        }
    }

    catalog_.clear();
    // The catalog comes from status, not list: only status carries the preview file paths.
    if (const Json* c = status.find("catalog"); c && c->isArray()) {
        for (auto& e : c->items()) {
            QVariantMap row;
            for (auto& kv : e.members()) {
                const QVariant v = toVariant(kv.second);
                if (kv.first == "thumb" || kv.first == "thumb_large")
                    row.insert(QString::fromStdString(kv.first),
                               QUrl::fromLocalFile(QDir::fromNativeSeparators(v.toString())).toString());
                else
                    row.insert(QString::fromStdString(kv.first), v);
            }
            if (const Json* fr = e.find("frames"); fr && fr->isArray()) {
                QStringList urls;
                for (auto& f : fr->items())
                    urls << QUrl::fromLocalFile(QDir::fromNativeSeparators(QString::fromStdString(f.asString()))).toString();
                row.insert("frameUrls", urls);
            }
            catalog_.push_back(row);
        }
    }

    caps_.clear();
    if (const Json* pc = status.find("power_caps"); pc && pc->isObject()) {
        for (auto& kv : pc->members()) {
            QVariantMap row;
            row.insert("key", QString::fromStdString(kv.first));
            row.insert("value", int(kv.second.asNumber()));
            row.insert("label", t(QString::fromStdString("cap_" + kv.first)));
            caps_.push_back(row);
        }
    }
    logPath_ = root_.isEmpty() ? QString() : QDir(root_ + "/logs").absolutePath();
}

void Bridge::say(const QString& msg, bool isError) {
    lastMessage_ = msg;
    lastIsError_ = isError;
    emit messageChanged();
}

bool Bridge::apply(const QString& wallpaperId) {
    Json out;
    std::string body = std::format(R"({{"cmd":"apply","arg":"{}","arg2":"{}"}})", wallpaperId.toStdString(),
                                   target_.toStdString());
    if (!request(body.c_str(), 1500, &out)) {
        say(t("failed") + ": " + lastError_, true);
        return false;
    }
    bool ok = out.boolOr("ok");
    say(ok ? t("applied") + ": " + wallpaperId + " -> " + target_
           : t("failed") + ": " + QString::fromStdString(out.strOr("error")),
        !ok);
    refresh();
    return ok;
}

bool Bridge::startRenderer() {
    const QString exe = QDir(root_.isEmpty() ? QCoreApplication::applicationDirPath()
                                             : root_)
                            .absoluteFilePath("WallpaperRenderer.exe");
    QString path = exe;
    if (!QFileInfo::exists(path))
        path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../RelWithDebInfo/WallpaperRenderer.exe");
    if (!QFileInfo::exists(path)) {
        say(t("failed") + ": WallpaperRenderer.exe", true);
        return false;
    }
    QProcess::startDetached(QFileInfo(path).absoluteFilePath(), {});
    say(t("start"), false);
    return true;
}

void Bridge::setPaused(bool p) {
    Json out;
    request(p ? R"({"cmd":"pause"})" : R"({"cmd":"resume"})", 1500, &out);
    paused_ = p;
    emit stateChanged();
}

void Bridge::setQuality(const QString& level) {
    Json out;
    std::string body = std::format(R"({{"cmd":"quality","arg":"{}"}})", level.toStdString());
    request(body.c_str(), 1500, &out);
    refresh();
}

void Bridge::setFit(const QString& mode) {
    Json out;
    std::string body = std::format(R"({{"cmd":"fit","arg":"{}"}})", mode.toStdString());
    request(body.c_str(), 1500, &out);
    // The pipe answers "queued" for anything that is not status/list/quit, so there is nothing to
    // read out of that reply - the snapshot is what carries the mode now in force.
    refresh();
}

void Bridge::setMaxFps(int fps) {
    Json out;
    std::string body = std::format(R"({{"cmd":"fps","arg":"{}"}})", fps);
    request(body.c_str(), 1500, &out);
    maxFps_ = fps;
    emit stateChanged();
}

void Bridge::setPowerCap(const QString& key, int value) {
    Json out;
    std::string body = std::format(R"({{"cmd":"powercap","arg":"{}","arg2":"{}"}})", key.toStdString(), value);
    if (!request(body.c_str(), 1500, &out) || !out.boolOr("ok")) {
        say(t("failed") + ": " + key, true);
        return;
    }
    say(t("set_ok") + ": " + t("cap_" + key) + " = " + QString::number(value), false);
    refresh();
}

void Bridge::setMonitorFps(const QString& tag, int fps) {
    Json out;
    std::string body = std::format(R"({{"cmd":"fps","arg":"{}","arg2":"{}"}})", fps, tag.toStdString());
    if (!request(body.c_str(), 1500, &out)) { say(t("failed"), true); return; }
    say(t("set_ok") + ": " + tag + " fps = " + QString::number(fps), false);
    refresh();
}

void Bridge::setAutostart(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"autostart","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    autostart_ = on;
    emit stateChanged();
}

QString Bridge::arg(const QString& name) const {
    const auto a = QCoreApplication::arguments();
    for (int i = 0; i < a.size(); ++i)
        if (a[i] == name && i + 1 < a.size()) return a[i + 1];
    return {};
}

void Bridge::setRotate(bool on) {
    Json out;
    request(std::format(R"({{"cmd":"rotate","arg":"{}"}})", on ? "on" : "off").c_str(), 1500, &out);
    rotateOn_ = on;
    emit stateChanged();
}

void Bridge::setRotateInterval(int minutes) {
    Json out;
    request(std::format(R"({{"cmd":"rotate","arg":"interval","arg2":"{}"}})", minutes).c_str(), 1500, &out);
    rotateMin_ = minutes;
    emit stateChanged();
}

void Bridge::setRotateScope(const QString& monitor) {
    Json out;
    request(std::format(R"({{"cmd":"rotate","arg":"on","arg2":"{}"}})", monitor.toStdString()).c_str(), 1500, &out);
    refresh();
}

void Bridge::addImageFile() {
    const QString file = QFileDialog::getOpenFileName(
        nullptr, t("add_image"), QDir::homePath(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp *.tiff)");
    if (file.isEmpty()) return;
    Json out;
    std::string body = std::format(R"({{"cmd":"addimage","arg":"{}"}})", file.toStdString());
    if (!request(body.c_str(), 8000, &out) || !out.boolOr("ok")) {
        say(t("failed") + ": " + QFileInfo(file).fileName(), true);
        return;
    }
    say(t("added_image") + ": " + QFileInfo(file).fileName(), false);
    refresh();
}

void Bridge::deleteImage(const QString& id) {
    Json out;
    std::string body = std::format(R"({{"cmd":"delimage","arg":"{}"}})", id.toStdString());
    if (!request(body.c_str(), 4000, &out) || !out.boolOr("ok")) {
        // The renderer names the screen that still holds the copy, so the message can say where.
        const std::string code = out.strOr("code");
        if (code == "in_use")
            say(t("delete_in_use") + QStringLiteral(" (") + QString::fromStdString(out.strOr("monitor"))
                    + QStringLiteral(")"), true);
        else
            say(t("delete_failed") + QStringLiteral(": ")
                    + QString::fromStdString(code.empty() ? out.strOr("error") : code), true);
        return;
    }
    refresh();
    say(t("deleted_image") + QStringLiteral(": ") + id, false);
}

void Bridge::toggleFavorite(const QString& id) {    const bool wasFavorite = favorites_.removeAll(id) > 0;
    if (!wasFavorite) favorites_ << id;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("favorites", favorites_);
    emit favoritesChanged();
}

bool Bridge::isFavorite(const QString& id) const { return favorites_.contains(id); }

void Bridge::reload() {
    Json out;
    request(R"({"cmd":"reload"})", 2000, &out);
    refresh();
}

void Bridge::showPreview(const QString& id) {
    if (id.isEmpty()) return;
    if (previewId_ != id) {
        previewId_ = id;
        previewBeatMs_ = 0;   // a new wallpaper asks for a beat straight away
        std::string body = std::format(R"({{"cmd":"preview","arg":"{}"}})", id.toStdString());
        Json out;
        request(body.c_str(), 2000, &out);
    }
    if (!previewTimer_->isActive()) previewTimer_->start(33);
}

void Bridge::hidePreview() {
    if (!previewTimer_->isActive()) return;
    previewTimer_->stop();
    request(R"({"cmd":"previewoff"})", 1000, nullptr);
    previewId_.clear();
}

void Bridge::previewBeat() {
    // The renderer stops its second pass if this window goes quiet, so a crashed settings window
    // cannot leave the desktop renderer burning GPU. LivePreview reads the pixels itself; Bridge
    // only owns the pipe.
    //
    // Wall clock, not a tick count: `root.current` is a JS object that the once-a-second status
    // refresh rebinds, so every refresh called syncPreview() again, and a counter reset there never
    // reached its threshold - no heartbeat was ever sent, the pass died 3 s after each switch, and
    // the page silently fell back to the saved strip (which restarts from frame zero: the "重头开始"
    // flicker).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (previewBeatMs_ != 0 && now - previewBeatMs_ < 700) return;
    previewBeatMs_ = now;
    request(R"({"cmd":"previewbeat"})", 500, nullptr);
}

void Bridge::quitRenderer() {
    Json out;
    request(R"({"cmd":"quit"})", 1500, &out);
    connected_ = false;
    emit stateChanged();
}

void Bridge::openLogFolder() {
    if (logPath_.isEmpty()) {
        say(t("need_renderer"), true);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(logPath_));
}

} // namespace sw
