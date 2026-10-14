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
        if (!fileReply_) return;
        qWarning("online: file transfer moved no bytes for %d ms at %lld, aborting",
                 kIdleAbortMs, static_cast<long long>(fileBytes_));
        fileReply_->abort();
    });
    searchIdle_.setInterval(kIdleAbortMs);
    connect(&searchIdle_, &QTimer::timeout, this, [this] {
        if (!searchReply_) return;
        qWarning("online: search moved no bytes for %d ms at %lld, aborting",
                 kIdleAbortMs, static_cast<long long>(searchBytes_));
        searchReply_->abort();
    });
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
    if (fileReply_ || fileQueue_.empty()) return;
    const FileJob job = fileQueue_.front();
    fileQueue_.erase(fileQueue_.begin());
    OnlineSource* src = source(QString::fromStdString(job.sourceName));

    const QString part = job.dest + QStringLiteral(".part");
    QFile::remove(part);
    QDir().mkpath(QFileInfo(job.dest).absolutePath());
    outFile_ = new QFile(part);
    if (!outFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = outFile_->errorString();
        delete outFile_;
        outFile_ = nullptr;
        job.done(QStringLiteral("cannot open %1: %2").arg(part, why), QString());
        pumpFile();
        return;
    }

    QNetworkRequest r(QUrl(job.url));
    r.setRawHeader("User-Agent", src ? QByteArray::fromStdString(src->userAgent()) : QByteArray());
    r.setTransferTimeout(60000);
    QNetworkReply* reply = nam_.get(r);
    fileReply_ = reply;
    fileBytes_ = 0;
    fileIdle_.start();           // a connection that never delivers a byte is the same failure

    QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                     [this, url = job.url](qint64 done, qint64 total) {
                         if (done != fileBytes_) { fileBytes_ = done; fileIdle_.start(); }
                         emit progress(url, total > 0 ? double(done) / double(total) : 0.0, done, total);
                     });
    QObject::connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (outFile_ && outFile_->isOpen()) outFile_->write(reply->readAll());
    });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, job, part]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        QString error;
        if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
        else if (status < 200 || status >= 300) error = QStringLiteral("http %1").arg(status);
        reply->deleteLater();
        fileReply_ = nullptr;
        fileIdle_.stop();
        if (outFile_) {
            outFile_->flush();
            outFile_->close();
            delete outFile_;
            outFile_ = nullptr;
        }
        // What actually reached the disk, not what the reply believes it received: a body that was cut
        // off mid-transfer still counts up while nothing of it was written.
        const qint64 got = QFileInfo(part).size();
        if (!error.isEmpty()) {
            QFile::remove(part);
            job.done(error, type);
        } else if (got <= 0) {
            QFile::remove(part);
            job.done(QStringLiteral("the site sent an empty body"), type);
        } else if (QFile::exists(job.dest) && !QFile::remove(job.dest)) {
            QFile::remove(part);
            job.done(QStringLiteral("%1 exists and could not be replaced").arg(job.dest), type);
        } else if (!QFile::rename(part, job.dest)) {
            QFile::remove(part);
            job.done(QStringLiteral("cannot move %1 into place").arg(part), type);
        } else {
            job.done(QString(), type);
        }
        pumpFile();
    });
}

} // namespace sw
