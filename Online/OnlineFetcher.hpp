#pragma once
// Online/OnlineFetcher.hpp - the network side of an online wallpaper source.
//
// Everything here is asynchronous on this thread's event loop. That is the point: the settings
// window's GUI thread is the one thing that has to keep answering, and a synchronous HTTP call has
// exactly the shape of the stall that 没有渲染器 was (a blocked GUI thread reads as "客户端有点卡").
#include "Online/OnlineSource.hpp"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <string>
#include <vector>

class QFile;

namespace sw {

class OnlineFetcher final : public QObject {
    Q_OBJECT
public:
    explicit OnlineFetcher(QObject* parent = nullptr);

    std::vector<OnlineSource>& sources() { return sources_; }
    void setSources(std::vector<OnlineSource> s) { sources_ = std::move(s); }
    OnlineSource* source(const QString& name);

    // One page of results. A second call while one is running is not dropped and not queued: it
    // replaces the outstanding request, because the newer query is the one the user is looking at.
    // `retries` is what a connection-level failure is answered with: wallhaven's api closed the socket
    // before a single byte on 2 of 3 window launches (http 0, 0 bytes read, 2026-10-12) and answered
    // the same URL fine on the third, so one backoff asks again without treating a hiccup as a dead
    // source. It goes back through search(), which is what keeps the site's own interval in force.
    void search(OnlineSource& src, const QString& query, int page, int retries = 1);

    // Streams `url` into `dest` through a *.part file, so an interrupted download can never be
    // imported as if it were a picture. Only one at a time: a grid of 24 tiles clicked fast should
    // not open 24 sockets at a site that publishes a per-minute budget.
    // `done` gets the error (empty on success) and the content type the site claimed, which is what
    // lets the caller refuse an html error page saved under a picture's name.
    void download(OnlineSource& src, const QString& url, const QString& dest,
                  std::function<void(QString error, QString contentType)> done);

    // A source description may only talk to the host it names, and to the hosts its own urls use.
    // This is what stops a hand-written Sources/*.json from turning the app into an open proxy.
    static bool sameHost(const QString& url, const QString& homepage);

signals:
    // items: one QVariantMap per OnlineItem, ready for a QML model.
    void searched(QString sourceName, QVariantList items, QVariantMap meta, QString error);
    void progress(QString url, double fraction, qint64 doneBytes, qint64 totalBytes);

private:
    struct FileJob {
        std::string sourceName;   // looked up again per job, so a rebuilt vector cannot dangle
        QString url, dest;
        std::function<void(QString, QString)> done;
    };

    void pumpSearch(OnlineSource& src, const QString& query, int page, int retries);
    void pumpFile();

    QNetworkAccessManager nam_;
    std::vector<OnlineSource> sources_;
    QPointer<QNetworkReply> searchReply_;
    QPointer<QNetworkReply> fileReply_;
    // A transfer whose socket stops moving bytes never reports anything: measured 2026-10-12, a
    // wallhaven CDN connection sat at 1310720 bytes for 7 minutes with no `finished` signal, and
    // Qt's own 60 s transfer timeout did not fire. This queue is single-slot, so that one reply held
    // up everything behind it - including the 入库 he had actually pressed. The watchdog therefore
    // watches the one thing that can't lie: has the byte count moved since the last tick.
    QTimer fileIdle_;
    qint64 fileBytes_ = -1;
    // Same rule for the api call: a search that stops moving bytes holds `onlineBusy_` up forever,
    // which on 2026-10-12 is exactly what happened when wallhaven became unreachable mid-session -
    // the page sat at 正在请求 for minutes and neither 15 s nor 60 s transfer timeout fired.
    QTimer searchIdle_;
    qint64 searchBytes_ = -1;
    std::vector<FileJob> fileQueue_;
    class QFile* outFile_ = nullptr;
    qint64 searchAtMs_ = 0;
    quint64 generation_ = 0;   // stale replies are dropped by this, not by hope
};

} // namespace sw
