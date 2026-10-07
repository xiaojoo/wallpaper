#pragma once
// Wallpaper/Bridge.hpp - the settings window's view of the renderer, over the control pipe.
#include "Engine/Core/Json.hpp"

#include <QObject>
#include <QVariantList>
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
    Q_PROPERTY(int trayAlpha READ trayAlpha NOTIFY stateChanged)
    Q_PROPERTY(int maxFps READ maxFps NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(QString hostMethod READ hostMethod NOTIFY stateChanged)
    Q_PROPERTY(QString rootDir READ rootDir NOTIFY stateChanged)
    Q_PROPERTY(QString logPath READ logPath NOTIFY stateChanged)
    Q_PROPERTY(double cpuPercent READ cpuPercent NOTIFY stateChanged)
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
    QString imageFit() const { return imageFit_; }
    int trayAlpha() const { return trayAlpha_; }
    int maxFps() const { return maxFps_; }
    bool paused() const { return paused_; }
    QString hostMethod() const { return host_; }
    QString rootDir() const { return root_; }
    QString logPath() const { return logPath_; }
    double cpuPercent() const { return cpu_; }
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
    Q_INVOKABLE void setQuality(const QString& level);
    Q_INVOKABLE void setFit(const QString& mode);
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

signals:
    void stateChanged();
    void langChanged();
    void targetChanged();
    void messageChanged();
    void favoritesChanged();

private:
    void poll();
    void tick();
    void previewBeat();
    bool request(const QString& body, int timeoutMs, Json* out = nullptr);
    void ingest(const Json& status, const Json& list);
    void say(const QString& msg, bool isError);
    void reportStep();
public:
    void setSelectAtStart(const QString& id) { selectAtStart_ = id; }
    void setSettingsAtStart(bool on) { settingsAtStart_ = on; }
    void setSettingsTabAtStart(const QString& t) { settingsTabAtStart_ = t; }
private:
    QString pipeName() const;

    QTimer* timer_ = nullptr;
    bool connected_ = false;
    int misses_ = 0;
    QString lang_ = QStringLiteral("zh");
    QString target_ = QStringLiteral("all");
    QString lastMessage_, lastError_;
    bool lastIsError_ = false;
    QVariantList monitors_, catalog_, caps_;
    QString quality_, host_, root_, logPath_;
    QString imageFit_ = QStringLiteral("fill");
    int trayAlpha_ = 0;
    bool qualityIsGlobal_ = false, paused_ = false, autostart_ = false;
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
    QString uiSettingsPath_;

    QTimer* previewTimer_ = nullptr;   // only sends the heartbeat; LivePreview reads the pixels
    QString previewId_;
    qint64 previewBeatMs_ = 0;   // wall clock of the last heartbeat; see Bridge::previewBeat

    quintptr chromeHwnd_ = 0;   // the settings window, for showCommand
};

} // namespace sw
