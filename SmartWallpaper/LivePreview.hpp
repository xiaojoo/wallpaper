#pragma once
// SmartWallpaper/LivePreview.hpp - the settings window's big picture when the renderer is drawing
// the wallpaper live. A QQuickPaintedItem rather than an Image with a new URL per frame: changing
// the URL makes Qt build a fresh texture for every frame and blank the item while it loads, which
// measured 16-18 fps and a visible flash. Here the same backing texture is repainted.
#include "Engine/App/PreviewStream.hpp"

#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>

namespace sw {

class LivePreview : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool running READ running NOTIFY frameArrived)
    // What the window actually shows, not what the renderer produced: the delivered rate.
    Q_PROPERTY(double fps READ fps NOTIFY fpsChanged)
    Q_PROPERTY(double latencyMs READ latencyMs NOTIFY frameArrived)
    Q_PROPERTY(qint64 frames READ frames NOTIFY frameArrived)
    // The frame in the section may still belong to the wallpaper the user just left. Until one of
    // the newly selected wallpaper arrives, matching stays false and the page shows that
    // wallpaper's already-loaded picture instead of the previous one still moving.
    Q_PROPERTY(QString wantedId READ wantedId WRITE setWantedId NOTIFY frameArrived)
    Q_PROPERTY(bool matching READ matching NOTIFY frameArrived)

public:
    explicit LivePreview(QQuickItem* parent = nullptr);

    void paint(QPainter* painter) override;

    bool active() const { return active_; }
    void setActive(bool on);
    bool running() const { return haveFrame_; }
    double fps() const { return fps_; }
    double latencyMs() const { return latency_; }
    qint64 frames() const { return frames_; }
    QString wantedId() const { return wantedId_; }
    void setWantedId(const QString& id);
    bool matching() const { return matching_; }

signals:
    void activeChanged();
    void frameArrived();
    void fpsChanged();

private:
    void poll();

    PreviewReader reader_;
    QImage frame_;
    QTimer timer_;
    bool active_ = false;
    bool haveFrame_ = false;
    qint64 frames_ = 0;
    double fps_ = 0, latency_ = 0;
    qint64 windowStartMs_ = 0;
    int windowFrames_ = 0;
    qint64 lastSeenMs_ = 0;
    UINT32 lastSeq_ = 0;
    QString wantedId_;
    quint64 wantedHash_ = 0, frameHash_ = 0;
    bool matching_ = false;
};

} // namespace sw
