#include "Online/OnlineFetcher.hpp"

#include "Engine/Core/Json.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>
#include <optional>

namespace sw {
namespace {

qint64 nowMs() {
    static const QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

QVariantMap toRow(const OnlineItem& it) {
    QVariantMap r;
    r.insert("key", QString::fromStdString(it.key));
    r.insert("title", QString::fromStdString(it.title));
    r.insert("thumb", QString::fromStdString(it.thumb));
    r.insert("image", QString::fromStdString(it.image));
    r.insert("hero", QString::fromStdString(it.hero));
    r.insert("page", QString::fromStdString(it.page));
    r.insert("width", it.width);
    r.insert("height", it.height);
    r.insert("bytes", qlonglong(it.bytes));
    r.insert("mime", QString::fromStdString(it.mime));
    return r;
}

} // namespace

// How long a file transfer may move zero bytes before it is called dead. A healthy link reports its
// first chunk in well under a second; this is only ever reached by a socket that has stopped.
static constexpr int kIdleAbortMs = 20000;

OnlineFetcher::OnlineFetcher(QObject* parent) : QObject(parent) {
    fileIdle_.setInterval(kIdleAbortMs);
    connect(&fileIdle_, &QTimer::timeout, this, [this] {
        if (!cur_) return;
        const qint64 now = bytesWritten();
        if (now != cur_->bytesAtLastCheck) {      // somebody is still moving; that is not a stall
            cur_->bytesAtLastCheck = now;
            return;
        }
        qWarning("online: file transfer moved no bytes for %d ms at %lld, aborting",
                 kIdleAbortMs, static_cast<long long>(now));
        for (auto& s : cur_->segs)
            if (s.reply) s.reply->abort();
    });
    searchIdle_.setInterval(kIdleAbortMs);
    connect(&searchIdle_, &QTimer::timeout, this, [this] {
        if (!searchReply_) return;
        qWarning("online: search moved no bytes for %d ms at %lld, aborting",
                 kIdleAbortMs, static_cast<long long>(searchBytes_));
        searchReply_->abort();
    });
}

int OnlineFetcher::streamCount() {
    static const int n = [] {
        const QByteArray v = qgetenv("WALLPAPER_DL_STREAMS");
        bool ok = false;
        const int asked = int(v.toInt(&ok));
        return ok && asked >= 1 && asked <= 8 ? asked : kSegments;
    }();
    return n;
}

OnlineSource* OnlineFetcher::source(const QString& name) {
    for (auto& s : sources_)
        if (name.compare(QString::fromStdString(s.name()), Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

bool OnlineFetcher::sameHost(const QString& url, const QString& homepage) {
    const auto trim = [](QString h) {
        if (h.startsWith(QLatin1String("www."))) h.remove(0, 4);
        return h.toLower();
    };
    const QString ha = trim(QUrl(url).host()), hb = trim(QUrl(homepage).host());
    if (ha.isEmpty() || hb.isEmpty()) return false;
    // The picture hosts are subdomains of the site (w.wallhaven.cc, th.wallhaven.cc), so the rule is
    // the site's own host, not an exact match - and it is still a rule: a description cannot point
    // the app at a host its homepage does not belong to.
    return ha == hb || ha.endsWith(QLatin1Char('.') + hb);
}

void OnlineFetcher::search(OnlineSource& src, const QString& query, int page, int retries) {
    // The newest query wins: a second call replaces the outstanding reply instead of racing it, so
    // what reaches the screen is what was last asked for.
    ++generation_;
    if (searchReply_) {
        searchReply_->abort();
        searchReply_ = nullptr;
    }
    const qint64 since = nowMs() - searchAtMs_;
    const int interval = src.minIntervalMs();
    if (since < interval) {
        QTimer::singleShot(int(interval - since), this, [this, &src, query, page, retries] {
            pumpSearch(src, query, page, retries);
        });
        return;
    }
    pumpSearch(src, query, page, retries);
}

void OnlineFetcher::pumpSearch(OnlineSource& src, const QString& query, int page, int retries) {
    const OnlineSource::Request req = src.BuildSearch(query.toStdString(), page);
    QUrl url(QString::fromStdString(req.url));
    QUrlQuery qq;
    for (const auto& [k, v] : req.query)
        qq.addQueryItem(QString::fromStdString(k), QString::fromStdString(v));
    url.setQuery(qq);

    QNetworkRequest r(url);
    r.setRawHeader("User-Agent", QByteArray::fromStdString(src.userAgent()));
    r.setTransferTimeout(15000);
    searchAtMs_ = nowMs();
    // The request line goes to the log. One process gets 24 rows and another gets a closed connection
    // on the same machine, and the only way to tell those apart is to see what each actually asked for.
    qWarning("online: GET %s", url.toString().toUtf8().constData());

    QNetworkReply* reply = nam_.get(r);
    searchReply_ = reply;
    searchBytes_ = 0;
    searchIdle_.start();
    QObject::connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 done, qint64) {
        if (done != searchBytes_) { searchBytes_ = done; searchIdle_.start(); }
    });
    const QString name = QString::fromStdString(src.name());
    const quint64 gen = generation_;
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, name, gen, url, query, page, retries, &src] {
        reply->deleteLater();
        if (searchReply_ == reply) { searchIdle_.stop(); searchReply_ = nullptr; }
        if (gen != generation_) return;   // superseded while it was in flight
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto fail = [&](const QString& why) {
            emit searched(name, {}, {}, QString(why).append(QStringLiteral(" (%1)").arg(status)));
        };
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("online: %s failed: %s (http %d, %lld bytes read)",
                     url.toString().toUtf8().constData(), reply->errorString().toUtf8().constData(), status,
                     qint64(body.size()));
            // No status at all means the connection died before the first byte - the site's edge, not
            // this request. One more ask, spaced by the source's own interval, is what 2 of 3 window
            // launches needed on 2026-10-12; an http status is the site answering, so that stays a no.
            if (status == 0 && retries > 0) {
                QTimer::singleShot(src.minIntervalMs(), this, [this, &src, query, page, retries] {
                    search(src, query, page, retries - 1);
                });
                return;
            }
            fail(reply->errorString());
            return;
        }
        if (status < 200 || status >= 300) { fail(QStringLiteral("http %1").arg(status)); return; }

        std::vector<OnlineItem> items;
        OnlineMeta meta;
        std::string err;
        const std::string text(body.constData(), size_t(body.size()));
        bool parsed = false;
        if (src.record().empty()) {
            std::string parseError;
            const auto json = Json::Parse(text, parseError);
            if (!json) { fail(QString::fromStdString(parseError)); return; }
            parsed = src.ParseSearch(*json, items, meta, err);
        } else {
            parsed = src.ParseHtml(text, page, items, meta, err);
        }
        if (!parsed) { emit searched(name, {}, {}, QString::fromStdString(err)); return; }
        QVariantList rows;
        rows.reserve(int(items.size()));
        for (const auto& it : items) rows.push_back(toRow(it));
        QVariantMap m;
        m.insert("page", meta.page);
        m.insert("lastPage", meta.lastPage);
        m.insert("total", qlonglong(meta.total));
        m.insert("pagesKnown", meta.pagesKnown);
        emit searched(name, rows, m, QString());
    });
}

void OnlineFetcher::download(OnlineSource& src, const QString& url, const QString& dest,
                             std::function<void(QString error, QString contentType)> done) {
    // One file at a time, in the order they were asked for: 24 tiles clicked fast is how a personal
    // app gets its address rate limited at a site that publishes a per-minute budget.
    fileQueue_.push_back(FileJob{src.name(), url, dest, std::move(done)});
    pumpFile();
}

void OnlineFetcher::pumpFile() {
    if (cur_ || fileQueue_.empty()) return;
    const FileJob job = fileQueue_.front();
    fileQueue_.erase(fileQueue_.begin());

    auto f = std::make_unique<SegFile>();
    f->job = job;
    f->part = job.dest + QStringLiteral(".part");
    f->sidecar = f->part + QStringLiteral(".json");
    QDir().mkpath(QFileInfo(job.dest).absolutePath());

    f->file = new QFile(f->part);
    // ReadWrite without Truncate is the whole resume story: what is already on disk stays, and every
    // segment writes over the range it was handed. Yesterday's abandoned 11.7 MB part is exactly the
    // bytes today's attempt would otherwise fetch a second time.
    const bool there = QFileInfo::exists(f->part);
    if (!f->file->open(QIODevice::ReadWrite)) {
        const QString why = f->file->errorString();
        const QString part = f->part;
        delete f->file;
        cur_.reset();
        job.done(QStringLiteral("cannot open %1: %2").arg(part, why), QString());
        pumpFile();
        return;
    }
    f->startedMs = nowMs();
    cur_ = std::move(f);
    fileIdle_.start();           // a connection that never delivers a byte is the same failure
    if (!there || !loadSidecar()) {
        // No usable history: either nothing is on disk, or what is there is some other file's bytes
        // (the same name under a new url, or a rung the site resized). Start over, and take the
        // record with it so a later attempt cannot resume onto the wrong bytes.
        cur_->file->resize(0);
        QFile::remove(cur_->sidecar);
        cur_->total = -1;
        cur_->ranged = false;
        cur_->segs.clear();
    }

    if (!cur_->segs.empty()) {
        for (size_t i = 0; i < cur_->segs.size(); ++i) openSegment(i);
        return;
    }
    openProbe();
}

// What the first request of an unknown file is for: how long is it, and does this host honour Range
// at all. One byte answers both and costs nothing to throw away, which beats carrying a half-finished
// stream into a plan that did not exist when it started. A host that ignores the header answers 200
// with a length, and the file then goes down the one stream it always used.
void OnlineFetcher::openProbe() {
    QNetworkRequest r(QUrl(cur_->job.url));
    OnlineSource* src = source(QString::fromStdString(cur_->job.sourceName));
    r.setRawHeader("User-Agent", src ? QByteArray::fromStdString(src->userAgent()) : QByteArray());
    r.setTransferTimeout(60000);
    r.setRawHeader("Range", "bytes=0-0");
    QNetworkReply* reply = nam_.get(r);
    cur_->probe = reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (!cur_) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        if (cur_->contentType.isEmpty()) cur_->contentType = type;
        const QString netErr = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        cur_->probe = nullptr;
        if (status == 206) {
            const QString cr = QString::fromUtf8(reply->rawHeader("Content-Range"));
            const int slash = cr.lastIndexOf(QLatin1Char('/'));
            cur_->ranged = true;
            cur_->total = slash >= 0 ? cr.mid(slash + 1).toLongLong() : -1;
        } else if (status >= 200 && status < 300) {
            cur_->ranged = false;
            cur_->total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        } else if (!netErr.isEmpty()) {
            finishFile(netErr);
            return;
        } else {
            finishFile(QStringLiteral("http %1").arg(status));
            return;
        }
        if (cur_->total <= 0) {
            finishFile(QStringLiteral("the site would not say how long the file is"));
            return;
        }
        planSegments();
        for (size_t i = 0; i < cur_->segs.size(); ++i) openSegment(i);
    });
}

void OnlineFetcher::openSegment(size_t i) {
    Segment& s = cur_->segs[i];
    OnlineSource* src = source(QString::fromStdString(cur_->job.sourceName));
    QNetworkRequest r(QUrl(cur_->job.url));
    r.setRawHeader("User-Agent", src ? QByteArray::fromStdString(src->userAgent()) : QByteArray());
    r.setTransferTimeout(60000);
    // One manager would happily carry all four ranges down a single HTTP/2 connection, and four
    // streams on one pipe is not four times the pipe: netstat on 2026-10-16 counted exactly 1
    // established :443 connection while "taken in 4 ranges" was running. HTTP/1.1 per request is
    // what makes the split real - the gain measured with python (148 KB/s on one connection,
    // 385 KB/s on six) only exists when there are six connections.
    r.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    const qint64 from = s.begin + s.written;
    // "bytes=" is part of the header, not decoration: sent without it the request is a Range header the
    // server does not understand, answers 200 with the whole file, and every stream then writes the
    // whole file at its own offset. Measured 2026-10-16 on cute-spooky: 4 streams, "got 11282188 of
    // 2820547 bytes", and a 4.9 MB file where 2.8 MB was expected.
    if (cur_->ranged)
        r.setRawHeader("Range", "bytes=" + QByteArray::number(from) + "-" + QByteArray::number(s.end));

    QNetworkReply* reply = nam_.get(r);
    s.reply = reply;
    QObject::connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64, qint64) {
        const qint64 got = bytesWritten();
        // Flush the record on the clock, not only at segment boundaries: the case the record exists
        // for is the process that gets killed with every stream still running, and that is exactly
        // what left an 11.7 MB orphan here on 2026-10-14.
        if (nowMs() - cur_->sidecarAtMs > 1000) { cur_->sidecarAtMs = nowMs(); saveSidecar(); }
        if (got != cur_->bytesAtLastCheck) {
            cur_->bytesAtLastCheck = got;
            fileIdle_.start();
        }
        emit progress(cur_->job.url, cur_->total > 0 ? double(got) / double(cur_->total) : 0.0,
                      got, cur_->total);
    });
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, i] { onSegmentBytes(i); });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, i] { onSegmentFinished(i); });
}

// The grid is a function of the total alone, so a resumed attempt lands its segments on the same
// offsets the previous one wrote - which is what makes the cursors on disk mean anything.
void OnlineFetcher::planSegments() {
    const qint64 total = cur_->total;
    const int n = (cur_->ranged && total >= kSegmentFromBytes) ? streamCount() : 1;
    std::vector<Segment> plan;
    plan.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        plan[size_t(i)].begin = total * i / n;
        plan[size_t(i)].end = (i + 1 == n ? total : total * (i + 1) / n) - 1;
    }
    cur_->segs = std::move(plan);
    qWarning("online: %s is %lld bytes, taken in %d range%s", cur_->job.url.toUtf8().constData(),
             static_cast<long long>(cur_->total), int(cur_->segs.size()),
             cur_->segs.size() == 1 ? "" : "s");
}

void OnlineFetcher::onSegmentBytes(size_t i) {
    if (!cur_ || i >= cur_->segs.size()) return;
    Segment& s = cur_->segs[i];
    if (!s.reply) return;
    const QByteArray chunk = s.reply->readAll();
    if (chunk.isEmpty()) return;
    if (!cur_->file || !cur_->file->isOpen()) return;
    const qint64 room = (s.end - s.begin + 1) - s.written;
    if (room <= 0) return;              // the range is full; anything else this stream sends is not ours
    const qint64 take = std::min<qint64>(room, chunk.size());
    cur_->file->seek(s.begin + s.written);
    const qint64 wrote = cur_->file->write(chunk.data(), take);
    if (wrote > 0) s.written += wrote;
}

void OnlineFetcher::onSegmentFinished(size_t i) {
    if (!cur_ || i >= cur_->segs.size()) return;
    Segment& s = cur_->segs[i];
    QNetworkReply* reply = s.reply.data();
    if (!reply) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (cur_->contentType.isEmpty())
        cur_->contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    QString error;
    if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
    else if (status >= 200 && status < 300) {
        // 206 while the plan expects a range, or 200 for the whole body: anything else is a page,
        // not the file, and it must not be renamed into the library.
    } else if (status != 0) error = QStringLiteral("http %1").arg(status);
    if (error.isEmpty() && cur_->ranged && status == 200)
        error = QStringLiteral("the site sent the whole file for a range request");
    onSegmentBytes(i);                  // whatever is still sitting in the socket
    reply->deleteLater();
    s.reply = nullptr;
    s.done = true;

    if (!error.isEmpty()) {
        saveSidecar();
        finishFile(error);
        return;
    }
    const bool all = [this] {
        for (const auto& x : cur_->segs)
            if (!x.done) return false;
        return true;
    }();
    if (!all) {
        saveSidecar();
        return;
    }
    // A segment that came up short is a hole, not a finished download: a file that is 15 MB of which
    // 14.9 is real imports happily and then stops mid-loop on the desktop. Re-ask the ranges that
    // are missing, and give up after three rounds rather than spinning against a server that keeps
    // closing the stream early.
    if (cur_->total > 0 && bytesWritten() < cur_->total) {
        if (++cur_->holeRounds > 3) {
            finishFile(QStringLiteral("the file stopped at %1 of %2 bytes")
                           .arg(bytesWritten()).arg(cur_->total));
            return;
        }
        for (size_t k = 0; k < cur_->segs.size(); ++k)
            if (cur_->segs[k].written < cur_->segs[k].end - cur_->segs[k].begin + 1) {
                cur_->segs[k].done = false;
                openSegment(k);
                return;
            }
        return;
    }
    finishFile(QString());
}

// Close the file, move it into place or keep the parts, and report once - whichever way this ends,
// the queue behind it has to move.
void OnlineFetcher::finishFile(const QString& error) {
    if (!cur_) return;
    SegFile& f = *cur_;
    if (f.file) {
        f.file->flush();
        f.file->close();
        delete f.file;
        f.file = nullptr;
    }
    for (auto& s : f.segs)
        if (s.reply) { s.reply->disconnect(this); s.reply->abort(); s.reply->deleteLater(); s.reply = nullptr; }
    fileIdle_.stop();

    const QString dest = f.job.dest;
    const QString part = f.part, sidecar = f.sidecar, type = f.contentType;
    auto done = f.job.done;
    const qint64 total = f.total;
    const qint64 got = bytesWritten();
    const qint64 ms = nowMs() - f.startedMs;
    const int streams = int(f.segs.size());
    cur_.reset();
    qWarning("online: %s %lld of %lld bytes in %lld ms over %d stream%s (%.0f KB/s)",
             error.isEmpty() ? "got" : "stopped at", static_cast<long long>(got),
             static_cast<long long>(total), static_cast<long long>(ms), streams,
             streams == 1 ? "" : "s", ms > 0 ? got / 1024.0 / (ms / 1000.0) : 0.0);

    if (!error.isEmpty()) {
        // Keep the bytes: that is the point of the sidecar. Only a file we never opened can be
        // nothing but garbage, and even then the record goes with it.
        done(error, type);
    } else if (total > 0 && got < total) {
        QFile::remove(part);
        QFile::remove(sidecar);
        done(QStringLiteral("the site sent an empty body"), type);
    } else if (total > 0 && QFileInfo(part).size() != total) {
        QFile::remove(part);
        QFile::remove(sidecar);
        done(QStringLiteral("the file is %1 bytes, the site said %2")
                 .arg(QFileInfo(part).size()).arg(total), type);
    } else if (got <= 0) {
        QFile::remove(part);
        QFile::remove(sidecar);
        done(QStringLiteral("the site sent an empty body"), type);
    } else if (QFile::exists(dest) && !QFile::remove(dest)) {
        done(QStringLiteral("%1 exists and could not be replaced").arg(dest), type);
    } else if (!QFile::rename(part, dest)) {
        done(QStringLiteral("cannot move %1 into place").arg(part), type);
    } else {
        QFile::remove(sidecar);
        done(QString(), type);
    }
    pumpFile();
}

qint64 OnlineFetcher::bytesWritten() const {
    if (!cur_) return 0;
    qint64 n = 0;
    for (const auto& s : cur_->segs) n += s.written;
    return n;
}

// The resume record: which url these bytes are for, how long that file is (the split depends on it),
// and how much of each segment is on disk. Written on every segment boundary and before every abort,
// because the case it exists for is the process that gets killed - which is how the 11.7 MB orphan
// happened in the first place.
void OnlineFetcher::saveSidecar() {
    if (!cur_ || !cur_->ranged || cur_->total <= 0) return;
    Json j = Json::Object();
    j.set("url", Json::Of(cur_->job.url.toStdString()));
    j.set("total", Json::Of(static_cast<long long>(cur_->total)));
    Json c = Json::Array();
    for (const auto& s : cur_->segs) c.push(Json::Of(static_cast<long long>(s.written)));
    j.set("cursors", std::move(c));
    QFile out(cur_->sidecar);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) out.write(j.dump(0).c_str());
}

bool OnlineFetcher::loadSidecar() {
    QFile in(cur_->sidecar);
    if (!QFileInfo::exists(cur_->sidecar) || !in.open(QIODevice::ReadOnly)) return false;
    std::string perr;
    const auto parsed = Json::Parse(QString::fromUtf8(in.readAll()).toStdString(), perr);
    if (!parsed) return false;
    const Json& j = *parsed;
    if (j.strOr("url") != cur_->job.url.toStdString()) return false;
    const qint64 total = qint64(j.i64Or("total"));
    const Json& c = j.at("cursors");
    const int n = int(c.size());
    if (total <= 0 || n <= 0) return false;

    cur_->total = total;
    cur_->ranged = true;
    cur_->segs.clear();
    cur_->segs.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        Segment& s = cur_->segs[size_t(i)];
        s.begin = total * i / n;
        s.end = (i + 1 == n ? total : total * (i + 1) / n) - 1;
        s.written = std::min<qint64>(c.items()[size_t(i)].asInt64(), s.end - s.begin + 1);
    }
    if (QFileInfo(cur_->part).size() < bytesWritten()) {
        cur_->total = -1;
        cur_->ranged = false;
        cur_->segs.clear();
        return false;
    }
    qWarning("online: resuming %s at %lld of %lld bytes", cur_->job.url.toUtf8().constData(),
             static_cast<long long>(bytesWritten()), static_cast<long long>(total));
    return true;
}

} // namespace sw
