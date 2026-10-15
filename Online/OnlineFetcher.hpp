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
#include <memory>
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
    // imported as if it were a picture. Only one *file* at a time, in the order they were asked for:
    // a grid of 24 tiles clicked fast should not open 24 sockets at a site that publishes a
    // per-minute budget.
    //
    // A file big enough to be worth it (an online clip is 12-19 MB) is pulled in kSegments parallel
    // Range requests, because that is the difference measured on this machine on 2026-10-15: one
    // connection to motionbgs' media host took 1 MB in 6.9 s (148 KB/s), six parallel 256 KB ranges
    // took the same wall-clock window for 385 KB/s. Small pictures stay single - the budget belongs to
    // the site, the speed to the clip.
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

    // One connection of one file. `begin`/`end` are absolute bytes in the finished file and `written`
    // is how much of that range is already on disk - which is also the resume offset, and the only
    // thing a killed process has to leave behind for the next attempt to be more than a re-download.
    struct Segment {
        qint64 begin = 0, end = -1, written = 0;
        QPointer<QNetworkReply> reply;
        bool done = false;
    };

    // The download in flight: one .part, one handle, every segment seeking to its own offset. All the
    // handlers run on this thread's event loop, so the shared handle needs no lock.
    struct SegFile {
        FileJob job;
        QString part, sidecar, contentType;
        qint64 total = -1;      // what the site says the whole file is
        bool ranged = false;    // the site answered 206; a 200 means one plain stream
        QPointer<QNetworkReply> probe;   // the 1-byte ask that found out which of the two it is
        std::vector<Segment> segs;
        QFile* file = nullptr;
        qint64 bytesAtLastCheck = 0;
        qint64 startedMs = 0;   // for the line that says how long the file took
        qint64 sidecarAtMs = 0; // the once-a-second flush that survives a killed process
        int holeRounds = 0;     // re-asks of ranges that came up short, capped so a broken server
                                // cannot make this spin forever
    };

    void pumpSearch(OnlineSource& src, const QString& query, int page, int retries);
    void pumpFile();
    void openProbe();                   // the 1-byte ask that says how long the file is and whether
                                        // this host honours Range
    void planSegments();                  // after the first reply's headers: how many streams is it
    void openSegment(size_t i);           // one Range request for segment i
    void onSegmentBytes(size_t i);        // readyRead: move it into the file at its own offset
    void onSegmentFinished(size_t i);
    void finishFile(const QString& error);   // close, rename or keep the parts, report once
    void saveSidecar();
    bool loadSidecar();
    qint64 bytesWritten() const;

    QNetworkAccessManager nam_;
    std::vector<OnlineSource> sources_;
    QPointer<QNetworkReply> searchReply_;
    // A transfer whose socket stops moving bytes never reports anything: measured 2026-10-12, a
    // wallhaven CDN connection sat at 1310720 bytes for 7 minutes with no `finished` signal, and
    // Qt's own 60 s transfer timeout did not fire. This queue is single-slot, so that one reply held
    // up everything behind it - including the 入库 he had actually pressed. The watchdog therefore
    // watches the one thing that can't lie: has the *aggregate* byte count moved since the last tick
    // (one segment finishing early is normal; one segment stalling forever is the failure).
    QTimer fileIdle_;
    std::unique_ptr<SegFile> cur_;
    // Same rule for the api call: a search that stops moving bytes holds `onlineBusy_` up forever,
    // which on 2026-10-12 is exactly what happened when wallhaven became unreachable mid-session -
    // the page sat at 正在请求 for minutes and neither 15 s nor 60 s transfer timeout fired.
    QTimer searchIdle_;
    qint64 searchBytes_ = -1;
    std::vector<FileJob> fileQueue_;
    qint64 searchAtMs_ = 0;
    quint64 generation_ = 0;   // stale replies are dropped by this, not by hope

    // 4 of the measured 6-way gain, because the budget that gets us rate limited is the site's, not
    // the transfer's. Anything under this size stays one connection: a 400 KB hero does not benefit
    // from four round trips, and the round trip on this link is ~1 s.
    // Overridable for the same reason WALLPAPER_VIDEO=sw exists: the question "is four streams
    // actually faster than one" can only be answered on his link, against a site whose speed moves
    // by 5x between midnight and now, and it has to be answerable without a rebuild between runs.
    static int streamCount();
    static constexpr int kSegments = 4;
    // Under this, one stream: the split costs a probe round trip (~1 s on this link) and a 200 KB
    // hero pays more than it saves. Over it, the measured difference on the same 2.8 MB clip,
    // paired minutes apart on 2026-10-16: 8403 ms and 15497 ms on one stream (328 / 178 KB/s)
    // against 6050 ms and 6117 ms on four (455 / 450 KB/s). The stability is the part worth having
    // - one stream is at the mercy of whatever the link decides a connection is worth this second.
    static constexpr qint64 kSegmentFromBytes = 1LL * 1024 * 1024;
};

} // namespace sw
