#pragma once
// Wallpaper/Bridge.hpp - the settings window's view of the renderer, over the control pipe.
#include "Engine/Core/Json.hpp"
#include "Online/OnlineFetcher.hpp"

#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>

namespace sw {

// Pipe calls are synchronous and short: 300 ms while the renderer is known to be up, 1500 ms for a
// command. A settings window blocking a few hundred ms once a second is cheaper than a worker
// thread plus the state marshalling that would come with it.
class Bridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString lang READ lang WRITE setLang NOTIFY langChanged)
    Q_PROPERTY(QStringList languages READ languages NOTIFY langChanged)
    Q_PROPERTY(QVariantList monitors READ monitors NOTIFY stateChanged)
    Q_PROPERTY(QVariantList catalog READ catalog NOTIFY stateChanged)
    Q_PROPERTY(QVariantList powerCaps READ powerCaps NOTIFY stateChanged)
    Q_PROPERTY(QStringList qualityLevels READ qualityLevels CONSTANT)
    Q_PROPERTY(QString quality READ quality NOTIFY stateChanged)
    Q_PROPERTY(bool qualityIsGlobal READ qualityIsGlobal NOTIFY stateChanged)
    Q_PROPERTY(QStringList fitLevels READ fitLevels CONSTANT)
    Q_PROPERTY(QString imageFit READ imageFit NOTIFY stateChanged)
    // How the big preview changes when a different wallpaper is picked. The window draws this
    // itself, so it is a ui.ini preference like the language, not a renderer setting.
    Q_PROPERTY(QStringList transitionLevels READ transitionLevels CONSTANT)
    Q_PROPERTY(QString transition READ transition WRITE setTransition NOTIFY transitionChanged)
    Q_PROPERTY(int trayAlpha READ trayAlpha NOTIFY stateChanged)
    // The engine's own status card. `hudReady` is reported apart from `hud` because a card the user
    // switched on that the renderer could not prepare would otherwise read as working, with nothing
    // on the screen and no reason why.
    Q_PROPERTY(bool hud READ hud NOTIFY stateChanged)
    Q_PROPERTY(bool hudReady READ hudReady NOTIFY stateChanged)
    Q_PROPERTY(QString hudError READ hudError NOTIFY stateChanged)
    Q_PROPERTY(QStringList hudCornerLevels READ hudCornerLevels CONSTANT)
    Q_PROPERTY(QString hudCorner READ hudCorner NOTIFY stateChanged)
    Q_PROPERTY(int hudScale READ hudScale NOTIFY stateChanged)
    Q_PROPERTY(QStringList hudFrameLevels READ hudFrameLevels CONSTANT)
    Q_PROPERTY(QString hudFrame READ hudFrame NOTIFY stateChanged)
    // Whether the desktop's drag layer is standing over the card. Optimistic on the way in, because
    // the pipe only answers "queued"; the snapshot reads the truth back, which is what snaps the
    // switch off again when the card is dropped.
    Q_PROPERTY(bool hudDrag READ hudDrag NOTIFY stateChanged)
    // True once the card has been dragged somewhere, which is what makes the position row say
    // 「自定义」 instead of naming a corner.
    Q_PROPERTY(bool hudCustom READ hudCustom NOTIFY stateChanged)
    Q_PROPERTY(bool orbit READ orbit NOTIFY stateChanged)
    Q_PROPERTY(bool orbitIcons READ orbitIcons NOTIFY stateChanged)
    Q_PROPERTY(bool orbitReady READ orbitReady NOTIFY stateChanged)
    Q_PROPERTY(bool orbitClickable READ orbitClickable NOTIFY stateChanged)
    Q_PROPERTY(int orbitPeriod READ orbitPeriod NOTIFY stateChanged)
    // Held still by the tray rather than by 转一圈=0: the period survives it.
    Q_PROPERTY(bool orbitHeld READ orbitHeld NOTIFY stateChanged)
    Q_PROPERTY(int orbitPerRing READ orbitPerRing NOTIFY stateChanged)
    Q_PROPERTY(int orbitRings READ orbitRings NOTIFY stateChanged)
    Q_PROPERTY(int orbitShown READ orbitShown NOTIFY stateChanged)
    Q_PROPERTY(QString orbitShape READ orbitShape NOTIFY stateChanged)
    Q_PROPERTY(QStringList orbitShapeLevels READ orbitShapeLevels CONSTANT)
    Q_PROPERTY(int orbitSlots READ orbitSlots NOTIFY stateChanged)
    // The ring's two sizes, as percentages of the layout it was measured at - the same convention the
    // card's and chart's scale properties are on, so a slider can show them without a conversion.
    Q_PROPERTY(int orbitScale READ orbitScale NOTIFY stateChanged)
    Q_PROPERTY(int orbitIconScale READ orbitIconScale NOTIFY stateChanged)
    // The picker's data: the desktop's own shortcuts, each with whether the ring is showing it, plus
    // how many slots the current selection can fill and whether a selection is in force at all.
    Q_PROPERTY(QVariantList orbitIconList READ orbitIconList NOTIFY stateChanged)
    Q_PROPERTY(int orbitPickable READ orbitPickable NOTIFY stateChanged)
    Q_PROPERTY(bool orbitPicked READ orbitPicked NOTIFY stateChanged)
    Q_PROPERTY(QString orbitError READ orbitError NOTIFY stateChanged)
    Q_PROPERTY(bool iconsHidden READ iconsHidden NOTIFY stateChanged)
    Q_PROPERTY(bool orbitDrag READ orbitDrag NOTIFY stateChanged)
    Q_PROPERTY(bool orbitCustom READ orbitCustom NOTIFY stateChanged)
    // The chart panel. Same shape as the card's, plus the history depth: a line that can only show
    // four seconds of sixteen is a different picture from one that is full, and the page says so
    // rather than letting a mostly-empty gauge look broken.
    Q_PROPERTY(bool chart READ chart NOTIFY stateChanged)
    Q_PROPERTY(bool chartReady READ chartReady NOTIFY stateChanged)
    Q_PROPERTY(QStringList chartTypeLevels READ chartTypeLevels CONSTANT)
    Q_PROPERTY(QString chartType READ chartType NOTIFY stateChanged)
    Q_PROPERTY(QStringList chartCornerLevels READ chartCornerLevels CONSTANT)
    Q_PROPERTY(QString chartCorner READ chartCorner NOTIFY stateChanged)
    Q_PROPERTY(int chartScale READ chartScale NOTIFY stateChanged)
    Q_PROPERTY(QString chartFrame READ chartFrame NOTIFY stateChanged)
    Q_PROPERTY(bool chartCustom READ chartCustom NOTIFY stateChanged)
    Q_PROPERTY(bool chartDrag READ chartDrag NOTIFY stateChanged)
    Q_PROPERTY(int chartHistory READ chartHistory NOTIFY stateChanged)
    Q_PROPERTY(int maxFps READ maxFps NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    // Whether the wallpaper on the desktop is a still. The tray's 暂停壁纸 row is grey for those:
    // there is nothing playing there to stop, and a tick that changes nothing is a dead control.
    Q_PROPERTY(bool showingDynamic READ showingDynamic NOTIFY stateChanged)
    // Whether the clip in the big preview is frozen. This is the browse page's own transport and is
    // scoped to the preview player, so it is deliberately not 暂停渲染 (which stops the drawing).
    Q_PROPERTY(bool previewVideoPaused READ previewVideoPaused WRITE setPreviewVideoPaused
               NOTIFY previewVideoPausedChanged)
    Q_PROPERTY(QString hostMethod READ hostMethod NOTIFY stateChanged)
    Q_PROPERTY(QString rootDir READ rootDir NOTIFY stateChanged)
    Q_PROPERTY(QString logPath READ logPath NOTIFY stateChanged)
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY stateChanged)
    // The card's busy percent, or -1 when this machine has nothing to ask it from (no NVML). The status
    // line leaves the whole segment out rather than showing a 0 that would read as an idle GPU.
    Q_PROPERTY(double gpuPercent READ gpuPercent NOTIFY stateChanged)
    // This process's own GPU share: the per-engine percentages added up, the way Task Manager's
    // per-process column does it. -1 when the counters have nothing to report.
    Q_PROPERTY(double gpuSelfPercent READ gpuSelfPercent NOTIFY stateChanged)
    Q_PROPERTY(double gpuSelf3d READ gpuSelf3d NOTIFY stateChanged)
    Q_PROPERTY(double gpuSelfDecode READ gpuSelfDecode NOTIFY stateChanged)
    Q_PROPERTY(double cpuWindowS READ cpuWindowS NOTIFY stateChanged)
    Q_PROPERTY(double workingSetMb READ workingSetMb NOTIFY stateChanged)
    Q_PROPERTY(double uptimeSeconds READ uptimeSeconds NOTIFY stateChanged)
    Q_PROPERTY(bool autostart READ autostart NOTIFY stateChanged)
    Q_PROPERTY(bool rotateOn READ rotateOn NOTIFY stateChanged)
    Q_PROPERTY(int rotateIntervalMin READ rotateIntervalMin NOTIFY stateChanged)
    Q_PROPERTY(int rotateNextInS READ rotateNextInS NOTIFY stateChanged)
    Q_PROPERTY(QStringList favorites READ favorites NOTIFY favoritesChanged)
    Q_PROPERTY(QString selectAtStart READ selectAtStart CONSTANT)
    Q_PROPERTY(bool settingsAtStart READ settingsAtStart CONSTANT)
    Q_PROPERTY(QString settingsTabAtStart READ settingsTabAtStart CONSTANT)
    Q_PROPERTY(QString targetMonitor READ targetMonitor WRITE setTargetMonitor NOTIFY targetChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY messageChanged)
    Q_PROPERTY(bool lastMessageIsError READ lastMessageIsError NOTIFY messageChanged)

    // ------------------------------------------------------------------ 在线源
    // A wallpaper site described by a file in `sources`, not by code compiled in here. Every network
    // call is asynchronous on this thread's event loop: the settings window answering is the thing
    // the no-renderer stall was about, and a blocked GUI thread reads as the whole app being slow.
    // Importing is the only part that needs the renderer, because the renderer owns the library.
    Q_PROPERTY(QVariantList onlineSourceItems READ onlineSourceItems NOTIFY onlineChanged)
    Q_PROPERTY(QString onlineSource READ onlineSource WRITE setOnlineSource NOTIFY onlineChanged)
    Q_PROPERTY(QString onlineSite READ onlineSite NOTIFY onlineChanged)
    Q_PROPERTY(QString onlineQuery READ onlineQuery WRITE setOnlineQuery NOTIFY onlineChanged)
    Q_PROPERTY(QVariantList onlineOptions READ onlineOptions NOTIFY onlineChanged)
    Q_PROPERTY(QVariantList onlineRows READ onlineRows NOTIFY onlineChanged)
    Q_PROPERTY(bool onlineBusy READ onlineBusy NOTIFY onlineChanged)
    Q_PROPERTY(QString onlineError READ onlineError NOTIFY onlineChanged)
    Q_PROPERTY(int onlinePage READ onlinePage NOTIFY onlineChanged)
    Q_PROPERTY(int onlineLastPage READ onlineLastPage NOTIFY onlineChanged)
    Q_PROPERTY(qint64 onlineTotal READ onlineTotal NOTIFY onlineChanged)
    // False for a site whose pager only links the neighbouring page: the window then shows the page
    // it is on rather than a numbered window whose last cell would claim a total nobody published.
    Q_PROPERTY(bool onlinePagesKnown READ onlinePagesKnown NOTIFY onlineChanged)
    // The one import in flight, kept out of the row list on purpose. A progress tick written into a
    // row would change the model and rebuild all 24 tiles, and every one of them would re-decode its
    // picture - which reads as the grid blinking while a single card is being fetched.
    Q_PROPERTY(QString onlinePendingKey READ onlinePendingKey NOTIFY onlineProgressChanged)
    Q_PROPERTY(QString onlinePendingState READ onlinePendingState NOTIFY onlineProgressChanged)
    Q_PROPERTY(QString onlinePendingDetail READ onlinePendingDetail NOTIFY onlineProgressChanged)
    Q_PROPERTY(double onlineProgress READ onlineProgress NOTIFY onlineProgressChanged)
    // The shortest gap this site is asked twice for, straight out of its description: the page quotes
    // it next to the site's own published budget, so the number on screen is the one in force.
    Q_PROPERTY(int onlineIntervalMs READ onlineIntervalMs NOTIFY onlineChanged)
    // False when the site has no text query its own robots rules let a program use. The page hides the
    // search box on this rather than greying it: a dead box promises a thing that will never answer.
    Q_PROPERTY(bool onlineAsk READ onlineAsk NOTIFY onlineChanged)
    // The full-size file for the picture the hero is showing, as a url QML can point an Image at, and
    // empty while there is not one yet. Its own NOTIFY because it is not part of the row list: writing
    // it into a row would rebuild all 24 tiles for the one card that just got sharper.
    Q_PROPERTY(QString onlineHeroFile READ onlineHeroFile NOTIFY onlineHeroFileChanged)
    // Which card that file belongs to, so the hero can refuse to paint another card's picture.
    Q_PROPERTY(QString onlineHeroKey READ onlineHeroKey NOTIFY onlineHeroFileChanged)
    // And how that fetch is going: queued, fetching, retrying, ready, failed, none. A silent failure
    // is the expensive kind - the caption kept claiming 3840 x 2160 while the hero painted the site's
    // 1920 file, which is 0.36 of its high-pass energy at that box and reads as "还是没原图清楚".
    Q_PROPERTY(QString onlineHeroState READ onlineHeroState NOTIFY onlineHeroStateChanged)
    // Only the code (429, closed) - the whole sentence goes to the log.
    Q_PROPERTY(QString onlineHeroError READ onlineHeroError NOTIFY onlineHeroStateChanged)

public:
    explicit Bridge(QObject* parent = nullptr);
    ~Bridge() override;

    bool connected() const { return connected_; }
    QString lang() const { return lang_; }
    QStringList languages() const;
    QVariantList monitors() const { return monitors_; }
    QVariantList catalog() const { return catalog_; }
    QVariantList powerCaps() const { return caps_; }
    QStringList qualityLevels() const;
    QString quality() const { return quality_; }
    bool qualityIsGlobal() const { return qualityIsGlobal_; }
    QStringList fitLevels() const;
    // The hero switch transition the settings window draws for itself. Not a renderer setting:
    // the desktop wallpaper is not repainted when this changes, so it lives in the ui.ini next to
    // the language rather than in the renderer's config.json.
    QStringList transitionLevels() const;
    QString transition() const { return transition_; }
    Q_INVOKABLE void setTransition(const QString& mode);
    QString imageFit() const { return imageFit_; }
    bool hud() const { return hud_; }
    bool hudReady() const { return hudReady_; }
    QString hudError() const { return hudError_; }
    QStringList hudCornerLevels() const;
    QString hudCorner() const { return hudCorner_; }
    int hudScale() const { return hudScale_; }
    QStringList hudFrameLevels() const;
    QString hudFrame() const { return hudFrame_; }
    bool hudDrag() const { return hudDrag_; }
    bool hudCustom() const { return hudCustom_; }
    // The shortcut ring and the shell's icon layer. `orbitClickable` is its own read-out because the
    // ring can be drawn on every screen and still only be clickable on the one the layer was armed
    // for, and a switch that promises "click to open" has to say when it cannot.
    bool orbit() const { return orbit_; }
    bool orbitIcons() const { return orbitIcons_; }
    bool orbitReady() const { return orbitReady_; }
    bool orbitClickable() const { return orbitClickable_; }
    int orbitPeriod() const { return orbitPeriod_; }
    int orbitPerRing() const { return orbitPerRing_; }
    int orbitRings() const { return orbitRings_; }
    int orbitShown() const { return orbitShown_; }
    QString orbitShape() const { return orbitShape_; }
    QStringList orbitShapeLevels() const;
    int orbitSlots() const { return orbitSlots_; }
    int orbitScale() const { return orbitScale_; }
    int orbitIconScale() const { return orbitIconScale_; }
    QVariantList orbitIconList() const { return orbitIconList_; }
    int orbitPickable() const { return orbitPickable_; }
    bool orbitPicked() const { return orbitPicked_; }
    QString orbitError() const { return orbitError_; }
    bool iconsHidden() const { return iconsHidden_; }
    bool orbitDrag() const { return orbitDrag_; }
    bool orbitCustom() const { return orbitCustom_; }
    bool chart() const { return chart_; }
    bool chartReady() const { return chartReady_; }
    QStringList chartTypeLevels() const;
    QString chartType() const { return chartType_; }
    QStringList chartCornerLevels() const;
    QString chartCorner() const { return chartCorner_; }
    int chartScale() const { return chartScale_; }
    QString chartFrame() const { return chartFrame_; }
    bool chartCustom() const { return chartCustom_; }
    bool chartDrag() const { return chartDrag_; }
    int chartHistory() const { return chartHistory_; }
    int trayAlpha() const { return trayAlpha_; }
    int maxFps() const { return maxFps_; }
    bool paused() const { return paused_; }
    bool orbitHeld() const { return orbitHeld_; }
    bool showingDynamic() const;
    bool previewVideoPaused() const { return previewPaused_; }
    QString hostMethod() const { return host_; }
    QString rootDir() const { return root_; }
    QString logPath() const { return logPath_; }
    double cpuPercent() const { return cpu_; }
    double gpuPercent() const { return gpu_; }
    double gpuSelfPercent() const { return gpuSelf_; }
    double gpuSelf3d() const { return gpu3d_; }
    double gpuSelfDecode() const { return gpuDec_; }
    double cpuWindowS() const { return cpuWin_; }
    double workingSetMb() const { return ws_; }
    double uptimeSeconds() const { return uptime_; }
    bool autostart() const { return autostart_; }
    bool rotateOn() const { return rotateOn_; }
    int rotateIntervalMin() const { return rotateMin_; }
    int rotateNextInS() const { return rotateNext_; }
    QStringList favorites() const { return favorites_; }
    // What the screen the tray and the settings window act on is showing right now: the id the
    // rotation steps from, and the name the tray's tooltip and toast show.
    QString showingId() const { return showingId_; }
    QString showingName() const { return showingName_; }
    QString selectAtStart() const { return selectAtStart_; }
    bool settingsAtStart() const { return settingsAtStart_; }
    QString settingsTabAtStart() const { return settingsTabAtStart_; }
    QString targetMonitor() const { return target_; }
    QString lastMessage() const { return lastMessage_; }
    bool lastMessageIsError() const { return lastIsError_; }

    QVariantList onlineSourceItems() const { return onlineSourceItems_; }
    QString onlineSource() const { return onlineSource_; }
    QString onlineSite() const { return onlineSite_; }
    QString onlineQuery() const { return onlineQuery_; }
    QVariantList onlineOptions() const { return onlineOptions_; }
    QVariantList onlineRows() const { return onlineRows_; }
    bool onlineBusy() const { return onlineBusy_; }
    QString onlineError() const { return onlineError_; }
    int onlinePage() const { return onlinePage_; }
    int onlineLastPage() const { return onlineLastPage_; }
    qint64 onlineTotal() const { return onlineTotal_; }
    bool onlinePagesKnown() const { return onlinePagesKnown_; }
    int onlineIntervalMs() const { return onlineIntervalMs_; }
    bool onlineAsk() const { return onlineAsk_; }
    QString onlinePendingKey() const { return pendingKey_; }
    QString onlinePendingState() const { return pendingState_; }
    QString onlinePendingDetail() const { return pendingDetail_; }
    double onlineProgress() const { return progress_; }
    QString onlineHeroFile() const { return heroFile_; }
    QString onlineHeroKey() const { return heroKey_; }
    QString onlineHeroState() const { return heroState_; }
    QString onlineHeroError() const { return heroError_; }
    void setOnlineSource(const QString& name);
    void setOnlineQuery(const QString& q) { onlineQuery_ = q; }

    void setTargetMonitor(const QString& tag);
    void setLang(const QString& code);

    Q_INVOKABLE QString t(const QString& key) const;
    // The settings window draws its own title bar, so it is borderless; these two carry that over
    // to the platform: the HWND to act on, and ShowWindow commands (SW_MAXIMIZE/SW_RESTORE/...)
    // so the state change goes through Windows and keeps the native zoom animation.
    Q_INVOKABLE void adoptChromeWindow(qint64 hwnd);
    Q_INVOKABLE void showCommand(int cmd);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool apply(const QString& wallpaperId);
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void setOrbitHeld(bool held);
    Q_INVOKABLE void setPreviewVideoPaused(bool paused);
    Q_INVOKABLE void setQuality(const QString& level);
    Q_INVOKABLE void setFit(const QString& mode);
    Q_INVOKABLE void setHud(bool on);
    Q_INVOKABLE void setHudCorner(const QString& corner);
    Q_INVOKABLE void setHudScale(int pct);
    Q_INVOKABLE void setHudFrame(const QString& style);
    // Arms and disarms the desktop's drag layer. Turning it on is refused while the card is hidden,
    // so this can also just do nothing - the snapshot then reads the switch back off.
    Q_INVOKABLE void setHudDrag(bool on);
    // Puts the card back on its corner preset.
    Q_INVOKABLE void resetHudPos();
    Q_INVOKABLE void setOrbit(bool on);
    Q_INVOKABLE void setOrbitPeriod(int seconds);
    Q_INVOKABLE void setOrbitCount(int n);
    Q_INVOKABLE void setOrbitRings(int n);
    Q_INVOKABLE void setOrbitShape(const QString& name);
    Q_INVOKABLE void setOrbitScale(int pct);
    Q_INVOKABLE void setOrbitIconScale(int pct);
    Q_INVOKABLE void setOrbitPicks(const QStringList& names);
    Q_INVOKABLE void setOrbitPicksAll();
    Q_INVOKABLE void setIconsHidden(bool hide);
    Q_INVOKABLE void setOrbitDrag(bool on);
    Q_INVOKABLE void resetOrbitPos();
    Q_INVOKABLE void setChart(bool on);
    Q_INVOKABLE void setChartType(const QString& t);
    Q_INVOKABLE void setChartCorner(const QString& corner);
    Q_INVOKABLE void setChartScale(int pct);
    Q_INVOKABLE void setChartFrame(const QString& style);
    Q_INVOKABLE void setChartDrag(bool on);
    Q_INVOKABLE void resetChartPos();
    Q_INVOKABLE void setTrayAlpha(int pct);
    Q_INVOKABLE void setMaxFps(int fps);
    Q_INVOKABLE void setPowerCap(const QString& key, int value);
    Q_INVOKABLE void setMonitorFps(const QString& tag, int fps);
    Q_INVOKABLE void setAutostart(bool on);
    Q_INVOKABLE void setRotate(bool on);
    Q_INVOKABLE void setRotateInterval(int minutes);
    Q_INVOKABLE void setRotateScope(const QString& monitor);
    // The tray's 上一张/下一张: asks the renderer to move its rotation pointer by one. It owns the
    // pool and the index, so this window never has to guess which wallpaper comes next.
    Q_INVOKABLE void stepWallpaper(int delta);
    Q_INVOKABLE void addImageFile();
    // A second entry, not a replacement: picking one file stays the way to add a single
    // picture, and this one queues every supported file in a directory.
    Q_INVOKABLE void addImageFolder();
    // Removes the copy the import made under Wallpapers\local_NN; the renderer refuses while the
    // package is the one showing on a screen.
    Q_INVOKABLE void deleteImage(const QString& id);
    Q_INVOKABLE void toggleFavorite(const QString& id);
    Q_INVOKABLE bool isFavorite(const QString& id) const;
    Q_INVOKABLE void reload();
    // Asks the renderer to draw this wallpaper a second time for the big picture. Cheap to call
    // again with the same id: the renderer ignores it and just refreshes the heartbeat.
    Q_INVOKABLE void showPreview(const QString& id);
    Q_INVOKABLE void hidePreview();
    Q_INVOKABLE void quitRenderer();
    Q_INVOKABLE bool startRenderer();
    Q_INVOKABLE void openLogFolder();
    Q_INVOKABLE QString arg(const QString& name) const;

    // The online page. `search` is the current query at the current page with the current option
    // values; `import` fetches the original file and hands it to the renderer's own import command,
    // so an online picture becomes a package exactly the way one picked from disk does.
    Q_INVOKABLE void onlineSearch();
    Q_INVOKABLE void onlinePageStep(int delta);
    // 页码药丸条：跳到第 n 页。用步长凑（点 87 要按 86 下）不是实现。
    Q_INVOKABLE void onlineGoTo(int page);
    Q_INVOKABLE void onlineSetOption(const QString& key, const QString& value);
    Q_INVOKABLE void onlineImport(int index);
    // Bring the full-size file in for the picture the hero is showing. Every local card reaches the
    // hero as a 1280x720 `*_large.png`; a remote one had only the site's 432x243 preview, which in a
    // 1360 px box is a 3.1x upscale. Only a deliberate selection asks - the 4 s rotation does not.
    Q_INVOKABLE void onlineHeroPreview(int index);
    // The caption's 重试: the backoff gave up on this card, and he is asking for another round.
    Q_INVOKABLE void onlineHeroRetry();
    // 用户自己接一个站：抓取语言就是内置源那一份 json，所以"通配"不需要新代码，只需要一个能
    // 写文件的入口 + 一次重新加载。三个方法都只在设置窗这一侧，引擎仍然零网络。
    Q_INVOKABLE QString onlineSourceTemplate() const;
    // 返回空串=成功；否则是"为什么这份描述不能用"，原样显示在界面上。
    Q_INVOKABLE QString onlineAddSource(const QString& text);
    Q_INVOKABLE bool onlineRemoveSource(const QString& name);
    // MB with the unit named, for a card that has to say how big the file is before it is taken.
    Q_INVOKABLE QString onlineSize(int bytes) const;

signals:
    void stateChanged();
    void onlineChanged();
    void onlineProgressChanged();
    void onlineHeroFileChanged();
    void onlineHeroStateChanged();
    void langChanged();
    void targetChanged();
    void messageChanged();
    void favoritesChanged();
    void transitionChanged();
    void previewVideoPausedChanged();

private:
    void poll();
    void tick();
    void previewBeat();
    bool request(const QString& body, int timeoutMs, Json* out = nullptr);
    void ingest(const Json& status, const Json& list);
    void say(const QString& msg, bool isError);
    void reportStep();
    QString rendererExePath() const;
    bool launchRenderer(bool announce);
    void engineDied();
    bool engineStoppedItself() const;
    void loadOnlineSources();
    void rebuildOnlineOptions();
    QString onlineCacheDir() const;
    // <exe>/sources，两份内置源和用户填的那一份都在这。
    QString sourcesDir() const;
    // 重新读一遍目录，尽量把他还停在那个站这件事保住（loadOnlineSources 会跳回第一个）。
    void reloadSources();
    // Where the file for `url` lives on this disk: the hero preview and 入库 name it the same way off
    // the same row, so on a picture source the original he looked at first is the file the import
    // registers without asking the site again. A source that names a separate `hero` gets a second,
    // differently-suffixed file for it.
    QString onlineDest(const QVariantMap& row, const QString& url);
    // The hero's original, in three parts: find the card by its key (the rows shift under a page
    // change, an index does not survive it), then the one state transition every caller reports.
    void requestHero(const QString& key);
    void setHeroState(const QString& state);
    // "429" / "closed" - what fits in the caption. The full sentence goes to the log.
    static QString heroErrorOf(const QString& error);
    // The renderer's answer to addimage is "queued", so the package this window is waiting for has to
    // be recognised by what it carries - the origin page - and not by a guess at the next id.
    void importDownloaded(int index, const QString& key, const QString& path, const QString& page);
    void setPending(const QString& key, const QString& state, const QString& detail);
    void markOnlineRow(int index, const QString& state, const QString& detail);
    // delimage answers `busy` while something still holds the file this window just wrote; the retry
    // rides the event loop instead of freezing this thread.
    void deleteImageTry(const QString& id, int attempt);
    bool catalogHasId(const QString& id) const;
    // A package's origin, read back out of the catalog: the id of the local package that came from
    // this page, empty when nothing did.
    QString libraryIdFor(const QString& origin) const;
public:
    void setSelectAtStart(const QString& id) { selectAtStart_ = id; }
    void setSettingsAtStart(bool on) { settingsAtStart_ = on; }
    void setSettingsTabAtStart(const QString& t) { settingsTabAtStart_ = t; }
private:
    QString pipeName() const;

    QTimer* timer_ = nullptr;
    bool connected_ = false;
    int misses_ = 0;
    // The engine going away unasked is relaunchable; the user pressing 退出壁纸进程 in this window
    // is not. The three numbers behind it are the rate limit on relaunches.
    bool quitAsked_ = false;
    int relaunches_ = 0;
    qint64 relaunchAtMs_ = 0, relaunchWindowMs_ = 0;
    qint64 lastPid_ = 0;
    // A crashed engine is relaunched by engineDied(); an engine the user stopped from this window
    // is not, and the relaunches are rate-limit
    QString lang_ = QStringLiteral("zh");
    QString target_ = QStringLiteral("all");
    QString lastMessage_, lastError_;
    bool lastIsError_ = false;
    QVariantList monitors_, catalog_, caps_;
    QString quality_, host_, root_, logPath_;
    QString imageFit_ = QStringLiteral("fill");
    bool hud_ = false;
    bool hudReady_ = false;
    QString hudError_;
    QString hudCorner_ = QStringLiteral("top-right");
    int hudScale_ = 100;
    QString hudFrame_ = QStringLiteral("notch");
    bool hudDrag_ = false;
    bool hudCustom_ = false;
    bool orbit_ = false;
    bool orbitIcons_ = false;
    bool orbitReady_ = false;
    bool orbitClickable_ = false;
    int orbitPeriod_ = 45;
    int orbitPerRing_ = 12;
    int orbitRings_ = 1;
    int orbitShown_ = 0;
    QString orbitShape_ = QStringLiteral("ellipse");
    int orbitSlots_ = 0;
    int orbitScale_ = 100;
    int orbitIconScale_ = 100;
    QVariantList orbitIconList_;
    int orbitPickable_ = 0;
    bool orbitPicked_ = false;
    QString orbitError_;
    bool iconsHidden_ = false;
    bool orbitDrag_ = false;
    bool orbitCustom_ = false;
    bool chart_ = false;
    bool chartReady_ = false;
    QString chartType_ = QStringLiteral("bar");
    QString chartCorner_ = QStringLiteral("bottom-left");
    int chartScale_ = 100;
    QString chartFrame_ = QStringLiteral("notch");
    bool chartCustom_ = false;
    bool chartDrag_ = false;
    int chartHistory_ = 0;
    QString transition_ = QStringLiteral("liquid");
    int trayAlpha_ = 0;
    bool qualityIsGlobal_ = false, paused_ = false, autostart_ = false;
    bool orbitHeld_ = false;
    bool rotateOn_ = false;
    int rotateMin_ = 60, rotateNext_ = 0;
    QString showingId_, showingName_;
    // A tray step waiting to show up on screen: the delta that was asked for, the wallpaper it was
    // asked from, and the moment the wait stops being believable. 0 when nothing is outstanding.
    int stepDelta_ = 0;
    QString stepFromId_;
    qint64 stepWaitMs_ = 0;
    QStringList favorites_;
    QString selectAtStart_;
    bool settingsAtStart_ = false;
    QString settingsTabAtStart_;
    int maxFps_ = 60;
    double cpu_ = 0, ws_ = 0, uptime_ = 0;
    double gpu_ = -1;   // -1 = no answer available, which is not the same as 0 (see gpuPercent)
    double gpuSelf_ = -1, gpu3d_ = -1, gpuDec_ = -1;
    double cpuWin_ = 0;   // the window the renderer averaged cpu_ over, not a constant here
    QString uiSettingsPath_;

    QTimer* previewTimer_ = nullptr;   // only sends the heartbeat; LivePreview reads the pixels
    QString previewId_;
    qint64 previewBeatMs_ = 0;   // wall clock of the last heartbeat; see Bridge::previewBeat
    // The preview player's own transport state, read back from the engine rather than remembered.
    bool previewPaused_ = false;

    // ------------------------------------------------------------------ 在线源
    OnlineFetcher online_;
    QVariantList onlineRows_, onlineSourceItems_, onlineOptions_;
    QString onlineSource_, onlineQuery_, onlineError_, onlineSite_;
    int onlinePage_ = 1, onlineLastPage_ = 1;
    qint64 onlineTotal_ = 0;
    bool onlinePagesKnown_ = true;
    int onlineIntervalMs_ = 0;
    bool onlineAsk_ = true;
    bool onlineBusy_ = false;
    // Every page already in the library, read out of the catalog on each poll: the site's page is the
    // key, because that is what the imported package itself carries.
    QHash<QString, QString> originIds_;
    // The card whose package has not appeared in the catalog yet: its index, the origin it was
    // imported under, and the moment waiting stops being believable. -1 when nothing is waiting.
    int importRow_ = -1;
    QString importOrigin_;
    qint64 importAtMs_ = 0;
    // The download/import the page is showing on a tile, by the site's own key so it survives a new
    // search replacing the grid underneath it.
    QString pendingKey_, pendingState_, pendingDetail_;
    double progress_ = 0.0;
    // The hero's own download: which card it is for, and the file once it is on disk. Kept apart from
    // the pending import because the hero moves without being pressed, and a card he never clicked
    // must not start showing 下载中 on its button.
    QString heroKey_, heroFile_, heroState_, heroError_;
    // The card whose file is on its way right now, and when the last one started: the site answers 429
    // to a run of 1-4 MB originals, so this side keeps its own interval as well as the search's.
    QString heroInFlight_;
    qint64 heroStartMs_ = 0;
    int heroTries_ = 0;

    quintptr chromeHwnd_ = 0;   // the settings window, for showCommand
};

} // namespace sw
