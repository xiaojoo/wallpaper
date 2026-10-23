#include "Wallpaper/Bridge.hpp"
#include "Engine/App/IPCClient.hpp"
#include "Engine/Core/Log.hpp"
#include "Engine/Core/Platform.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>

#include <QDesktopServices>

namespace sw {
namespace {

// What the bytes actually are, not what the site claimed. 4kwallpapers answers a JPEG with
// `Content-Type: binary/data` plus `X-Content-Type-Options: nosniff` (measured 2026-10-12: the file
// begins ff d8 ff e0 "JFIF", 444130 bytes), so on that site the header is a download policy and the
// signature is the only fact available before the engine opens the file.
QString sniffMedia(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray h = f.read(16);
    f.close();
    if (h.startsWith("\xFF\xD8\xFF")) return QStringLiteral("image/jpeg");
    if (h.startsWith("\x89PNG\x0D\x0A\x1A\x0A")) return QStringLiteral("image/png");
    if (h.startsWith("GIF87a") || h.startsWith("GIF89a")) return QStringLiteral("image/gif");
    if (h.startsWith("RIFF") && h.mid(8, 4) == "WEBP") return QStringLiteral("image/webp");
    if (h.size() >= 12 && h.mid(4, 4) == "ftyp") return QStringLiteral("video/mp4");
    if (h.startsWith("\x1A\x45\xDF\xA3")) return QStringLiteral("video/webm");
    return QStringLiteral("unknown");
}

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
    {"all", "全部壁纸", "All wallpapers"},
    {"search_placeholder", "搜索壁纸、分类", "Search wallpapers, categories"},
    {"set_as_wallpaper", "设为壁纸", "Set as wallpaper"},
    {"apply_mode", "应用方式", "Apply mode"},
    {"mode_current", "设为壁纸", "Set as wallpaper"},
    {"mode_all", "所有显示器", "All monitors"},
    {"mode_random", "随机切换", "Random"},
    {"interval", "切换间隔", "Interval"},
    {"enable_auto_rotate", "启用自动更换", "Enable auto rotate"},
    {"next_switch", "下次切换", "Next switch"},
    {"add_image", "添加本地壁纸", "Add a local wallpaper"},
    {"add_folder", "选择文件夹", "Choose a folder"},
    {"folder_queued", "已排入导入队列", "queued for import"},
    {"folder_empty", "这个文件夹里没有可用的图片或视频", "no pictures or videos in that folder"},
    {"added_image", "已添加", "Added"},
    {"generated", "着色器生成", "generated"},
    {"settings", "设置", "Settings"},
    {"language", "语言", "Language"},
    {"wallpaper_count", "张壁纸", "wallpapers"},
    {"renderer_section", "渲染器", "Renderer"},
    {"performance", "性能与功耗", "Performance & power"},
    {"back", "返回", "Back"},
    {"app_title", "Wallpaper 壁纸设置", "Wallpaper settings"},
    {"subtitle", "渲染器通过命名管道控制，关掉本窗口不影响壁纸", "The renderer is driven over a named pipe; closing this window leaves the wallpaper running"},
    {"lang", "语言", "Language"},
    {"renderer", "渲染器", "Renderer"},
    {"running", "运行中", "running"},
    {"stopped", "未运行", "not running"},
    {"start", "启动渲染器", "Start renderer"},
    {"quit", "退出渲染器", "Quit renderer"},
    // One key each way, because the button is the same button: it says what clicking it will do.
    // Off takes the wallpaper off the desktop and keeps the process, so the preview still moves.
    {"wallpaper_off", "退出壁纸", "Wallpaper off"},
    {"wallpaper_on", "恢复壁纸", "Wallpaper on"},
    {"reload", "重载壁纸", "Reload wallpapers"},
    {"pause", "暂停渲染", "Pause rendering"},
    {"resume", "恢复渲染", "Resume rendering"},
    // The clip's own transport on the big picture - a different thing from the two above, which stop
    // the wallpaper drawing itself, so the wording has to name the video.
    {"video_pause", "暂停视频", "Pause clip"},
    {"video_resume", "继续播放视频", "Resume clip"},
    {"monitors", "显示器", "Monitors"},
    {"all_monitors", "全部显示器", "All monitors"},
    // The licence notice is a distribution obligation for the FFmpeg DLLs, so it has to be readable
    // from the running program rather than only from the source tree.
    // The percent in the status line is an average, so the label carries the window it averages over.
    {"cpu_over", "均值窗口", "over"},
    // The LGPL sentence that used to live here was taken out of the 通用 page on 2026-10-08. The
    // obligation itself is untouched: LICENSE-FFmpeg.txt and NOTICE-FFmpeg.txt still ship beside the
    // exe (CMake stages them), and `--ctl status` still answers which build is loaded.
    {"target", "操作目标", "Target"},
    {"wallpapers", "壁纸", "Wallpapers"},
    {"tab_general", "通用", "General"},
    {"tab_power", "省电", "Power saving"},
    {"tab_tools", "工具", "Tools"},
    {"tab_shortcuts", "快捷键", "Shortcuts"},
    {"current", "正在显示", "showing"},
    {"resolution", "分辨率", "Resolution"},
    {"refresh", "刷新率", "Refresh"},
    {"dpi", "缩放", "Scale"},
    {"fps_cap", "帧率上限", "FPS cap"},
    {"quality", "画质档", "Quality"},
    {"quality_package", "跟随壁纸", "Per wallpaper"},
    {"image_fit", "壁纸铺展", "Wallpaper fit"},
    {"transition", "切换动画", "Switch animation"},
    {"transition_none", "无", "None"},
    {"transition_dissolve", "溶解", "Dissolve"},
    {"transition_liquid", "液态", "Liquid"},
    {"fit_fill", "裁剪填充", "Fill (crop)"},
    {"fit_fit", "适应", "Fit"},
    {"fit_stretch", "拉伸", "Stretch"},
    {"fit_center", "居中", "Center"},
    {"fit_tile", "平铺", "Tile"},
    {"hud_card", "资源状态卡", "Status card"},
    {"hud_desc", "把这台机器的 CPU / 内存 / 磁盘 / GPU 实时读数画在壁纸上，任何壁纸都适用。",
     "Draws this machine's live CPU / memory / disk / GPU readout over the wallpaper, whatever it is."},
    {"hud_corner", "卡片位置", "Card position"},
    {"hud_corner_top_right", "右上", "Top right"},
    {"hud_corner_top_left", "左上", "Top left"},
    {"hud_corner_bottom_right", "右下", "Bottom right"},
    {"hud_corner_bottom_left", "左下", "Bottom left"},
    {"hud_size", "卡片大小", "Card size"},
    {"hud_frame", "边框样式", "Border style"},
    {"hud_frame_notch", "凹口重边", "Notch and heavy edge"},
    {"hud_frame_basetab", "底托上条", "Base tab"},
    {"hud_frame_ticks", "两端刻度", "Ticks at both ends"},
    {"hud_frame_topbar", "顶栏厚边", "Top bar"},
    {"hud_frame_band", "右上厚带", "Upper-right band"},
    {"hud_frame_split", "双层标题栏", "Split header"},
    {"hud_drag", "允许拖动", "Allow dragging"},
    {"hud_drag_desc", "打开后卡片外面会出现一个虚线框，按住它拖到想要的位置，松手即生效并自动关闭。",
     "Turn this on and a dashed frame appears over the card: drag it wherever you want, release to "
     "drop the card there and the frame goes away by itself."},
    {"hud_corner_custom", "自定义（已拖动）", "Custom (dragged)"},
    {"hud_corner_reset", "回到角落", "Back to corner"},
    {"chart_tab", "图表", "Charts"},
    {"chart_panel", "图表面板", "Chart panel"},
    {"chart_desc", "把三个温度画成图形：显卡、硬盘、主板热区，独立于资源状态卡，位置和大小各调各的。"
                   "CPU 和内存的温度这台机答不上（要第三方内核驱动），所以不画；主板热区是机箱传感器，"
                   "不冒充 CPU。硬盘温度只有 NVMe 答得上，SATA 或 USB 盘上那一格空着。",
     "Draws three temperatures as a graphic - GPU, drive and the motherboard's ACPI thermal zone - "
     "separate from the status card, with its own position and size. This machine answers neither a "
     "CPU-die nor a DIMM temperature without a third-party kernel driver, so those are not drawn; the "
     "thermal zone is a chassis sensor and is not labelled CPU. Only NVMe drives report a temperature."},
    {"chart_type", "图表类型", "Chart type"},
    {"chart_type_bar", "柱状", "Bars"},
    {"chart_type_donut", "环形", "Rings"},
    {"chart_type_line", "折线", "Line"},
    {"chart_corner", "面板位置", "Panel position"},
    {"chart_size", "面板大小", "Panel size"},
    {"chart_frame", "边框样式", "Border style"},
    {"chart_drag", "允许拖动", "Allow dragging"},
    {"chart_drag_desc", "打开后面板外面会出现一个虚线框，按住它拖到想要的位置，松手即生效并自动关闭。",
     "Turn this on and a dashed frame appears over the panel: drag it wherever you want, release to "
     "drop it there and the frame goes away by itself."},
    {"chart_corner_custom", "自定义（已拖动）", "Custom (dragged)"},
    {"chart_corner_reset", "回到角落", "Back to corner"},
    {"chart_unavailable", "图表面板不可用", "Chart panel unavailable"},
    {"hud_unavailable", "状态卡不可用", "Status card unavailable"},
    {"hud_off_hint", "引擎没能准备这张卡，原因见下。",
     "The renderer could not prepare it; the reason follows."},
    {"orbit_ring", "快捷键环", "Shortcut ring"},
    {"orbit_desc", "把桌面上的快捷方式排成一圈画在壁纸上，单击启动。",
     "Puts the desktop's shortcuts in a ring on the wallpaper. One click starts one."},
    {"orbit_period", "转一圈", "Revolution"},
    {"orbit_stopped", "停住", "Stopped"},
    {"orbit_count", "每环数量", "Icons per ring"},
    {"orbit_rings", "环数", "Rings"},
    // The two knobs together decide what is on the desktop, so the number drawn is stated rather than
    // left to arithmetic: with 43 shortcuts and 17 per ring it is not 17 times anything.
    {"orbit_shown", "环上共", "On the rings"},
    {"orbit_shown_unit", "个", "icons"},
    {"orbit_shape", "形状", "Shape"},
    {"orbit_shape_ellipse", "椭圆", "Ellipse"},
    {"orbit_shape_funnel", "漏斗", "Funnel"},
    {"orbit_shape_star", "五角星", "Star"},
    {"orbit_shape_desc", "椭圆和五角星绕同一个环心；漏斗一层层往下收，口大颈小。",
     "The ellipse and the star share one centre; the funnel steps downward, wide at the mouth and "
     "narrow at the neck."},
    {"orbit_size", "圆环大小", "Ring size"},
    {"orbit_icon_size", "图标大小", "Icon size"},
    {"orbit_pick", "选择图标", "Icons on the ring"},
    {"orbit_pick_edit", "挑选", "Choose"},
    {"orbit_pick_desc", "勾选哪几个上环；环上的顺序按名字排。",
     "Tick which shortcuts go on the ring. Their order stays by name."},
    {"orbit_pick_selected", "已选", "Selected"},
    {"orbit_pick_all", "全部", "All"},
    {"orbit_pick_none", "清空", "None"},
    {"orbit_pick_close", "完成", "Done"},
    {"orbit_pick_empty", "桌面上没有可读的快捷方式。", "No desktop shortcuts could be read."},
    {"orbit_unavailable", "快捷键环不可用", "Shortcut ring unavailable"},
    {"orbit_pos", "环心位置", "Ring centre"},
    {"orbit_pos_desc", "按住出现的虚线框拖到想要的位置，松手即生效。",
     "Drag the dashed frame that appears; it takes effect on release."},
    {"orbit_home", "回到中央", "Back to centre"},
    {"hide_icons", "隐藏桌面图标", "Hide desktop icons"},
    {"hide_icons_desc", "让位给这一圈。桌面右键 > 查看 > 显示桌面图标 随时改回来。",
     "Gives the desktop to the ring. Right-click the desktop > View > Show desktop icons undoes it."},
    {"tray_alpha", "任务栏透明度", "Taskbar transparency"},
    {"tray_mute_audio", "静音壁纸", "Mute wallpaper"},
    {"audio", "声音", "Sound"},
    {"audio_peak", "送出电平", "level out"},
    {"audio_muted_now", "已静音", "muted"},
    {"audio_no_track", "当前壁纸没有音轨", "no track on this wallpaper"},
    {"audio_volume", "壁纸音量", "Wallpaper volume"},
    {"audio_mute", "静音壁纸", "Mute wallpaper"},
    {"dl_taken", "已取 ", "Downloaded "},
    {"dl_eta_a", "还要 ", "about "},
    {"dl_eta_b", " 秒", " s left"},
    {"preview_sound_on", "预览出声：开", "Preview sound: on"},
    {"preview_sound_off", "预览出声：关", "Preview sound: off"},
    {"audio_desc", "只有真正设为壁纸、且包里带音轨的那一张会响；被全屏窗口盖住、锁屏或显示器熄灭时"
                   "自动静音。这颗只管桌面那一遍：预览的声音是卡片行那个喇叭，两者互不影响。"
                   "此滑条是感知刻度，25 是默认。",
     "Only the wallpaper actually on the desktop sounds, and only if its package carries a track. "
     "It goes quiet under a fullscreen window, a locked session or a powered-down monitor. This "
     "switch covers the desktop only - the preview has its own speaker on the card row, and the two "
     "do not touch each other. The slider is perceptual; 25 is the default."},

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
    // The empty page's own words: `none` stays the value of a dropdown ("host layer: none"), this one
    // says what is missing, because it sits where a wallpaper picture would have been.
    {"no_wallpaper_info", "未获取到壁纸信息", "no wallpaper to show"},
    {"host_layer", "桌面层级", "Desktop layer"},
    {"cpu", "进程 CPU", "Process CPU"},
    // The three labels on the window's bottom line. CPU and GPU are the same letters in both languages
    // here, and keeping them in the table is what lets the line read as one string built from parts.
    {"stat_cpu", "CPU", "CPU"},
    {"stat_mem", "内存", "Memory"},
    {"stat_gpu", "GPU", "GPU"},
    // The two GPU readings name whose GPU they mean, and the per-engine parts name the engines.
    {"gpu_self", "本程序", "this app"},
    {"gpu_machine", "整机", "whole PC"},
    {"gpu_3d", "3D 引擎", "3D engine"},
    {"gpu_decode", "视频解码", "video decode"},
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
    {"adapter", "显卡", "Adapter"},
    {"device", "设备", "Device"},
    {"wallpaper", "壁纸", "Wallpaper"},
    {"not_connected_hint", "渲染器没在跑。点“启动渲染器”，或自己运行 WallpaperRenderer.exe。", "The renderer is not running. Press Start renderer, or run WallpaperRenderer.exe yourself."},
    // The window is frameless, so its own title-bar buttons need words.
    {"win_min", "最小化", "Minimize"},
    {"win_max", "最大化", "Maximize"},
    {"win_restore", "还原", "Restore"},
    // The tray menu. Closing the window only hides it now, so these are the commands that have to
    // work without it; 上一张/下一张 reuse "prev"/"next" above so both say the same thing.
    {"tray_quit", "退出", "Quit"},
    {"tray_auto_play", "自动播放", "Auto play"},
    {"tray_stop_play", "停止播放", "Stop playing"},
    // Two holds, each on the thing the row above it governs: the wallpaper's own motion, and the
    // ring's turning. Named 暂停 rather than 停止 because neither stops anything for good.
    {"tray_pause_wallpaper", "暂停壁纸", "Pause the wallpaper"},
    {"tray_pause_ring", "暂停图标", "Pause the icons"},
    // 在线：一个站 = sources 目录里的一个 json。标签写在这里而不是写在那份描述里，是为了"中文默认 +
    // 英文开关"这条约定只有一处要维护；那份文件只描述端点和字段。
    {"tab_online", "在线壁纸", "Online wallpapers"},
    {"online_filters", "筛选", "Filters"},
    {"online_bad_json", "这份描述不是合法的 JSON", "This description is not valid JSON"},
    {"online_not_object", "这份描述不是一个对象", "This description is not a JSON object"},
    {"online_bad_name", "name 只能用字母数字和 . _ -，且不能为空", "name may only use letters, digits and . _ -, and must not be empty"},
    {"online_bad_homepage", "homepage 必须以 http:// 或 https:// 开头", "homepage must start with http:// or https://"},
    {"online_write_failed", "写不进去", "Could not write the file"},
    {"online_load_failed", "这份描述没被加载", "This description did not load"},
    {"online_add", "添加", "Add"},
    {"online_cancel", "取消", "Cancel"},
    {"online_manage", "管理", "Manage"},
    {"online_done", "完成", "Done"},
    {"online_delete", "删除", "Delete"},
    {"online_editor_title", "新地址源", "New source"},    {"online_editor_hint", "填好按保存就能用。name 是缓存文件名，留空存不进去。", "Fill it in and save; name is the cache file name and must not be empty."},
    {"online_save", "保存", "Save"},
    {"online_address_group", "地址", "Sources"},
    {"online_expand", "展开全部", "Expand"},
    {"online_collapse", "折叠", "Collapse"},
    {"online_confirm_delete", "确认", "Confirm"},


    {"online_query", "搜索词", "Search"},
    {"online_search", "搜索", "Search"},
    {"online_empty", "还没有结果：输入一个词，按回车", "Nothing yet - type a word and press Enter"},
    {"online_page", "第", "Page"},
    {"online_items", "条", "items"},
    {"online_total", "共", "of"},
    {"online_prev", "上一页", "Previous"},
    {"online_next", "下一页", "Next"},
    {"online_resolution", "分辨率下限", "Resolution floor"},
    // The same word means two different things on two sites: a floor to search from on wallhaven, an
    // exact file to take here. 4kwallpapers names its row through the site-qualified key.
    {"online_4kwallpapers_resolution", "分辨率", "Resolution"},
    {"online_category", "分类", "Category"},
    {"online_hero_failed", "原图没取到，现在是站点预览", "original not fetched - this is the site preview"},
    {"online_hero_retry", "重试", "Retry"},
    // The clip's own two sentences: while it is 10 MB away the big picture is a still frame of it, and
    // that has to read as "还在取", not as a wallpaper that stopped moving.
    {"online_clip_failed", "视频没取到，画面是它的一帧", "clip not fetched - this is one frame of it"},
    {"online_preview_rung", "预览清晰度", "Preview clarity"},
    // The strip card's own failure line. It names what is missing (the cover, not the wallpaper) so
    // it cannot be read as the 原图 message above it.
    {"online_thumb_failed", "封面没取到", "Cover did not arrive"},
    {"online_thumb_retry", "点此重试", "Click to retry"},
    {"online_sort", "排序", "Sort by"},
    {"online_purity", "内容分级", "Content rating"},
    {"opt_wallhaven_sort_relevance", "相关", "Relevance"},
    {"opt_wallhaven_sort_newest", "最新", "Newest"},
    {"opt_wallhaven_sort_top", "最热", "Most favourited"},
    {"opt_wallhaven_purity_100", "只收安全", "Safe only"},
    {"opt_wallhaven_purity_110", "安全加暧昧", "Safe plus sketchy"},
    {"opt_wallhaven_purity_111", "全部（含成人）", "Everything, adults included"},
    {"online_import", "入库", "Import"},
    {"online_downloading", "下载中", "Downloading"},
    {"online_importing", "入库中", "Importing"},
    {"online_imported", "已入库", "Imported"},
    {"online_already", "这张已经在库里", "Already in the library"},
    {"online_download_failed", "下载失败", "Download failed"},
    {"online_not_image", "站点回的不是图片", "The site answered with something that is not a picture"},
    {"online_gif", "动图只会取第一帧，不入库", "An animated gif would be imported as its first frame"},
    {"online_unsupported", "这个类型进不了库", "This file type cannot be imported"},
    {"online_need_renderer", "入库要连着渲染器", "Importing needs the renderer running"},
    {"online_no_answer", "渲染器没有回音，看它的日志", "The renderer never answered; check its log"},
    {"online_no_sources", "没有读到任何在线源描述", "No online source description was loaded"},
    {"online_offsite", "源描述的地址不在它自己的站上", "The source points outside its own site"},
    // A delete the renderer never completed. Not "it failed because X" - what was measured is that the
    // package is still in the catalog.
    {"delete_still_there", "问了几次还在库里", "Still in the library after several asks"},
    {"tray_hint", "已退到托盘，壁纸继续在放；点托盘图标就能再打开", "Hidden to the tray - the wallpaper keeps playing. Click the tray icon to open this window again."},
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

// The clip kinds the picker offers and the ones it routes to `addvideo`. The engine's import is
// extension-agnostic - it probes the file with FFmpeg, keeps the real suffix in the package and lets
// the manifest's "renderer": "video" decide how it is drawn - so this list exists only to tell a clip
// from a picture, which WIC cannot do for a video. The bundled avcodec registers the matroska/webm
// demuxer and vp8 / vp9 / av1 decoders, so .webm and .mkv - same demuxer, different container name -
// need nothing but the name here.
const QStringList kVideoExt { "mp4", "webm", "mkv" };

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
    transition_ = s.value("transition", "liquid").toString();
    previewRung_ = s.value("preview_rung").toString();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &Bridge::tick);
    timer_->start(1000);
    // 33 ms is the same cadence LivePreview polls at; previewBeat turns that into one heartbeat
    // every ~700 ms.
    previewTimer_ = new QTimer(this);
    connect(previewTimer_, &QTimer::timeout, this, &Bridge::previewBeat);
    // The online page's replies land here. Both handlers ignore a source the page has already left,
    // which is what a switched-away search would otherwise do: paint the other site's pictures.
    connect(&online_, &OnlineFetcher::searched, this,
            [this](const QString& name, const QVariantList& rows, const QVariantMap& meta,
                   const QString& error) {
                if (name != onlineSource_) return;
                onlineBusy_ = false;
                if (!error.isEmpty()) {
                    onlineError_ = error;
                    emit onlineChanged();
                    return;
                }
                onlinePage_ = meta.value("page").toInt();
                onlineLastPage_ = meta.value("lastPage").toInt();
                onlineTotal_ = meta.value("total").toLongLong();
                // A description file that never says anything about its pager is a site that publishes
                // one (wallhaven and 4kwallpapers both do), and the numbered window is the shape that
                // was measured with - so the key's absence means known, not unknown.
                onlinePagesKnown_ = meta.contains("pagesKnown") ? meta.value("pagesKnown").toBool()
                                                                : true;
                onlineRows_.clear();
                for (const auto& v : rows) {
                    QVariantMap row = v.toMap();
                    const QString have = libraryIdFor(row.value("page").toString());
                    // What the renderer can actually take: a still goes to WIC, a clip to FFmpeg. A gif
                    // would come in as its first frame and sit there motionless, which is not the
                    // picture the tile just showed - so it is refused on the card, not after a press.
                    const QString mime = row.value("mime").toString().toLower();
                    static const QStringList accept { "image/jpeg", "image/png", "image/webp",
                                                      "image/bmp", "image/tiff", "video/mp4",
                                                      "video/webm", "video/x-matroska" };
                    const bool can = accept.contains(mime);
                    const QString why = mime == QLatin1String("image/gif") ? t("online_gif")
                                  : can ? QString() : t("online_unsupported") + QStringLiteral(": ") + mime;
                    row.insert("state", !have.isEmpty() ? QStringLiteral("imported")
                                 : can ? QStringLiteral("new") : QStringLiteral("unsupported"));
                    row.insert("detail", have.isEmpty() ? why : have);
                    onlineRows_.push_back(row);
                }
                emit onlineChanged();
            });
    connect(&online_, &OnlineFetcher::progress, this,
            [this](const QString& url, double fraction, qint64 done, qint64 total) {
                if (!clipUrl_.isEmpty() && url == clipUrl_) {
                    clipProgress_ = fraction;
                    emit onlineClipProgressChanged();
                    clipBytes_ = done;
                    if (total > 0) clipTotal_ = total;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (clipMeterAtMs_ != 0 && now - clipMeterAtMs_ >= 500) {
                        const double inst = double(done - clipMeterBytes_) * 1000.0
                                            / double(now - clipMeterAtMs_) / 1024.0;
                        clipKbps_ = clipKbps_ > 0.0 ? 0.6 * clipKbps_ + 0.4 * inst : inst;
                        clipMeterAtMs_ = now;
                        clipMeterBytes_ = done;
                        emit onlineClipChanged();
                    } else if (clipMeterAtMs_ == 0) {
                        clipMeterAtMs_ = now;
                        clipMeterBytes_ = done;
                        emit onlineClipChanged();
                    }
                }
                for (const auto& v : onlineRows_)
                    if (v.toMap().value("image").toString() == url) {
                        // Only the card this download belongs to reads it, and nothing else rebuilds.
                        // The pending key has to match too: the hero fetches the picture he is looking
                        // at without being pressed, and its bytes must not start a progress bar on
                        // whichever card happens to hold the one import he did press.
                        const QString rowKey = v.toMap().value("key").toString();
                        if (pendingState_ == QLatin1String("downloading") && pendingKey_ == rowKey) {
                            progress_ = fraction;
                            emit onlineProgressChanged();
                        }
                        return;
                    }
            });
    loadOnlineSources();
    refresh();
}

Bridge::~Bridge() = default;

QStringList Bridge::languages() const { return {"zh", "en"}; }

QStringList Bridge::qualityLevels() const {
    return {"package", "ultra", "high", "medium", "low", "battery"};
}

// The order is the number the shaders branch on (Contract.hlsl, uPerf.w); adding one here means
// adding a case in Image.hlsl and an Engine/Packages/WallpaperManager.hpp enum member, in order.
QStringList Bridge::fitLevels() const { return {"fill", "fit", "stretch", "center", "tile"}; }
QStringList Bridge::hudCornerLevels() const { return {"top-right", "top-left", "bottom-right", "bottom-left"}; }
// Same order as HudFrameName() in the renderer. The two lists are the one place this
// window has to agree with a shader about what a number means, so they are named, not
// indexed - a picker showing "3" tells nobody what it draws.
QStringList Bridge::chartTypeLevels() const { return {"bar", "donut", "line"}; }

// The three trajectories, by the names the renderer stores and answers with, so the label the row
// shows is looked up from the value that is actually in force rather than from the row's position.
QStringList Bridge::orbitShapeLevels() const { return {"ellipse", "funnel", "star"}; }

QStringList Bridge::chartCornerLevels() const {
    return {"top-right", "top-left", "bottom-right", "bottom-left"};
}

QStringList Bridge::hudFrameLevels() const {
    return {"notch", "basetab", "ticks", "topbar", "band", "split"};
}

// The three ways the big preview can change when a different wallpaper is picked. "none" skips
// the frame capture as well, which is the only setting that makes a switch cost nothing.
QStringList Bridge::transitionLevels() const { return {"none", "dissolve", "liquid"}; }

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
    // The online page's option labels were built through t() when the source was picked, so they are
    // rebuilt here too - otherwise the page keeps the language it was opened in. The results themselves
    // stay: a language switch is not a new search.
    rebuildOnlineOptions();
    emit onlineChanged();
}

void Bridge::setTargetMonitor(const QString& tag) {
    if (tag == target_) return;
    target_ = tag;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("target", target_);
    emit targetChanged();
}

void Bridge::setTransition(const QString& mode) {
    if (mode == transition_ || mode.isEmpty()) return;
    transition_ = mode;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("transition", transition_);
    emit transitionChanged();
}

void Bridge::setPreviewRung(const QString& which) {
    if (which == previewRung_ || which.isEmpty()) return;
    previewRung_ = which;
    QSettings s(QSettings::IniFormat, QSettings::UserScope, "SmartWallpaper", "ui");
    s.setValue("preview_rung", previewRung_);
    emit previewRungChanged();
    // The card under the big picture was fetched at the old rung, so the answer about it is stale:
    // drop it (which stops the second pass through the QML gate) and ask again at the new one.
    const QString key = heroKey_;
    clearClip();
    if (!key.isEmpty()) requestClip(key);
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
    // Shorter still once we have stopped believing it is there: an open attempt against a pipe nobody
    // owns is answered in under a millisecond, so the only thing the remaining budget buys is a few
    // retries of a race the next 2 s poll would catch anyway. Measured with the renderer gone, over
    // 12 s of WM_NULL probes at 150 ms: median 0.1 ms, p95 1.0 ms, one sample over 100 ms - a 172 ms
    // hiccup during the two polls it takes to *discover* the renderer is gone (that window still runs
    // at the connected budget, capped by IPCCall's 250 ms grace). Before this pair of changes the same
    // measurement gave median 190 ms, p95 411 ms, max 448 ms, 22 of 43 samples over 100 ms.
    const int timeout = connected_ ? 400 : 60;
    const bool okStatus = request(R"({"cmd":"status"})", timeout, &status);
    if (okStatus) request(R"({"cmd":"list"})", timeout, &list);

    if (!okStatus) {
        if (++misses_ >= 2 && connected_) {
            connected_ = false;
            // Stop asking right away rather than letting the heartbeat discover it on the next beat:
            // that next beat is another 500 ms of frozen window.
            if (previewTimer_->isActive()) previewTimer_->stop();
            // Forget which pass we were watching, so the next refresh after the renderer comes back
            // re-requests it. The engine ends a pass three seconds after the beats stop, and
            // `showPreview` only speaks when the id changes - leaving the id set would mean the
            // second pass never returns until he clicks a different wallpaper.
            previewId_.clear();
            // An import that was in flight cannot land: the renderer is the one that copies the file
            // into the library, so the card is told now rather than left spinning until the wait times
            // out on a poll that no longer runs at the connected rate.
            if (importRow_ >= 0) {
                setPending(pendingKey_, QStringLiteral("failed"), t("online_need_renderer"));
                importRow_ = -1;
            }
            emit stateChanged();
            engineDied();
        }
        if (!connected_ && timer_->interval() != 2000) timer_->start(2000);
        reportStep();   // a renderer that went away mid-step must not leave the wait armed
        return;
    }
    if (!connected_ || timer_->interval() != 1000) timer_->start(1000);
    // Back up: the beat was stopped when the renderer went quiet, and `showPreview` is only called
    // when the page changes, so nothing else would start it again. An empty id means the page has no
    // second pass to watch, which is the one case that must stay stopped.
    if (!previewId_.isEmpty() && !previewTimer_->isActive()) previewTimer_->start(33);
    misses_ = 0;
    connected_ = true;
    ingest(status, list);
    emit stateChanged();
    reportStep();
}

void Bridge::ingest(const Json& status, const Json& list) {
    uptime_ = status.numOr("uptime_s");
    // Which process this snapshot came from: the watchdog reads that engine's own log to tell a
    // crash from a stop he asked for.
    lastPid_ = status.numOr("pid");
    host_ = QString::fromStdString(status.strOr("host"));
    root_ = QString::fromStdString(status.strOr("root"));
    paused_ = status.boolOr("paused");
    // Default true: a renderer older than this field answers nothing, and "the desktop is showing a
    // wallpaper" is the honest reading of that, not "it is off".
    desktopOn_ = status.boolOr("desktop_on", true);
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
        if (const Json* p = status.find("preview"); p && p->isObject()) {
            // The clip's own transport state, from the same snapshot as everything else here. When the
            // pass is off the field is absent, which is "not paused" - the next look at that clip
            // starts playing, because a video instance does not survive the pass ending.
            const bool pz = p->boolOr("video_paused");
            if (pz != previewPaused_) {
                previewPaused_ = pz;
                emit previewVideoPausedChanged();
            }
            if (!p->boolOr("on")) {
                const QString id = previewId_;
                previewId_.clear();
                showPreview(id);
            }
        }
    }
    qualityIsGlobal_ = status.boolOr("quality_is_global");
    quality_ = qualityIsGlobal_ ? QString::fromStdString(status.strOr("quality", "high"))
                                : QStringLiteral("package");
    imageFit_ = QString::fromStdString(status.strOr("image_fit", "fill"));
    trayAlpha_ = status.intOr("tray_alpha", 0);
    audioVolume_ = status.intOr("audio_volume", 25);
    audioMuted_ = status.boolOr("audio_muted");
    audioPlaying_ = status.boolOr("audio_playing");
    audioSource_ = QString::fromStdString(status.strOr("audio_source", ""));
    audioPeak_ = status.intOr("audio_peak", 0);
    previewAudio_ = status.boolOr("preview_audio");
    hud_ = status.boolOr("hud");
    hudReady_ = status.boolOr("hud_ready");
    hudError_ = QString::fromStdString(status.strOr("hud_error", ""));
    hudCorner_ = QString::fromStdString(status.strOr("hud_corner", "top-right"));
    hudScale_ = status.intOr("hud_scale", 100);
    hudFrame_ = QString::fromStdString(status.strOr("hud_frame", "notch"));
    hudDrag_ = status.boolOr("hud_drag");
    orbit_ = status.boolOr("orbit");
    orbitIcons_ = status.boolOr("orbit_icons");
    orbitReady_ = status.boolOr("orbit_ready");
    orbitClickable_ = status.boolOr("orbit_clickable");
    orbitPeriod_ = status.intOr("orbit_period_s", 45);
    orbitHeld_ = status.boolOr("orbit_held");
    orbitPerRing_ = status.intOr("orbit_per_ring", 12);
    orbitRings_ = status.intOr("orbit_rings", 1);
    orbitShown_ = status.intOr("orbit_shown", 0);
    orbitShape_ = QString::fromStdString(status.strOr("orbit_shape", "ellipse"));
    orbitSlots_ = status.intOr("orbit_slots", 0);
    orbitScale_ = status.intOr("orbit_scale", 100);
    orbitIconScale_ = status.intOr("orbit_icon_scale", 100);
    orbitPickable_ = status.intOr("orbit_pickable", 0);
    orbitPicked_ = status.boolOr("orbit_picked");
    orbitIconList_.clear();
    if (const Json* p = status.find("orbit_icon_list"); p && p->isArray())
        for (auto& e : p->items()) {
            QVariantMap row;
            row.insert("index", e.intOr("i"));
            row.insert("name", QString::fromStdString(e.strOr("name")));
            row.insert("picked", e.boolOr("picked"));
            orbitIconList_.push_back(row);
        }
    orbitError_ = QString::fromStdString(status.strOr("orbit_error", ""));
    iconsHidden_ = status.boolOr("icons_hidden");
    orbitDrag_ = status.boolOr("orbit_drag");
    if (const Json* p = status.find("orbit_pos"); p) orbitCustom_ = p->isArray();
    chart_ = status.boolOr("chart");
    chartReady_ = status.boolOr("chart_ready");
    chartType_ = QString::fromStdString(status.strOr("chart_type", "bar"));
    chartCorner_ = QString::fromStdString(status.strOr("chart_corner", "bottom-left"));
    chartScale_ = status.intOr("chart_scale", 100);
    chartFrame_ = QString::fromStdString(status.strOr("chart_frame", "notch"));
    chartDrag_ = status.boolOr("chart_drag");
    chartHistory_ = status.intOr("chart_history", 0);
    if (const Json* p = status.find("chart_pos"); p) chartCustom_ = p->isArray();
    // null while the card sits on a corner preset, [x, y] once it has been dragged.
    if (const Json* p = status.find("hud_pos"); p) hudCustom_ = p->isArray();
    if (const Json* p = status.find("process"); p && p->isObject()) {
        cpu_ = p->numOr("cpu_percent");
        cpuWin_ = p->numOr("cpu_window_s");
        ws_ = p->numOr("working_set_mb");
        // The engine leaves this out when it has no NVML to ask; -1 keeps that distinguishable from
        // "the GPU is idle", which is what a 0 would say.
        gpu_ = p->find("gpu_percent") ? p->find("gpu_percent")->asNumber() : -1.0;
        // This process's own share, from the per-engine counters. Same -1 rule.
        gpuSelf_ = p->find("gpu_self_percent") ? p->find("gpu_self_percent")->asNumber() : -1.0;
        gpu3d_ = p->find("gpu_self_3d") ? p->find("gpu_self_3d")->asNumber() : -1.0;
        gpuDec_ = p->find("gpu_self_decode") ? p->find("gpu_self_decode")->asNumber() : -1.0;
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
        // Which decoder each screen landed on used to be aggregated here for a line on the 通用 page.
        // He took that line out (2026-10-08); `--ctl status` still answers it per monitor, and nothing
        // in this process reads it any more.
        if (!target_.isEmpty() && target_ != "all") {
            bool stillThere = false;
            for (auto& v : monitors_)
                if (v.toMap().value("tag").toString() == target_) stillThere = true;
            if (!stillThere) setTargetMonitor("all");
        }
    }

    // What this window acts on: the screen the target selector points at, or the first one when the
    // target is "all". The tray's tooltip and its step toast both name this.
    showingId_.clear();
    showingName_.clear();
    for (auto& v : monitors_) {
        const QVariantMap row = v.toMap();
        if (target_ != "all" && !target_.isEmpty() && row.value("tag").toString() != target_) continue;
        showingId_ = row.value("wallpaper").toString();
        showingName_ = row.value("wallpaperName").toString();
        break;
    }

    catalog_.clear();
    // The catalog comes from status, not list: only status carries the preview file paths.
    if (const Json* c = status.find("catalog"); c && c->isArray()) {
        for (auto& e : c->items()) {
            QVariantMap row;
            // Rebuilt thumbnails have to replace the bytes this window already loaded, and Qt's image
            // cache is keyed by URL - so the URL carries the file's own write time. Without this a
            // window that was open while the renderer rebuilt a card keeps showing the old picture
            // (measured: a card fixed at 11:39 still displayed the green picture from 11:31).
            const long long stamp = e.find("thumb_ms") ? e.find("thumb_ms")->asNumber() : 0.0;
            for (auto& kv : e.members()) {
                const QVariant v = toVariant(kv.second);
                if (kv.first == "thumb" || kv.first == "thumb_large") {
                    QString url = QUrl::fromLocalFile(QDir::fromNativeSeparators(v.toString())).toString();
                    if (stamp > 0) url += QStringLiteral("?v=%1").arg(stamp);
                    row.insert(QString::fromStdString(kv.first), url);
                } else if (kv.first == "file") {
                    // The engine gives a native path; Qt reads "H:\dir\image.jpg" as a url whose
                    // scheme is "h", and the image silently never loads ("协议"h"未知). The stamp is
                    // deliberately not appended here - it is the thumbnail's write time, and this is
                    // the package's own file, which the renderer never rebuilds.
                    row.insert(QStringLiteral("file"),
                               QUrl::fromLocalFile(QDir::fromNativeSeparators(v.toString())).toString());
                } else
                    row.insert(QString::fromStdString(kv.first), v);
            }
            if (const Json* fr = e.find("frames"); fr && fr->isArray()) {
                QStringList urls;
                for (auto& f : fr->items()) {
                    QString url = QUrl::fromLocalFile(QDir::fromNativeSeparators(QString::fromStdString(f.asString()))).toString();
                    if (stamp > 0) url += QStringLiteral("?v=%1").arg(stamp);
                    urls << url;
                }
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

    // Which site pages the library already holds, read off the packages themselves. The online page's
    // 「已入库」 says what the catalog says, so it stays true across a restart and turns itself off
    // when he deletes the package by hand - a note this window kept about its own imports could not.
    originIds_.clear();
    for (const auto& v : catalog_) {
        const QVariantMap row = v.toMap();
        const QString origin = row.value("origin").toString();
        if (!origin.isEmpty()) originIds_.insert(origin, row.value("id").toString());
    }
    bool rowsChanged = false;
    for (int i = 0; i < onlineRows_.size(); ++i) {
        QVariantMap row = onlineRows_[i].toMap();
        // Only the two states that are about the library itself. A card refused because of its file
        // type, or one whose download failed, keeps the reason it carries.
        const QString was = row.value("state").toString();
        if (was != QLatin1String("new") && was != QLatin1String("imported")) continue;
        const QString have = libraryIdFor(row.value("page").toString());
        const bool nowIn = !have.isEmpty();
        const bool saidIn = was == QLatin1String("imported");
        if (nowIn == saidIn && row.value("detail").toString() == have) continue;
        row.insert("state", nowIn ? QStringLiteral("imported") : QStringLiteral("new"));
        row.insert("detail", have);
        onlineRows_[i] = row;
        rowsChanged = true;
    }
    if (rowsChanged) emit onlineChanged();   // once, not once per card
    // The command answering "queued" only means the renderer took it. The import is finished when a
    // package carrying that page is in the catalog, and not before.
    if (importRow_ >= 0) {
        const QString have = libraryIdFor(importOrigin_);
        if (!have.isEmpty()) {
            setPending(QString(), QString(), QString());
            importRow_ = -1;
            say(t("online_imported") + QStringLiteral(": ") + have, false);
        } else if (QDateTime::currentMSecsSinceEpoch() - importAtMs_ > 20000) {
            // The renderer's own answer went to its log, which is the only place a refused import is.
            const QString why = t("online_no_answer") + QStringLiteral(": ") + logPath_;
            setPending(pendingKey_, QStringLiteral("failed"), why);
            importRow_ = -1;
            say(why, true);
        }
    }
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

QString Bridge::rendererExePath() const {
    // Prefer the folder the engine itself reported in its status snapshot: that is the copy which was
    // actually running, and on a dev tree the window and the engine do not necessarily live together.
    QStringList cand;
    if (!root_.isEmpty()) cand << QDir(root_).absoluteFilePath(QStringLiteral("WallpaperRenderer.exe"));
    cand << QDir(QCoreApplication::applicationDirPath())
                .absoluteFilePath(QStringLiteral("WallpaperRenderer.exe"));
    cand << QDir(QCoreApplication::applicationDirPath())
                .absoluteFilePath(QStringLiteral("../RelWithDebInfo/WallpaperRenderer.exe"));
    for (const QString& p : cand)
        if (QFileInfo::exists(p)) return QFileInfo(p).absoluteFilePath();
    return QString();
}

bool Bridge::launchRenderer(bool announce) {
    const QString path = rendererExePath();
    if (path.isEmpty()) {
        if (announce) say(t("failed") + ": WallpaperRenderer.exe", true);
        else qInfo("engine went away, no WallpaperRenderer.exe under %s", qPrintable(root_));
        return false;
    }
    QProcess::startDetached(path, {});
    if (announce) say(t("start"), false);
    return true;
}

bool Bridge::startRenderer() {
    // His button: clear the watchdog's budget so a manual start is followed by a fresh allowance,
    // and drop the deliberate-quit flag in case he stopped it here earlier.
    relaunches_ = 0;
    relaunchWindowMs_ = 0;
    relaunchAtMs_ = 0;
    quitAsked_ = false;
    return launchRenderer(true);
}

// The engine dying takes the desktop wallpaper with it and leaves this window reading "渲染器未启动"
// over a black screen, so the window that saw it running is the only thing that can bring it back.
// Quietly: he asked for a crash to be a crash, not a prompt. Bounded, because an engine that dies
// again as soon as it starts would otherwise be relaunched on every poll forever - at most one
// relaunch per 30 s and three per 10 min, then it waits for the button in 壁纸进程管理.
// A stop he asked for and a crash look the same from here: the pipe simply stops answering. The
// engine's own log tells them apart - it writes "final: ..." as its last act on a clean shutdown
// (`Application::Stop` logs "stopping: <why>" up to ~41 KB / 450 lines before it, so only the last
// line is usable), and a crash ends on whatever it was logging: the file of the one that took his
// wallpaper down at 02:20:56 ends mid-video-line with no farewell. No log readable counts as a
// crash, because "he stopped it" is the claim that needs the evidence.
bool Bridge::engineStoppedItself() const {
    if (root_.isEmpty() || lastPid_ <= 0) return false;
    QFile f(QDir(root_).absoluteFilePath(
        QStringLiteral("logs/renderer-%1.log").arg(lastPid_)));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qint64 back = qMin<qint64>(f.size(), qint64(1) << 17);
    f.seek(f.size() - back);
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return !lines.isEmpty() && lines.last().contains(QStringLiteral("final:"));
}

void Bridge::engineDied() {
    if (quitAsked_) return;
    if (engineStoppedItself()) {
        qInfo("engine stopped itself (%lld), leaving it down", static_cast<long long>(lastPid_));
        return;
    }
    if (engineStoppedItself()) {
        qInfo("engine stopped on request, leaving it down");
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - relaunchWindowMs_ > 600000) { relaunchWindowMs_ = now; relaunches_ = 0; }
    if (relaunches_ >= 3) {
        qInfo("engine went away, giving up after %d relaunches", relaunches_);
        return;
    }
    if (now - relaunchAtMs_ < 30000) return;
    relaunchAtMs_ = now;
    ++relaunches_;
    qInfo("engine went away, relaunching (attempt %d)", relaunches_);
    launchRenderer(false);
}

void Bridge::setPaused(bool p) {
    Json out;
    request(p ? R"({"cmd":"pause"})" : R"({"cmd":"resume"})", 1500, &out);
    paused_ = p;
    emit stateChanged();
}

void Bridge::setDesktopOn(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"desktop","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    // Optimistic like the pause switch: the reply only says "queued", and the next status read either
    // confirms it or puts the button's label back.
    desktopOn_ = on;
    emit stateChanged();
}

void Bridge::setOrbitHeld(bool held) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbithold","arg":"{}"}})", held ? "on" : "off");
    request(body.c_str(), 1500, &out);
    orbitHeld_ = held;      // the reply is queued, so this is the same optimistic tick the switch uses
    emit stateChanged();
    refresh();
}

// The tray's 暂停壁纸 row. A still image on the desktop has nothing playing, so pausing it would move
// a tick and change nothing - that is a dead control, so it goes grey instead. An id this window has
// no catalog row for (mid-switch, or a renderer older than the `type` field) counts as dynamic: the
// row being live is the older, better-tested behaviour, and grey on a guess would be worse.
bool Bridge::showingDynamic() const {
    for (const QVariant& v : catalog_) {
        const QVariantMap row = v.toMap();
        if (row.value("id").toString() != showingId_) continue;
        return row.value("type").toString() != QStringLiteral("image");
    }
    return true;
}

void Bridge::setPreviewVideoPaused(bool p) {
    // The transport is per player and this one names the surface he is looking at, so pausing the big
    // picture cannot stop the wallpaper behind it (Application's `video` command, arg2 = "preview").
    const std::string body = std::format(R"({{"cmd":"video","arg":"{}","arg2":"preview"}})",
                                         p ? "pause" : "resume");
    Json out;
    request(body.c_str(), 1500, &out);
    // `video` is a queued command, so the pipe answers "queued" and the real reply only ever appears
    // in the engine's log: the state cannot be read back from here. The button is enabled exactly when
    // the preview player exists (a video hero with a live pass), so apply the request now and let the
    // snapshot correct it within a second if the engine disagreed.
    if (p == previewPaused_) return;
    previewPaused_ = p;
    emit previewVideoPausedChanged();
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

// All three go through the renderer rather than an ini of this window's own: the card is drawn on
// the desktop, so the only place its true state lives is the engine. Each one calls refresh() after
// sending, because the pipe answers "queued" and there is nothing to read out of that reply.
void Bridge::setHud(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"hud","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    refresh();
}

void Bridge::setHudCorner(const QString& corner) {
    Json out;
    std::string body = std::format(R"({{"cmd":"hudpos","arg":"{}"}})", corner.toStdString());
    request(body.c_str(), 1500, &out);
    refresh();
}

void Bridge::setHudFrame(const QString& style) {
    Json out;
    std::string body = std::format(R"({{"cmd":"hudframe","arg":"{}"}})", style.toStdString());
    request(body.c_str(), 1500, &out);
    refresh();
}

void Bridge::setHudScale(int pct) {
    Json out;
    std::string body = std::format(R"({{"cmd":"hudscale","arg":"{}"}})", pct);
    request(body.c_str(), 1500, &out);
    refresh();
}

void Bridge::setHudDrag(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"huddrag","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    // Echoed locally because the pipe's answer is only "queued": waiting for the next snapshot would
    // leave the switch showing the opposite of the desktop for up to a second after every click.
    hudDrag_ = on;
    emit stateChanged();
    refresh();
}

void Bridge::resetHudPos() {
    Json out;
    request(R"({"cmd":"hudhome"})", 1500, &out);
    hudCustom_ = false;
    emit stateChanged();
    refresh();
}

// The shortcut ring. Echoed locally for the same reason the drag switch is - the pipe answers
// "queued" - but only the switch itself: the period and the count come back from the snapshot, so a
// slider the engine clamped (it holds the ring's cost ceiling) cannot end up showing a number the
// desktop is not using.
void Bridge::setOrbit(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbit","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    orbit_ = on;
    if (!on) orbitClickable_ = false;
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitPeriod(int seconds) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitspeed","arg":"{}"}})", seconds);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitCount(int n) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitcount","arg":"{}"}})", n);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

// Both saved on the renderer's side, and neither echoed locally: the ring clamps the per-ring count
// when the ring count makes the two together exceed what it has rows for, so the number that comes
// back is the one that is drawn and the slider has to read it rather than keep its own guess.
void Bridge::setOrbitRings(int n) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitrings","arg":"{}"}})", n);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitShape(const QString& name) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitshape","arg":"{}"}})", name.toStdString());
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

// No local echo, unlike the switches: these two are read back from the renderer, which is the side
// that clamps, so the slider shows the size the desktop actually got rather than the number typed in.
void Bridge::setOrbitScale(int pct) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitscale","arg":"{}"}})", pct);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitIconScale(int pct) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbiticonscale","arg":"{}"}})", pct);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

// The list travels as names joined with '|', which needs no escaping because Windows forbids that
// character in a file name - so does it forbid " and \, which is why the same shortcut works for the
// border-style verbs. An empty list is sent as `none` rather than as nothing, because nothing means
// `all` to the renderer.
void Bridge::setOrbitPicks(const QStringList& names) {
    Json out;
    std::string joined;
    for (const QString& n : names) {
        if (!joined.empty()) joined += "|";
        joined += n.toStdString();
    }
    if (joined.empty()) joined = "none";
    std::string body = std::format(R"({{"cmd":"orbitpicks","arg":"{}"}})", joined);
    request(body.c_str(), 1500, &out);
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitPicksAll() {
    Json out;
    request(R"({"cmd":"orbitpicks","arg":"all"})", 1500, &out);
    emit stateChanged();
    refresh();
}

void Bridge::setOrbitDrag(bool on) {
    Json out;
    std::string body = std::format(R"({{"cmd":"orbitdrag","arg":"{}"}})", on ? "on" : "off");
    request(body.c_str(), 1500, &out);
    orbitDrag_ = on;
    emit stateChanged();
    refresh();
}

void Bridge::resetOrbitPos() {
    Json out;
    request(R"({"cmd":"orbithome"})", 1500, &out);
    orbitCustom_ = false;
    emit stateChanged();
    refresh();
}

void Bridge::setIconsHidden(bool hide) {
    Json out;
    std::string body = std::format(R"({{"cmd":"icons","arg":"{}"}})", hide ? "on" : "off");
    request(body.c_str(), 1500, &out);
    iconsHidden_ = hide;
    emit stateChanged();
    refresh();
}

// The chart panel's setters mirror the card's: every one goes through the renderer, echoes nothing it
// cannot read back, and calls refresh() because the pipe only answers "queued".
void Bridge::setChart(bool on) {
    Json out;
    request(std::format(R"({{"cmd":"chart","arg":"{}"}})", on ? "on" : "off").c_str(), 1500, &out);
    refresh();
}

void Bridge::setChartType(const QString& t) {
    Json out;
    request(std::format(R"({{"cmd":"charttype","arg":"{}"}})", t.toStdString()).c_str(), 1500, &out);
    refresh();
}

void Bridge::setChartCorner(const QString& corner) {
    Json out;
    request(std::format(R"({{"cmd":"chartpos","arg":"{}"}})", corner.toStdString()).c_str(), 1500, &out);
    refresh();
}

void Bridge::setChartScale(int pct) {
    Json out;
    request(std::format(R"({{"cmd":"chartscale","arg":"{}"}})", pct).c_str(), 1500, &out);
    refresh();
}

void Bridge::setChartFrame(const QString& style) {
    Json out;
    request(std::format(R"({{"cmd":"chartframe","arg":"{}"}})", style.toStdString()).c_str(),
            1500, &out);
    refresh();
}

void Bridge::setChartDrag(bool on) {
    Json out;
    request(std::format(R"({{"cmd":"chartdrag","arg":"{}"}})", on ? "on" : "off").c_str(), 1500, &out);
    // Echoed locally because the pipe's answer is only "queued"; the snapshot reads the truth back,
    // which is what snaps the switch off when the panel is dropped.
    chartDrag_ = on;
    emit stateChanged();
    refresh();
}

void Bridge::resetChartPos() {
    Json out;
    request(R"({"cmd":"charthome"})", 1500, &out);
    chartCustom_ = false;
    emit stateChanged();
    refresh();
}

void Bridge::setTrayAlpha(int pct) {

    Json out;
    const int clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    request(std::format(R"({{"cmd":"taskbar","arg":"{}"}})", clamped).c_str(), 1500, &out);
    // Echo the value rather than polling: the request is blocking, and this arrives from a drag.
    // The once-a-second snapshot still reads tray_alpha back, so a renderer that refused would show
    // up on the next tick.
    trayAlpha_ = clamped;
    emit stateChanged();
}

void Bridge::setAudioVolume(int pct) {
    Json out;
    const int clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    request(std::format(R"({{"cmd":"volume","arg":"{}"}})", clamped).c_str(), 1500, &out);
    // Echoed for the drag, read back from the status block every second - the same deal as the
    // taskbar slider: a renderer that refused would correct itself on the next tick.
    audioVolume_ = clamped;
    emit stateChanged();
}

void Bridge::setAudioMuted(bool muted) {
    Json out;
    request(std::format(R"({{"cmd":"mute","arg":"{}"}})", muted ? "on" : "off").c_str(), 1500, &out);
    audioMuted_ = muted;
    emit stateChanged();
}

void Bridge::setPreviewAudio(bool on) {
    Json out;
    request(std::format(R"({{"cmd":"previewaudio","arg":"{}"}})", on ? "on" : "off").c_str(), 1500, &out);
    previewAudio_ = on;
    emit stateChanged();
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

// The tray's 上一张/下一张. The renderer owns the pool and the pointer, so this window only asks for
// one step. The pipe answers "queued" before anything has been drawn and a switch costs a shader
// compile, so the wallpaper this actually landed on is read out of a later snapshot - reportStep().
void Bridge::stepWallpaper(int delta) {
    if (!connected_) {
        say(t("need_renderer"), true);
        return;
    }
    stepFromId_ = showingId_;
    stepDelta_ = delta;
    stepWaitMs_ = QDateTime::currentMSecsSinceEpoch() + 4000;
    Json out;
    request(std::format(R"({{"cmd":"rotate","arg":"{}"}})", delta < 0 ? "prev" : "next").c_str(), 1500, &out);
    refresh();
}

void Bridge::reportStep() {
    if (stepDelta_ == 0) return;
    const QString what = t(stepDelta_ > 0 ? "next" : "prev");
    if (!showingId_.isEmpty() && showingId_ != stepFromId_) {
        stepDelta_ = 0;
        say(what + ": " + (showingName_.isEmpty() ? showingId_ : showingName_), false);
    } else if (QDateTime::currentMSecsSinceEpoch() > stepWaitMs_) {
        stepDelta_ = 0;
        say(t("failed") + ": " + what, true);
    }
}

void Bridge::addImageFile() {
    const QString file = QFileDialog::getOpenFileName(
        nullptr, t("add_image"), QDir::homePath(),
        "Images and video (*.png *.jpg *.jpeg *.bmp *.webp *.tiff *.mp4 *.webm *.mkv)");
    if (file.isEmpty()) return;
    // One picker for both kinds, but two different packages: addimage hands the file to WIC, which
    // answers "not a decodable image" for a clip, so the extension decides which command to send.
    const bool video = kVideoExt.contains(QFileInfo(file).suffix(), Qt::CaseInsensitive);
    Json out;
    std::string body = std::format(R"({{"cmd":"{}","arg":"{}"}})", video ? "addvideo" : "addimage",
                                   file.toStdString());
    if (!request(body.c_str(), 8000, &out) || !out.boolOr("ok")) {
        say(t("failed") + ": " + QFileInfo(file).fileName(), true);
        return;
    }
    say(t("added_image") + ": " + QFileInfo(file).fileName(), false);
    refresh();
}

void Bridge::addImageFolder() {
    const QString dir = QFileDialog::getExistingDirectory(nullptr, t("add_folder"), QDir::homePath());
    if (dir.isEmpty()) return;
    static const QStringList imgExt { "png", "jpg", "jpeg", "bmp", "webp", "tiff" };
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Files, QDir::Name);
    int queued = 0;
    // Each of these answers as soon as the renderer has taken the command, so the window never waits
    // on the import itself - the copies and their pictures appear as the render loop gets to them.
    for (const QFileInfo& f : entries) {
        const QString suffix = f.suffix().toLower();
        const bool isVideo = kVideoExt.contains(suffix);
        if (!isVideo && !imgExt.contains(suffix)) continue;
        Json out;
        const std::string body = std::format(R"({{"cmd":"{}","arg":"{}"}})", isVideo ? "addvideo" : "addimage",
                                             f.absoluteFilePath().toStdString());
        if (request(body.c_str(), 4000, &out) && out.boolOr("ok")) ++queued;
    }
    if (!queued) {
        say(t("folder_empty") + QStringLiteral(": ") + d.dirName(), true);
        return;
    }
    say(QStringLiteral("%1 %2").arg(queued).arg(t("folder_queued")), false);
    refresh();
}

void Bridge::deleteImage(const QString& id) { deleteImageTry(id, 0); }

// The pipe's answer to a queued command is only "we took it", so it is not evidence the package is
// gone. Success is read back out of the catalog; while the id is still in it, this asks again. A file
// another process still has open is refused with a sharing violation and clears within seconds - the
// first no is not a no. (The case that put this here was my own probe: it read the package's
// wallpaper.json, left the handle open, and asked for the delete. The renderer was right to refuse.)
void Bridge::deleteImageTry(const QString& id, int round) {
    Json out;
    const std::string body = std::format(R"({{"cmd":"delimage","arg":"{}"}})", id.toStdString());
    const bool taken = request(body.c_str(), 4000, &out) && out.boolOr("ok");
    if (!taken) {
        say(t("delete_failed") + QStringLiteral(": ")
                + QString::fromStdString(out.strOr("error").empty() ? out.strOr("code") : out.strOr("error")),
            true);
        return;
    }
    refresh();
    if (!catalogHasId(id)) { say(t("deleted_image") + QStringLiteral(": ") + id, false); return; }
    if (round >= 3) {
        // Still there after four asks. The one reason this window can name without guessing is a
        // package a screen is showing - the renderer refuses that one and says which monitor.
        for (const auto& v : monitors_) {
            const QVariantMap m = v.toMap();
            if (m.value("wallpaper").toString() == id) {
                say(t("delete_in_use") + QStringLiteral(" (") + m.value("tag").toString()
                        + QStringLiteral(")"), true);
                return;
            }
        }
        say(t("delete_still_there") + QStringLiteral(": ") + id, true);
        return;
    }
    QTimer::singleShot(1500, this, [this, id, round] { deleteImageTry(id, round + 1); });
}

bool Bridge::catalogHasId(const QString& id) const {
    for (const auto& v : catalog_)
        if (v.toMap().value("id").toString() == id) return true;
    return false;
}

// ------------------------------------------------------------------ 在线源
//
// A site is a file in `sources`, so its endpoint, its field names and its politeness interval are
// data rather than something compiled in here. What is deliberately not data is the accepting side:
// only the site's own hosts get requests, so a hand-written description cannot turn this window into
// a general fetcher.
void Bridge::loadOnlineSources() {
    const QString dir = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("sources"));
    std::vector<std::string> skipped;
    online_.setSources(OnlineSource::LoadAll(dir.toStdWString(), skipped));
    for (const auto& s : skipped) qWarning("online source skipped: %s", s.c_str());
    onlineSourceItems_.clear();
    for (const auto& s : online_.sources()) {
        QVariantMap row;
        row.insert("value", QString::fromStdString(s.name()));
        row.insert("label", QString::fromStdString(s.label()));
        onlineSourceItems_.push_back(row);
    }
    if (onlineSourceItems_.isEmpty()) {
        onlineError_ = t("online_no_sources") + QStringLiteral(": ") + dir;
        return;
    }
    setOnlineSource(onlineSourceItems_.first().toMap().value("value").toString());
}

void Bridge::setOnlineSource(const QString& name) {
    OnlineSource* s = online_.source(name);
    if (!s) return;
    onlineSource_ = QString::fromStdString(s->name());
    onlineSite_ = QString::fromStdString(s->homepage());
    onlineIntervalMs_ = s->minIntervalMs();
    onlineAsk_ = s->acceptsQuery();
    onlineRows_.clear();
    onlinePage_ = onlineLastPage_ = 1;
    onlineTotal_ = 0;
    onlineError_.clear();
    importRow_ = -1;
    heroKey_.clear();
    heroInFlight_.clear();
    // A size picked on one site is not carried to the next: 4kwallpapers defaults its own import to
    // 3840x2160, and letting that land on motionbgs would preview every card at 19-28 MB without him
    // ever choosing it. Every site starts at the row above - 分辨率下限, the size 入库 takes.
    previewRung_.clear();
    if (!heroFile_.isEmpty()) { heroFile_.clear(); emit onlineHeroFileChanged(); }
    rebuildOnlineOptions();
    emit onlineChanged();
}

// The option rows the page draws come out of the site's own description: a source that offers no sort
// order gets no sort control, instead of a picker that does nothing. Labels go through t(), which is
// why the language switch rebuilds them - see setLang.
void Bridge::rebuildOnlineOptions() {
    OnlineSource* s = online_.source(onlineSource_);
    onlineOptions_.clear();
    onlineRungs_.clear();
    if (!s) return;
    for (const auto& o : s->options()) {
        // The resolution list is the clip preview's menu, kept beside the row the page already draws so
        // the two cannot disagree about what this site offers.
        if (o.key == "resolution")
            for (const auto& v : o.values) onlineRungs_ << QString::fromStdString(v);
        QVariantList vals;
        for (const auto& v : o.values) {
            const QString raw = QString::fromStdString(v);
            const QString key = QStringLiteral("opt_") + onlineSource_ + QLatin1Char('_')
                              + QString::fromStdString(o.key) + QLatin1Char('_') + raw;
            // t() hands back the key when the table has no entry, so an option nobody translated shows
            // the name the site gives it (4kwallpapers' own "CGI", "Black/Dark") and only failing that
            // its raw url slug.
            const QString tr = t(key);
            const QString siteLabel = QString::fromStdString(o.labelFor(v));
            QVariantMap one;
            one.insert("value", raw);
            one.insert("label", tr == key ? siteLabel : tr);
            vals.push_back(one);
        }
        // A row's own name can differ per site: `resolution` is a floor on wallhaven and an exact size
        // here, and the two read differently in Chinese. The site-qualified key wins when it exists.
        const QString siteKey = QStringLiteral("online_") + onlineSource_ + QLatin1Char('_')
                              + QString::fromStdString(o.key);
        const QString rowKey = QStringLiteral("online_") + QString::fromStdString(o.key);
        QString rowLabel = t(siteKey);
        if (rowLabel == siteKey) rowLabel = t(rowKey);
        if (rowLabel == rowKey) rowLabel = QString::fromStdString(o.key);
        QVariantMap row;
        row.insert("key", QString::fromStdString(o.key));
        row.insert("label", rowLabel);
        row.insert("values", vals);
        row.insert("current", QString::fromStdString(s->optionValue(o.key)));
        onlineOptions_.push_back(row);
        // The same list, handed to 预览清晰度 on its own so that group shows exactly the sizes this
        // site has rather than a low/mid/high invented here.
        if (o.key == "resolution") onlineRungs_ = vals;
    }
    // A stored size the new site does not publish (a 4K remembered from motionbgs, now on a site whose
    // top rung is 1440p) is not a choice anybody can see highlighted, so it falls back to the rung
    // 入库 itself would take - which is also the cheapest answer: the file is already the one being
    // fetched, so previewing costs nothing extra until he picks another row.
    QStringList vals;
    for (const auto& v : onlineRungs_) vals << v.toMap().value("value").toString();
    if (!vals.contains(previewRung_)) {
        const QString keep = previewRung_;
        const QString cur = QString::fromStdString(s->optionValue("resolution"));
        if (vals.contains(cur)) previewRung_ = cur;
        else if (!vals.isEmpty()) previewRung_ = vals.first();
        // The pill row binds on this signal, and `onlineChanged` is not enough to move it.
        if (previewRung_ != keep) emit previewRungChanged();
    }
}

void Bridge::onlineSearch() {
    OnlineSource* s = online_.source(onlineSource_);
    if (!s) {
        onlineError_ = t("online_no_sources");
        emit onlineChanged();
        return;
    }
    if (!OnlineFetcher::sameHost(QString::fromStdString(s->BuildSearch({}, 1).url), onlineSite_)) {
        onlineError_ = t("online_offsite") + QStringLiteral(": ") + onlineSite_;
        emit onlineChanged();
        return;
    }
    onlineBusy_ = true;
    onlineError_.clear();
    emit onlineChanged();
    online_.search(*s, onlineQuery_, onlinePage_);
}

void Bridge::onlinePageStep(int delta) {
    const int next = onlinePage_ + delta;
    if (next < 1 || next > onlineLastPage_) return;
    onlinePage_ = next;
    onlineSearch();
}

void Bridge::onlineGoTo(int page) {
    if (page < 1 || page > onlineLastPage_ || page == onlinePage_) return;
    onlinePage_ = page;
    onlineSearch();
}

void Bridge::onlineSetOption(const QString& key, const QString& value) {
    OnlineSource* s = online_.source(onlineSource_);
    if (!s || !s->setOption(key.toStdString(), value.toStdString())) return;
    // A changed filter is a different search, not a re-sort of what is already on the screen - so the
    // page goes back to the first page of the new one, which is what the site's own paging means.
    setOnlineSource(onlineSource_);
    onlineSearch();
}

QString Bridge::onlineSize(int bytes) const {
    if (bytes <= 0) return {};
    const double mb = bytes / 1048576.0;
    // The unit is in the string because the card says nothing else about what the number measures.
    return QString::number(mb, 'f', mb >= 10 ? 0 : 1) + QStringLiteral(" MB");
}

QString Bridge::libraryIdFor(const QString& origin) const {
    return origin.isEmpty() ? QString() : originIds_.value(origin);
}

void Bridge::markOnlineRow(int index, const QString& state, const QString& detail) {
    if (index < 0 || index >= onlineRows_.size()) return;
    QVariantMap row = onlineRows_[index].toMap();
    row.insert("state", state);
    row.insert("detail", detail);
    onlineRows_[index] = row;
    emit onlineChanged();
}

QString Bridge::onlineCacheDir() const {
    // Next to the engine's own thumb cache, inside the tree the renderer reads: addimage copies the
    // file into the library, so this is scratch space and never the store.
    const QString base = root_.isEmpty() ? QCoreApplication::applicationDirPath() : root_;
    const QString dir = QDir(base).absoluteFilePath(QStringLiteral("cache/online"));
    QDir().mkpath(dir);
    return dir;
}

QString Bridge::onlineDest(const QVariantMap& row, const QString& url, const QString& tag) {
    // The extension comes from the url actually being fetched, so a card whose clip and whose preview
    // frame are both wanted keeps two files (`-10319.jpg` and `-10319.mp4`) instead of writing one
    // over the other.
    OnlineSource* s = online_.source(onlineSource_);
    const QString ext = QString::fromStdString(
        s ? s->ExtensionFor(url.toStdString()) : std::string());
    return QDir(onlineCacheDir())
        .absoluteFilePath(onlineSource_ + QLatin1Char('-') + row.value("key").toString() + tag
                          + QLatin1Char('.') + ext);
}

namespace {
// The site answers 429 to a run of originals, so a failed one gets two more rounds and each waits
// twice as long: 6 s, then 12 s. More than that is the site saying no for this visit.
constexpr int kHeroRetries = 2;
constexpr qint64 kHeroBackoffMs = 3000;
// A clip that gave up stays given up for the rest of the visit unless he asks again: 10 MB of a clip
// that will not arrive is not worth re-fetching every time the hero drifts back over it.
constexpr qint64 kClipGiveUpMs = 600000;
} // namespace

void Bridge::onlineHeroPreview(int index) {
    // The key travels with the file: the hero keeps moving on its own, and a binding that only looked
    // at `onlineHeroFile` would paint the previous card's full-size picture under the new card's
    // caption. QML compares the two and falls back to the site's preview when they differ.
    const QString key = (index >= 0 && index < onlineRows_.size())
                            ? onlineRows_[index].toMap().value("key").toString() : QString();
    const bool sameKey = (key == heroKey_);
    heroKey_ = key;
    // Re-asking for the card that is already up must not tear its picture down. `hero` is reassigned
    // with the *same* id about once a second, when Bridge::tick re-reads the renderer's status (the QML
    // note above `liveGraceMs` documents that), and this function used to clear `heroFile_`
    // unconditionally: the Image's `source` went to "" and Qt drops the frame an asynchronous Image is
    // holding when its source changes, so the sharp original vanished, the on-disk fast path below
    // handed the very same URL back a moment later, and the 1-4 MB jpeg decoded again. Measured
    // 2026-10-13 sitting on one card without touching anything: that layer went Ready -> Null -> Ready
    // at t+15.0 s and again at t+25.4 s, each time falling back to the site's 400x225 preview for
    // ~400 ms. That is the "在线壁纸还在动" he still reports - a sharp/soft pump on a timer, not a
    // movement, and it is why it looked per-card: it only happens once the file is on disk.
    if (sameKey && !heroFile_.isEmpty()) return;
    heroFile_.clear();
    heroError_.clear();
    heroTries_ = 0;
    emit onlineHeroFileChanged();
    if (key.isEmpty()) { setHeroState(QString()); return; }
    // The clip is asked for on the same schedule as the still: this is the one place that knows a card
    // became the picture he is looking at. Order matters only in the queue - the still is 405 KB and
    // the clip is 10 MB, and the sharp frame of it is worth more on screen than its motion.
    requestHero(key);
    requestClip(key);
}

QString Bridge::onlineSourceTemplate() const {
    // 一份能直接改的骨架。name 故意留空：LoadAll 会以 "no 'name'" 拒掉它，所以原样按下保存
    // 一定失败并说出原因——一个填了个标题就能出现的空源比报错更难解释。
    return QStringLiteral(R"JSON({
  "_怎么填": "把下面每一项改成你的站的信息，name 留空是存不进去的。带花括号的地址里 {query} 是搜索词、{page} 是第一页之外的页码、{选项key} 是你在 options 里写的每一项。",
  "name": "",
  "label": "显示在药丸上的名字",
  "homepage": "https://example.com",
  "url": "https://example.com/list/{page}",
  "ask": false,
  "user_agent": "Mozilla/5.0",
  "min_interval_ms": 3000,
  "_record": "抓网页时用：一条正则，每个字段用 $1 $2 这样引用它抓到的组。整个正则会被反复匹配，一次匹配=一张卡片。",
  "record": "<a[^>]+href=\"(/wall/[^\"]+)-(\\d+)\\.html\"[^>]*title=\"([^\"]*)\"[^>]*><img[^>]+src=\"([^\"]+)\"",
  "_fields": "JSON 应答的站把 record 删掉、改用 fields 的点号路径；网页站里 $n 表示第 n 个捕获组，其余是模板。",
  "fields": {
    "key": "$2",
    "title": "$3",
    "page": "https://example.com$1.html",
    "thumb": "$4",
    "image": "https://example.com/images/{slug}-{resolution}-{key}.jpg"
  },
  "_slug": "image 里 {slug} 从哪来：slug_from 指到某个字段，slug_basename 取它的文件名，slug_cut 在第一个 '-' 处截断。",
  "slug_from": "$1",
  "slug_basename": false,
  "slug_cut": true,
  "size": "",
  "last_page": "page=(\\d+)",
  "ext": "jpg",
  "options": {
    "category": {
      "values": ["nature", "space"],
      "default": "nature",
      "labels": { "nature": "自然", "space": "宇宙" }
    },
    "resolution": { "values": ["1920x1080", "3840x2160"], "default": "3840x2160" }
  }
})JSON");
}

QString Bridge::sourcesDir() const {
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("sources"));
}

void Bridge::reloadSources() {
    const QString keep = onlineSource_;
    loadOnlineSources();
    for (const auto& row : onlineSourceItems_)
        if (row.toMap().value("value").toString() == keep) { setOnlineSource(keep); return; }
}

QString Bridge::onlineAddSource(const QString& text) {
    std::string err;
    const auto parsed = Json::Parse(text.toStdString(), err);
    if (!parsed) return t("online_bad_json") + QStringLiteral(": ") + QString::fromStdString(err);
    if (!parsed->isObject()) return t("online_not_object");
    const QString name = QString::fromStdString(parsed->strOr("name"));
    // 文件名从这里派生，所以它不能带路径分隔符或 ..；界面上填的 name 也就是缓存前缀。
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!safe.match(name).hasMatch()) return t("online_bad_name") + QStringLiteral(": ") + name;
    const QString home = QString::fromStdString(parsed->strOr("homepage"));
    if (!home.startsWith(QLatin1String("http://")) && !home.startsWith(QLatin1String("https://")))
        return t("online_bad_homepage") + QStringLiteral(": ") + home;

    const QString path = QDir(sourcesDir()).absoluteFilePath(name + QStringLiteral(".json"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return t("online_write_failed") + QStringLiteral(": ") + f.errorString();
    f.write(text.toUtf8());
    f.close();

    reloadSources();
    for (const auto& row : onlineSourceItems_)
        if (row.toMap().value("value").toString() == name) return QString();

    // 没通过加载就被删回去：目录里留一份坏描述，下次启动只会再多一行 skipped 警告。
    QStringList why;
    std::vector<std::string> skipped;
    OnlineSource::LoadAll(sourcesDir().toStdWString(), skipped);
    for (const auto& s : skipped)
        if (QString::fromStdString(s).startsWith(name + QLatin1Char('.'))) why << QString::fromStdString(s);
    QFile::remove(path);
    reloadSources();
    return why.isEmpty() ? t("online_load_failed") : why.join(QStringLiteral("; "));
}

bool Bridge::onlineRemoveSource(const QString& name) {
    QDir d(sourcesDir());
    bool removed = false;
    for (const QFileInfo& e : d.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(e.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray body = f.readAll();
        f.close();
        std::string err;
        const auto j = Json::Parse(body.toStdString(), err);
        if (j && QString::fromStdString(j->strOr("name")) == name) { removed = QFile::remove(e.absoluteFilePath()); }
    }
    if (removed) reloadSources();
    return removed;
}

void Bridge::onlineHeroRetry() {
    if (heroKey_.isEmpty()) return;
    heroTries_ = 0;
    heroStartMs_ = 0;      // he asked for it; the interval between two cards does not apply to a retry
    requestHero(heroKey_);
}

void Bridge::onlineClipRetry() {
    if (heroKey_.isEmpty()) return;
    clipTries_ = 0;
    clipBlockedKey_.clear();
    clipBlockedUntilMs_ = 0;
    clipStartMs_ = 0;      // same rule as the still above
    requestClip(heroKey_);
}

void Bridge::requestHero(const QString& key) {
    // By key, not by the index the caller had: a page landing in the meantime moves the card.
    int index = -1;
    for (int i = 0; i < onlineRows_.size(); ++i)
        if (onlineRows_[i].toMap().value("key").toString() == key) { index = i; break; }
    if (index < 0) { setHeroState(QString()); return; }   // this list no longer holds that card
    const QVariantMap row = onlineRows_[index].toMap();
    const QString image = row.value("image").toString();
    // A video's "original" is the whole clip, and a hero is only ever looked at, so pulling tens of
    // megabytes for it buys nothing. A source that can name a cheap full-size frame of the clip gets
    // that fetched instead; one that cannot stays with pictures, as it did before clips came here.
    const QString heroUrl = row.value("hero").toString();
    const QString fetch = heroUrl.isEmpty() ? image : heroUrl;
    const QString mime = row.value("mime").toString();
    if (fetch.isEmpty() || (heroUrl.isEmpty() && !mime.startsWith(QLatin1String("image/")))) {
        setHeroState("none"); return;
    }
    if (row.value("state").toString() == QLatin1String("unsupported")) { setHeroState("none"); return; }
    if (!OnlineFetcher::sameHost(fetch, onlineSite_)) { setHeroState("none"); return; }

    const QString dest = onlineDest(row, fetch);
    if (QFileInfo::exists(dest) && QFileInfo(dest).size() > 0) {
        heroFile_ = QUrl::fromLocalFile(dest).toString();
        emit onlineHeroFileChanged();
        setHeroState("ready");
        return;
    }
    if (heroInFlight_ == key) return;   // this one is already on its way
    OnlineSource* s = online_.source(onlineSource_);
    if (!s) return;
    // The site's own interval covers originals too, not just the listing: one of these is 1-4 MB, and
    // clicking through five cards in ten seconds is what came back as 429 forty times on 2026-10-12.
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - heroStartMs_;
    if (heroStartMs_ != 0 && since < s->minIntervalMs()) {
        setHeroState("queued");
        QTimer::singleShot(int(s->minIntervalMs() - since), this, [this, key] {
            if (key == heroKey_) requestHero(key);
        });
        return;
    }
    heroInFlight_ = key;
    heroStartMs_ = QDateTime::currentMSecsSinceEpoch();
    // Which card, which attempt. A page of 24 that all asked for a picture without being touched is
    // the failure the log line exists to catch.
    qWarning("online: hero asks key=%s (try %d of %d)", key.toUtf8().constData(), heroTries_ + 1,
             kHeroRetries + 1);
    setHeroState("fetching");
    // Deliberately not through setPending: that one belongs to the press on 入库, and its progress
    // would otherwise start on a card he only looked at.
    online_.download(*s, fetch, dest, [this, key, dest](const QString& error, const QString& type) {
        if (heroInFlight_ == key) heroInFlight_.clear();
        if (!error.isEmpty()) {
            // The file stays on disk whichever way this went - 入库 would have wanted it too. The whole
            // sentence goes to the log; the screen gets the code and the fact that it is retrying,
            // because "why is this one still soft" has to be answerable without opening a file.
            qWarning("online: hero preview %s: %s", key.toUtf8().constData(), error.toUtf8().constData());
            if (key != heroKey_) return;      // he moved on; nothing retries for a card nobody is looking at
            heroError_ = heroErrorOf(error);
            if (heroTries_ < kHeroRetries) {
                ++heroTries_;
                setHeroState("retrying");
                QTimer::singleShot(int(kHeroBackoffMs << heroTries_), this, [this, key] {
                    if (key == heroKey_) requestHero(key);
                });
            } else {
                setHeroState("failed");
            }
            emit onlineHeroStateChanged();    // the error text changed with the state's payload
            return;
        }
        const QString real = sniffMedia(dest);
        if (!real.startsWith(QLatin1String("image/"))) {
            qWarning("online: hero preview %s is %s (%s), not a picture", key.toUtf8().constData(),
                     real.toUtf8().constData(), type.toUtf8().constData());
            if (key == heroKey_) { heroError_ = real; setHeroState("failed"); }
            return;
        }
        if (heroKey_ != key) return;   // he moved on; the next look finds it already there
        heroFile_ = QUrl::fromLocalFile(dest).toString();
        emit onlineHeroFileChanged();
        setHeroState("ready");
    });
}

void Bridge::setHeroState(const QString& state) {
    if (heroState_ == state) return;
    heroState_ = state;
    emit onlineHeroStateChanged();
}

// Which size of this site's own list to preview at. A site that publishes no resolution list gets an
// empty answer and its cards stay still frames: inventing a size here is how a preview turns into a
// 404 that can only be seen as "它不动".
QString Bridge::clipRung() {
    return onlineRungs_.isEmpty() ? QString() : previewRung_;
}

// The big picture over an online clip plays the clip rather than a frame of it. Three steps: fetch the
// video at the rung 预览画质 picks, hand it to the renderer as a throwaway package (`onlineclip`,
// which registers it and starts the second pass in one command), then let the hero read the frames
// that pass already ships for imported videos.
//
// The download is deliberately not 入库's: a card he only looked at must not end up in the library,
// and the library copy is fetched at the *import* resolution, which is usually not this one. When the
// two agree the same file is asked for, and the on-disk check below makes the second one free.
void Bridge::setOnlineClipWanted(bool on) {
    if (clipWanted_ == on) return;
    clipWanted_ = on;
    emit onlineClipWantedChanged();
    // Coming back on is the moment the rotation stopped, and the card under the big picture is the one
    // he chose to look at - ask for it now rather than waiting for the next hero change.
    if (on && !heroKey_.isEmpty()) requestClip(heroKey_);
}

void Bridge::requestClip(const QString& key) {
    if (key.isEmpty() || !clipWanted_) return;
    OnlineSource* s = online_.source(onlineSource_);
    if (!s) return;
    int index = -1;
    for (int i = 0; i < onlineRows_.size(); ++i)
        if (onlineRows_[i].toMap().value("key").toString() == key) { index = i; break; }
    if (index < 0) return;                       // this list no longer holds that card
    const QVariantMap row = onlineRows_[index].toMap();
    if (!row.value("mime").toString().startsWith(QLatin1String("video/"))) return;
    if (row.value("state").toString() == QLatin1String("unsupported")) return;
    const QString image = row.value("image").toString();
    if (image.isEmpty() || !OnlineFetcher::sameHost(image, onlineSite_)) return;
    if (clipKey_ == key && !clipId_.isEmpty()) return;   // this one is already playing
    if (clipInFlight_ == key) return;                     // and this one is already on its way
    // Awaiting a backoff, or already given up, is not a moment to start again: this function is also
    // called once a second by the hero being reassigned the same card, and without this the retry
    // timer's 6 s and 12 s would be walked straight over. Blocked by key, because moving to another
    // card has to be able to ask immediately - and only his press on 重试 lifts a giving-up.
    if (clipBlockedKey_ == key && QDateTime::currentMSecsSinceEpoch() < clipBlockedUntilMs_) return;
    const QString rung = clipRung();
    if (rung.isEmpty()) { clearClip(); return; }          // the site names no rungs to choose from

    const QString cur = QString::fromStdString(s->optionValue("resolution"));
    QString url = image;
    QString tag;
    // The rung is one token in the url the site itself printed, so the request for another one is that
    // token changed - and the file is named after it, which is what lets the 540p preview and the 4K
    // import of the same card sit beside each other instead of over each other.
    if (!cur.isEmpty() && rung != cur) { url.replace(cur, rung); tag = QLatin1String("-") + rung; }
    if (!OnlineFetcher::sameHost(url, onlineSite_)) return;
    const QString dest = onlineDest(row, url, tag);
    if (QFileInfo::exists(dest) && QFileInfo(dest).size() > 0) { clipReady(key, dest); return; }

    // One interval for both halves of a card: the site counts its per-minute budget over everything
    // this host gets, and a card now asks for a frame and a clip.
    const qint64 last = std::max(heroStartMs_, clipStartMs_);
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - last;
    if (last != 0 && since < s->minIntervalMs()) {
        setClipState(QStringLiteral("queued"));
        QTimer::singleShot(int(s->minIntervalMs() - since), this, [this, key] {
            if (key == heroKey_) requestClip(key);
        });
        return;
    }
    clipInFlight_ = key;
    clipStartMs_ = QDateTime::currentMSecsSinceEpoch();
    clipUrl_ = url;
    clipProgress_ = 0.0;
    clipBytes_ = 0; clipTotal_ = 0; clipKbps_ = 0.0;
    clipMeterAtMs_ = 0; clipMeterBytes_ = 0;
    qWarning("online: clip asks key=%s row=%s want=%s file=%s (try %d of %d)", key.toUtf8().constData(),
             cur.toUtf8().constData(), rung.toUtf8().constData(),
             QFileInfo(dest).fileName().toUtf8().constData(), clipTries_ + 1, kHeroRetries + 1);
    setClipState(QStringLiteral("fetching"));
    online_.download(*s, url, dest, [this, key, url, dest](const QString& error, const QString& type) {
        if (clipInFlight_ == key) clipInFlight_.clear();
        if (clipUrl_ == url) clipUrl_.clear();   // this answer is for the bar's owner, whoever it was
        if (!error.isEmpty()) {
            qWarning("online: clip %s: %s", key.toUtf8().constData(), error.toUtf8().constData());
            if (key != heroKey_) return;      // he moved on; nothing retries for a card nobody is on
            if (clipTries_ < kHeroRetries) {
                ++clipTries_;
                const qint64 wait = kHeroBackoffMs << clipTries_;
                clipBlockedKey_ = key;
                clipBlockedUntilMs_ = QDateTime::currentMSecsSinceEpoch() + wait;
                setClipState(QStringLiteral("retrying"));
                QTimer::singleShot(int(wait), this, [this, key] {
                    if (key == heroKey_) requestClip(key);
                });
            } else {
                clipBlockedKey_ = key;
                clipBlockedUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kClipGiveUpMs;
                setClipState(QStringLiteral("failed"));
            }
            return;
        }
        // 4kwallpapers answers with the wrong Content-Type, so the bytes decide: an html error page
        // saved under a clip's name would otherwise be handed to the decoder as if it were a video.
        const QString real = sniffMedia(dest);
        if (!real.startsWith(QLatin1String("video/"))) {
            qWarning("online: clip %s is %s (%s), not a video", key.toUtf8().constData(),
                     real.toUtf8().constData(), type.toUtf8().constData());
            if (key == heroKey_) {
                clipBlockedKey_ = key;
                clipBlockedUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kClipGiveUpMs;
                setClipState(QStringLiteral("failed"));
            }
            return;
        }
        if (clipTries_ != 0) setClipState(QStringLiteral("fetching"));
        clipTries_ = 0;
        if (heroKey_ != key) return;   // he moved on; the next look finds the file already there
        clipReady(key, dest);
    });
}

void Bridge::clipReady(const QString& key, const QString& path) {
    clipKey_ = key;
    clipPath_ = path;
    clipTries_ = 0;
    clipBlockedKey_.clear();
    clipBlockedUntilMs_ = 0;
    // The renderer derives the id from the file name, and the file name is the card's key plus the
    // rung, so this window can say which clip it is asking for without a second round trip.
    clipId_ = QStringLiteral("pv:") + QFileInfo(path).fileName();
    setClipState(QStringLiteral("ready"));
    emit onlineClipChanged();
}

void Bridge::clearClip() {
    if (clipKey_.isEmpty() && clipId_.isEmpty() && clipState_.isEmpty()) return;
    clipKey_.clear();
    clipId_.clear();
    clipPath_.clear();
    clipState_.clear();
    clipTries_ = 0;
    clipUrl_.clear();
    clipProgress_ = 0.0;
    emit onlineClipChanged();
}

void Bridge::setClipState(const QString& state) {
    if (clipState_ == state) return;
    clipState_ = state;
    emit onlineClipChanged();
}

QString Bridge::heroErrorOf(const QString& error) {
    // Only the number goes on screen: it is language-neutral, and Qt's own sentences ("the site sent
    // an empty body") would put English inside a Chinese page. The full sentence is in the log.
    if (const int at = error.indexOf(QStringLiteral("status code")); at >= 0)
        return error.mid(at + 11).trimmed().left(3);
    return QString();
}

void Bridge::onlineImport(int index) {
    if (index < 0 || index >= onlineRows_.size()) return;
    const QVariantMap row = onlineRows_[index].toMap();
    const QString page = row.value("page").toString();
    if (const QString have = libraryIdFor(page); !have.isEmpty()) {
        markOnlineRow(index, QStringLiteral("imported"), have);
        say(t("online_already") + QStringLiteral(": ") + have, false);
        return;
    }
    // The library is the renderer's, so this is the one online action that cannot be done offline.
    if (!connected_) { say(t("online_need_renderer"), true); return; }
    OnlineSource* s = online_.source(onlineSource_);
    const QString image = row.value("image").toString();
    if (!s || image.isEmpty()) return;
    // The card already says why; a press on it does nothing.
    if (row.value("state").toString() == QLatin1String("unsupported")) return;
    if (!OnlineFetcher::sameHost(image, onlineSite_)) {
        say(t("online_offsite") + QStringLiteral(": ") + image, true);
        return;
    }
    const QString dest = onlineDest(row, image);
    const QString key = row.value("key").toString();
    setPending(key, QStringLiteral("downloading"), QString());
    // The key travels with the callback: a new search can replace the grid while this is in flight,
    // and writing to whatever sits at the old index would land a picture's result on another card.
    if (QFileInfo::exists(dest) && QFileInfo(dest).size() > 0) {
        importDownloaded(index, key, dest, page);
        return;
    }
    online_.download(*s, image, dest, [this, index, key, dest, page](const QString& error, const QString& type) {
        if (key != pendingKey_) return;   // another card was asked for in the meantime
        if (!error.isEmpty()) {
            setPending(key, QStringLiteral("failed"), error);
            say(t("online_download_failed") + QStringLiteral(": ") + error, true);
            return;
        }
        const QString real = sniffMedia(dest);
        if (!real.startsWith(QLatin1String("image/")) && !real.startsWith(QLatin1String("video/"))) {
            // Both names, because on at least one site they disagree, and the card has to say
            // something true about why this file will not enter the library.
            const QString why = t("online_not_image") + QStringLiteral(": ") + real
                              + QStringLiteral(" (")
                              + (type.isEmpty() ? QStringLiteral("no type given") : type) + QLatin1Char(')');
            QFile::remove(dest);
            setPending(key, QStringLiteral("failed"), why);
            say(why, true);
            return;
        }
        importDownloaded(index, key, dest, page);
    });
}

void Bridge::setPending(const QString& key, const QString& state, const QString& detail) {
    pendingKey_ = key;
    pendingState_ = state;
    pendingDetail_ = detail;
    if (state != QLatin1String("downloading")) progress_ = 0.0;
    emit onlineProgressChanged();
}

void Bridge::importDownloaded(int index, const QString& key, const QString& path, const QString& page) {
    const bool video = kVideoExt.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
    // Built with the JSON writer rather than a format string: the file path and the site's own url are
    // data, and `Json::Parse` refuses a bare backslash as an unknown escape - so a path that came from
    // a dialog has to be escaped, not pasted between quotes.
    Json cmd = Json::Object();
    cmd.set("cmd", Json::Of(video ? "addvideo" : "addimage"));
    cmd.set("arg", Json::Of(QDir::fromNativeSeparators(path).toStdString()));
    cmd.set("arg2", Json::Of(page.toStdString()));
    Json out;
    if (!request(cmd.dump().c_str(), 8000, &out) || !out.boolOr("ok")) {
        const QString why = QString::fromStdString(out.strOr("error"));
        setPending(key, QStringLiteral("failed"), why.isEmpty() ? t("failed") : why);
        say(t("failed") + QStringLiteral(": ") + why, true);
        return;
    }
    setPending(key, QStringLiteral("importing"), QString());
    importRow_ = index;
    importOrigin_ = page;
    importAtMs_ = QDateTime::currentMSecsSinceEpoch();
    refresh();
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
    // No visibility gate here. There was one until 2026-10-08 ("a window sitting in the tray must not
    // keep the renderer drawing a second pass nobody can see"), but stopping the pass is what made
    // coming back show a still for the ~0.7 s the renderer needs to resume, and he asked for the
    // motion to keep playing through a minimise or a trip to the tray instead. The renderer still
    // ends the pass on its own if this window stops beating, so a crashed window cannot leave it
    // burning; `useLivePreview` in the QML is the switch that means it now.
    if (previewId_ != id) {
        previewId_ = id;
        // The clip the page moved to is a fresh player and is not paused, and the state only comes
        // back on the next snapshot - a second later. Clearing it here is what keeps the button from
        // showing 已暂停 over a picture that is already running.
        if (previewPaused_) { previewPaused_ = false; emit previewVideoPausedChanged(); }
        previewBeatMs_ = 0;   // a new wallpaper asks for a beat straight away
        // A `pv:` id is not in the library, so `preview` could not find it: the engine has to be told
        // which file to register as a throwaway package first, and `onlineclip` does that and starts
        // the pass in one command. This is why the id travels as the file's name and not as a row index
        // - the same clip asked for twice must land on the same id, or the pass restarts and the
        // picture jumps back to frame zero.
        const std::string body = id.startsWith(QLatin1String("pv:"))
            ? std::format(R"({{"cmd":"onlineclip","arg":"{}"}})", clipPath_.toStdString())
            : std::format(R"({{"cmd":"preview","arg":"{}"}})", id.toStdString());
        Json out;
        request(body.c_str(), 2000, &out);
    }
    if (!previewTimer_->isActive()) previewTimer_->start(33);
}

void Bridge::hidePreview() {
    // The clip belongs to the pass, not to this window's memory: once nothing is drawing it, the id it
    // was known by has to go, or the hero keeps asking for a feed the renderer no longer resolves.
    // Only that triple - the download's own story stays, because this function is what the QML gate
    // calls while a clip is still on its way (nothing is being fed yet), and wiping the state here is
    // what made 取视频中 and its bar vanish a moment after they appeared.
    // Announced at the very end: emitting first would run this function again from the QML gate while
    // the beat timer was still going, and the outer call would then skip the teardown.
    const bool told = !clipKey_.isEmpty() || !clipId_.isEmpty();
    clipKey_.clear(); clipId_.clear(); clipPath_.clear();
    if (!previewTimer_->isActive()) {
        if (told) emit onlineClipChanged();
        return;
    }
    previewTimer_->stop();
    request(R"({"cmd":"previewoff"})", 1000, nullptr);
    previewId_.clear();
    if (told) emit onlineClipChanged();
}

void Bridge::previewBeat() {
    // The beat's whole job is to tell a live renderer that the second pass is still being watched.
    // With no renderer there is nobody to tell, and asking costs this thread - the GUI thread - the
    // call's entire 500 ms, once every 700. That is the "退出渲染器之后整个客户端都卡" he reported:
    // the poll had already backed itself off to two seconds, the heartbeat had not.
    if (!connected_) {
        if (previewTimer_->isActive()) previewTimer_->stop();
        return;
    }
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
    // His choice, not a fault: the watchdog above must not put the engine back up.
    quitAsked_ = true;
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
