#include "Wallpaper/OnlineSelfTest.hpp"
#include "Wallpaper/Bridge.hpp"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <QVariant>

#include <functional>

namespace sw {
namespace {

// Waits on what the app already reports rather than on sleeping: the same signals the page is drawn
// from, with a hard deadline. Returns whether the predicate became true.
bool waitFor(Bridge& b, const std::function<bool()>& pred, int timeoutMs) {
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.start(timeoutMs);
    const auto wake = [&loop] { loop.quit(); };
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&b, &Bridge::onlineChanged, &loop, wake);
    QObject::connect(&b, &Bridge::onlineProgressChanged, &loop, wake);
    QObject::connect(&b, &Bridge::stateChanged, &loop, wake);
    QObject::connect(&b, &Bridge::messageChanged, &loop, wake);
    while (!pred()) {
        if (!deadline.isActive()) break;
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return pred();
}

QVariantMap firstRow(Bridge& b) {
    const QVariantList rows = b.onlineRows();
    return rows.isEmpty() ? QVariantMap() : rows.first().toMap();
}

} // namespace

int RunOnlineSelfTest(Bridge& bridge, const QString& query, const QString& outFile,
                      const QString& source) {
    QFile f(outFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return 2;
    QTextStream ts(&f);
    const auto line = [&ts](const QString& s) { ts << s << '\n'; ts.flush(); };

    line("query " + query);
    const bool up = waitFor(bridge, [&] { return bridge.connected(); }, 15000);
    line(QString("renderer connected=%1").arg(up));
    if (!up) {
        line("verdict: no renderer, so the import stage cannot be measured");
        return 1;
    }
    const QString root = bridge.rootDir();
    line("root " + root);
    // Which site this run measured has to be in the report now that there is more than one, and the
    // first one alphabetically is not the one a regression check wants.
    if (!source.isEmpty()) bridge.setOnlineSource(source);
    line("source " + bridge.onlineSource() + " ask=" + (bridge.onlineAsk() ? "1" : "0"));

    bridge.setOnlineQuery(query);
    bridge.onlineSearch();
    const bool answered = waitFor(bridge, [&] {
        return !bridge.onlineBusy()
            && (!bridge.onlineRows().isEmpty() || !bridge.onlineError().isEmpty());
    }, 25000);
    line(QString("search answered=%1 busy=%2").arg(answered).arg(bridge.onlineBusy()));
    if (bridge.onlineRows().isEmpty()) {
        line("error: " + bridge.onlineError());
        return 1;
    }
    const QVariantMap first = firstRow(bridge);
    line(QString("rows=%1 page=%2/%3 total=%4")
             .arg(int(bridge.onlineRows().size())).arg(bridge.onlinePage()).arg(bridge.onlineLastPage())
             .arg(bridge.onlineTotal()));
    line("first key=" + first.value("key").toString());
    line("first dims=" + first.value("width").toString() + "x" + first.value("height").toString()
             + " bytes=" + first.value("bytes").toString() + " mime=" + first.value("mime").toString());
    line("first thumb=" + first.value("thumb").toString());
    line("first image=" + first.value("image").toString());
    line("first page=" + first.value("page").toString());

    const int catalogBefore = bridge.catalog().size();
    line(QString("catalog before=%1").arg(catalogBefore));
    bridge.onlineImport(0);
    const bool settled = waitFor(bridge, [&] {
        const QVariantMap r = firstRow(bridge);
        return r.value("state").toString() == QLatin1String("imported")
            || bridge.onlinePendingState() == QLatin1String("failed");
    }, 120000);
    const QVariantMap now = firstRow(bridge);
    const QString detail = now.value("state").toString() == QLatin1String("imported")
                             ? now.value("detail").toString() : bridge.onlinePendingDetail();
    line(QString("import settled=%1 state=%2 detail=%3 message=%4")
             .arg(settled).arg(now.value("state").toString()).arg(detail).arg(bridge.lastMessage()));

    const QString page = first.value("page").toString();
    const QString key = first.value("key").toString();
    const QString downloads = QDir(root).absoluteFilePath("cache/online");
    // The prefix is whichever source is up, not the one this was first written against: the file is
    // named `<source>-<key>.<ext>`, and a hardcoded name reads "no file" for every other site.
    const QStringList kept = QDir(downloads).entryList(QStringList() << (bridge.onlineSource() + QLatin1Char('-')
                                                                         + key + QStringLiteral(".*")),
                                                      QDir::Files);
    line("downloaded file " + kept.join(", ")
             + (kept.isEmpty() ? QString()
                               : QStringLiteral(" bytes=%1")
                                     .arg(QFileInfo(QDir(downloads).absoluteFilePath(kept.first())).size())));

    if (now.value("state").toString() == QLatin1String("imported")) {
        const QString id = now.value("detail").toString();
        const QString pkg = QDir(root).absoluteFilePath("Wallpapers/" + id);
        QString text;
        {
            // Scoped, and closed before the delete: while this handle is open the renderer cannot
            // remove the directory, and it says so with a sharing violation. Reading a manifest and
            // then deleting the package it belongs to has to let go of the file in between.
            QFile m(QDir(pkg).absoluteFilePath("wallpaper.json"));
            if (m.open(QIODevice::ReadOnly | QIODevice::Text)) text = QString::fromUtf8(m.readAll());
        }
        line("package dir " + pkg);
        line("package files " + QDir(pkg).entryList(QDir::Files, QDir::Name).join(", "));
        // The two things the manifest is supposed to have gained by coming from a site.
        line(QString("manifest carries the origin=%1 category=在线图片=%2")
                 .arg(text.contains(page)).arg(text.contains(QStringLiteral("在线图片"))));
        const QString thumbDir = QDir(root).absoluteFilePath("cache/thumbs");
        line(QString("thumb still=%1 large=%2")
                 .arg(QFileInfo(QDir(thumbDir).absoluteFilePath(id + "_still.png")).size())
                 .arg(QFileInfo(QDir(thumbDir).absoluteFilePath(id + "_large.png")).size()));
        // Back out of it: the library has to end with the packages it started with.
        bridge.deleteImage(id);
        const bool gone = waitFor(bridge, [&] { return bridge.catalog().size() == catalogBefore; }, 15000);
        line(QString("after delete catalog=%1 back to the start=%2 dir gone=%3")
                 .arg(bridge.catalog().size()).arg(gone).arg(!QFileInfo::exists(pkg)));
        return gone ? 0 : 1;
    }
    return 1;
}

} // namespace sw
