#pragma once
// Wallpaper/CoverAccess.hpp - the network manager QML's Image elements use, plus the small table that
// tells it which page each online cover was cut out of.
//
// Why this exists: 4kwallpapers' CDN answers 403 to a request with no Referer. The card covers are the
// only pictures in this window that QML loads straight (`Image { source: <url> }`), so they were the
// only ones that never carried the header - the hero and every import go through OnlineFetcher, which
// does set one. Measured in ui.log within the same minute, same host: thirteen
// `QQuickImage: Error transferring .../thumbs/27443.jpg - server replied with status code 403` lines
// against `online: .../purple-abstract-3840x2160-27445.jpg is 1708622 bytes, taken in 4 ranges`.
//
// The header is not set globally: a request that already has one is left alone, and a host we know
// nothing about is sent exactly what Qt built, so this cannot change how any other site behaves.
#include <QQmlNetworkAccessManagerFactory>
#include <QString>

namespace sw {

class CoverAccess {
public:
    // Called as each search page lands: this cover came from that item's page.
    static void remember(const QString& coverUrl, const QString& pageUrl);
    // A site's front page and the user agent its own fetcher uses, kept per host so a cover can be
    // asked for the same way the hero is.
    static void rememberSite(const QString& host, const QString& homepage, const QString& userAgent);
    // Empty when nothing is known about the URL, which leaves the request untouched.
    static QString refererFor(const QString& url);
    static QString userAgentFor(const QString& url);
};

// What the QML engine asks for when it needs a network manager. The engine owns the factory.
class CoverAccessFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager* create(QObject* parent) override;
};

} // namespace sw
