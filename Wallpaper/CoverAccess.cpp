#include "Wallpaper/CoverAccess.hpp"

#include <QHash>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace sw {
namespace {

struct Site {
    QString homepage;
    QString ua;
};

QHash<QString, QString> g_coverPage;   // a cover's url -> the item page it was read out of
QHash<QString, Site> g_sites;          // host -> front page and the ua that site accepts

QString hostOf(const QString& url) { return QUrl(url).host().toLower(); }

// Every request QML's Image, Canvas or BusyIndicator makes comes through here.
class Manager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& req, QIODevice* outgoing) override {
        QNetworkRequest r = req;
        const QString url = r.url().toString();
        const QString host = hostOf(url);
        if (r.hasRawHeader("Referer")) {
            // Somebody already decided where this request came from; that decision wins.
        } else if (const QString ref = CoverAccess::refererFor(url); !ref.isEmpty()) {
            r.setRawHeader("Referer", ref.toUtf8());
        }
        if (const QString ua = CoverAccess::userAgentFor(url); !ua.isEmpty())
            r.setHeader(QNetworkRequest::UserAgentHeader, ua);
        return QNetworkAccessManager::createRequest(op, r, outgoing);
    }
};

} // namespace

void CoverAccess::remember(const QString& coverUrl, const QString& pageUrl) {
    if (coverUrl.isEmpty() || pageUrl.isEmpty()) return;
    g_coverPage.insert(coverUrl, pageUrl);
}

void CoverAccess::rememberSite(const QString& host, const QString& homepage, const QString& userAgent) {
    if (host.isEmpty()) return;
    g_sites.insert(host.toLower(), Site { homepage, userAgent });
}

QString CoverAccess::refererFor(const QString& url) {
    if (const auto it = g_coverPage.constFind(url); it != g_coverPage.constEnd()) return *it;
    if (const auto s = g_sites.constFind(hostOf(url)); s != g_sites.constEnd()) return s->homepage;
    return {};
}

QString CoverAccess::userAgentFor(const QString& url) {
    if (const auto s = g_sites.constFind(hostOf(url)); s != g_sites.constEnd()) return s->ua;
    return {};
}

QNetworkAccessManager* CoverAccessFactory::create(QObject* parent) { return new Manager(parent); }

} // namespace sw
