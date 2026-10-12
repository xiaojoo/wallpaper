// Wallpaper/LivePreview.cpp
#include "Wallpaper/LivePreview.hpp"

#include <QDateTime>
#include <QPainter>

#include <algorithm>

namespace sw {

LivePreview::LivePreview(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setRenderTarget(Image);
    setAntialiasing(false);
    connect(&timer_, &QTimer::timeout, this, &LivePreview::poll);
}

void LivePreview::setActive(bool on) {
    if (active_ == on) return;
    active_ = on;
    emit activeChanged();
    if (!on) {
        timer_.stop();
        haveFrame_ = false;
        fps_ = 0;
        emit fpsChanged();
        emit frameArrived();
        return;
    }
    if (!reader_.open()) {
        std::string err;
        reader_.Open(err);   // the section appears once the renderer acted on the command
    }
    windowStartMs_ = 0;
    windowFrames_ = 0;
    // 15 ms, not 33: Windows rounds timer wakeups to ~15.6 ms, so a 33 ms poll actually fired every
    // ~47 ms and delivered 21 of the 30 frames the renderer produced. Polling twice as often costs
    // one 4-byte header read per tick.
    timer_.start(15);
}

void LivePreview::setPaused(bool on) {
    if (paused_ == on) return;
    paused_ = on;
    // Coming back gets the whole staleness window from *now*, not from the frame it was left on, so
    // a renderer that is really gone still loses the held frame after the same 1200 ms anybody else
    // would wait for.
    if (!on) lastSeenMs_ = QDateTime::currentMSecsSinceEpoch();
    emit frameArrived();
}

void LivePreview::poll() {
    if (!reader_.open()) {
        std::string err;
        reader_.Open(err);
        if (!reader_.open()) return;
    }
    // Frames stopped arriving: the renderer exited, or its own heartbeat watchdog ended the pass.
    // Without this the item would sit on the last live picture forever and never hand the page back
    // to the saved strip. Not while this window paused the pass itself - that hold is the whole
    // point of `paused`, and resuming shows the frame it stopped on instead of the wallpaper's still.
    if (haveFrame_ && !paused_ && QDateTime::currentMSecsSinceEpoch() - lastSeenMs_ > 1200) {
        haveFrame_ = false;
        matching_ = false;
        fps_ = 0;
        emit fpsChanged();
        emit frameArrived();
    }
    const UINT w = reader_.width(), h = reader_.height();
    if (w == 0 || h == 0 || w > kPreviewMaxW || h > kPreviewMaxH) return;
    if (frame_.size() != QSize(int(w), int(h))) {
        frame_ = QImage(int(w), int(h), QImage::Format_ARGB32);
        haveFrame_ = false;
    }
    UINT rw = 0, rh = 0;
    UINT32 seq = 0;
    ULONGLONG stamp = 0;
    UINT64 hash = 0;
    // One copy, straight from the section into the image paint() draws. Newest() returns false when
    // the writer was mid-frame, so a torn picture never reaches the screen.
    // Only the header is looked at first: pulling the same frame again would copy 0.9 MB and
    // repaint the item for nothing, and the 15 ms poll would then report 66 "frames" a second.
    if (reader_.frames() == lastSeq_) return;
    if (!reader_.Newest(frame_.bits(), size_t(frame_.sizeInBytes()), rw, rh, seq, stamp, &hash)) return;
    if (rw != w || rh != h) return;
    lastSeq_ = seq;
    frameHash_ = hash;
    const bool wasMatching = matching_;
    matching_ = (hash == wantedHash_);
    if (matching_ != wasMatching) emit frameArrived();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    latency_ = double(ULONGLONG(GetTickCount64()) - stamp);
    if (windowStartMs_ == 0) {
        windowStartMs_ = now;
        windowFrames_ = 0;
    }
    if (const double el = double(now - windowStartMs_); el >= 1000.0) {
        fps_ = double(windowFrames_) * 1000.0 / el;
        windowStartMs_ = now;
        windowFrames_ = 0;
        emit fpsChanged();
    }
    ++windowFrames_;
    ++frames_;
    lastSeenMs_ = now;
    const bool was = haveFrame_;
    haveFrame_ = true;
    if (!was) emit frameArrived();
    update();
}

void LivePreview::setWantedId(const QString& id) {
    if (wantedId_ == id) return;
    wantedId_ = id;
    wantedHash_ = IdHash(id.toUtf8().constData());
    matching_ = (frameHash_ == wantedHash_);
    // The held frame belongs to this window's wallpaper only while its hash says so. If the
    // selection moved during the pause (the tray menu's 下一张 works while the window is minimised),
    // keeping it painted would keep painting the *previous* wallpaper on screen. Outside a pause the
    // frame is not dropped here: the handover onto a newly picked wallpaper deliberately keeps the
    // outgoing live frame painted until its own first frame lands.
    if (paused_ && !matching_) haveFrame_ = false;
    emit frameArrived();
}

void LivePreview::paint(QPainter* painter) {
    if (!haveFrame_) return;
    // PreserveAspectCrop, like the still and the strip drew it: the picture is 16:9 and the big
    // area is whatever is left over the cards, so cover rather than stretch.
    const QSizeF item(width(), height());
    const QSizeF src(frame_.size());
    if (item.width() <= 0 || item.height() <= 0 || src.width() <= 0) return;
    const double s = std::max(item.width() / src.width(), item.height() / src.height());
    const QSizeF scaled = src * s;
    const QRectF target(-(scaled.width() - item.width()) / 2.0, -(scaled.height() - item.height()) / 2.0,
                        scaled.width(), scaled.height());
    painter->setRenderHint(QPainter::SmoothPixmapTransform, s > 1.3);
    // Source, not SourceOver: this item is opaque, and blending would let the dark card behind it
    // show through the semi-transparent snow edges.
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->drawImage(target, frame_);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
}

} // namespace sw
