// Wallpaper/main.cpp - the Qt settings window. It owns no wallpaper state: everything it
// shows and changes goes through the renderer's control pipe.
#include "Wallpaper/Bridge.hpp"
#include "Wallpaper/LivePreview.hpp"
#include "Wallpaper/TrayMenu.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QApplication>
#include <QIcon>
#include <QMessageLogContext>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QQmlContext>
#include <QTextStream>
#include <QUrl>
#include <Qt>

namespace {
// A GUI-subsystem process has no stderr, so without this a QML error would look like a silent exit.
void logMessage(QtMsgType, const QMessageLogContext& ctx, const QString& msg) {
    static QFile f(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("ui.log"));
    if (!f.isOpen() && !f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts << msg << " \n" << "  [" << ctx.file << ":" << ctx.line << "]\n";
    ts.flush();
}
} // namespace

int main(int argc, char* argv[]) {
    // QApplication, not QGuiApplication: the import button opens QFileDialog, which is a QWidget, and
    // creating one with only a QGuiApplication kills the process on the click
    // ("QWidget: Cannot create a QWidget without QApplication").
    QApplication app(argc, argv);
    qInstallMessageHandler(logMessage);
    app.setOrganizationName(QStringLiteral("SmartWallpaper"));
    app.setApplicationName(QStringLiteral("SmartWallpaper"));
    // The taskbar and alt-tab take this one; Explorer takes the icon embedded in the exe
    // (resources/app.rc). Both come from the same generated Wallpaper.ico.
    app.setWindowIcon(QIcon(QDir(QCoreApplication::applicationDirPath())
                                .absoluteFilePath("Wallpaper.ico")));
    // Closing the window hides it to the tray now, and a hidden window must not read as "no windows
    // left". The tray's 退出 is the one path that ends this process, and it calls quit() itself.
    app.setQuitOnLastWindowClosed(false);

    sw::Bridge bridge;
    sw::TrayMenu tray(bridge);   // gets its window in attach(), below, once QML has made one
    // Deep links for the click harness: self-drawn Qt controls are not reachable through UIA on
    // this build, so the state a screenshot should show is passed in here instead.
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        // The id flags need a value after them; --settings is a bare flag and must still be
        // recognised when it comes last, so the bound is i < argc, not i + 1 < argc.
        if (a == "--select" && i + 1 < argc) bridge.setSelectAtStart(QString::fromLocal8Bit(argv[++i]));
        else if (a == "--settings") bridge.setSettingsAtStart(true);
        else if (a == "--settings-tab" && i + 1 < argc) { bridge.setSettingsAtStart(true);
            bridge.setSettingsTabAtStart(QString::fromLocal8Bit(argv[++i])); }
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("Bridge"), &bridge);
    // The title bar's close button asks the tray to hide the window instead of closing it.
    engine.rootContext()->setContextProperty(QStringLiteral("Tray"), &tray);
    qmlRegisterType<sw::LivePreview>("SW", 1, 0, "LivePreview");
    // Loaded from disk next to the exe: editing Main.qml then restarting is enough to see the
    // change, which matters more here than shipping one embedded resource.
    const QString qml = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("qml/Main.qml");
    if (!QFileInfo::exists(qml)) qWarning("missing %s", qml.toUtf8().constData());
    engine.load(QUrl::fromLocalFile(qml));
    if (engine.rootObjects().isEmpty()) {
        qWarning("QML failed to load; check %s", qml.toUtf8().constData());
        return 1;
    }
    // `winId` is not exposed to QML (it is a plain C++ method on QWindow), so the HWND the chrome
    // filter watches is taken from the root object here rather than from the QML side.
    for (QObject* o : engine.rootObjects())
        if (auto* w = qobject_cast<QWindow*>(o)) {
            bridge.adoptChromeWindow(static_cast<qint64>(w->winId()));
            tray.attach(w);
            break;
        }
    return app.exec();
}
