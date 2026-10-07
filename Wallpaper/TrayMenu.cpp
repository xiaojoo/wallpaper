#include "Wallpaper/TrayMenu.hpp"
#include "Wallpaper/Bridge.hpp"

#include <QApplication>
#include <QAction>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QWindow>

namespace sw {
namespace {

// The brand mark, loaded from the generated file the executable's own icon comes from too
// (tools/make-icon.py -> resources/Wallpaper.ico, staged next to the exe by CMake). One asset for
// Explorer, the taskbar and the tray, so the three cannot disagree about what the app looks like.
// The .ico carries 16/20/24/32/40/48/64/128/256 px frames, which is why nothing here scales a 16 px
// bitmap up at 200 % display scaling.
QIcon makeIcon() {
    const QString path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("Wallpaper.ico");
    QIcon icon(path);
    if (icon.isNull()) qWarning("tray: no icon at %s", path.toUtf8().constData());
    return icon;
}

} // namespace

TrayMenu::TrayMenu(Bridge& bridge, QObject* parent) : QObject(parent), bridge_(bridge) {}

// QMenu wants a QWidget parent and this class is not one, so the menu is created parentless and
// freed here.
TrayMenu::~TrayMenu() {
    delete menu_;
}

void TrayMenu::attach(QWindow* window) {
    window_ = window;
    if (!window_) return;
    // No tray on this session means nowhere to hide, so nothing gets installed and the window's
    // close button stays a real close rather than an exit the user cannot find.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    menu_ = new QMenu();
    // Plate, rule and gutter measured off his reference menu: #F9F9F9 background, a 1 px #D8D8D8
    // line with 4 px above and below between groups, 22 px row pitch and 17 px from the check to
    // the words. The two arithmetic relations that set those: pitch = 2*vertical padding + the
    // 15 px glyph box (so 4 -> 23, the reference's 22 needs a 3.5 px padding), and the check gap
    // = left padding + 4 (so 13 -> 17).
    menu_->setStyleSheet(QStringLiteral(
        "QMenu { background: #F9F9F9; }"
        "QMenu::item { padding: 4px 24px 4px 13px; }"
        "QMenu::item:selected { background: #D7E6FD; }"
        "QMenu::separator { height: 1px; background: #D8D8D8; margin-top: 4px; margin-bottom: 4px; }"));
    next_ = menu_->addAction(QString());
    prev_ = menu_->addAction(QString());
    menu_->addSeparator();
    autoPlay_ = menu_->addAction(QString());
    stopPlay_ = menu_->addAction(QString());
    menu_->addSeparator();
    quit_ = menu_->addAction(QString());
    // Checkable so the mark renders; the rows themselves never read their own mark, they only act,
    // and syncStates() re-marks both from what the renderer says.
    autoPlay_->setCheckable(true);
    stopPlay_->setCheckable(true);

    connect(quit_, &QAction::triggered, this, [] { QApplication::quit(); });
    connect(next_, &QAction::triggered, this, [this] { bridge_.stepWallpaper(1); });
    connect(prev_, &QAction::triggered, this, [this] { bridge_.stepWallpaper(-1); });
    connect(autoPlay_, &QAction::triggered, this, [this] { bridge_.setRotate(true); });
    connect(stopPlay_, &QAction::triggered, this, [this] { bridge_.setRotate(false); });

    icon_ = new QSystemTrayIcon(makeIcon(), this);
    connect(icon_, &QSystemTrayIcon::activated, this, [this] (QSystemTrayIcon::ActivationReason r) {
        switch (r) {
            // The shell's own menu (window class #32768) sizes its rows from the system font and
            // cannot be given padding, so the right-click pops our QMenu instead.
            case QSystemTrayIcon::Context: menu_->popup(QCursor::pos()); break;
            case QSystemTrayIcon::Trigger:
            case QSystemTrayIcon::DoubleClick: showWindow(); break;
            default: break;
        }
    });
    connect(&bridge_, &Bridge::stateChanged, this, &TrayMenu::syncStates);
    connect(&bridge_, &Bridge::langChanged, this, &TrayMenu::retranslate);
    // The menu is read the moment it opens: whether the renderer is up and where the rotation
    // stands have both moved many times since the last time he looked at it.
    connect(menu_, &QMenu::aboutToShow, this, &TrayMenu::syncStates);

    icon_->show();
    window_->installEventFilter(this);
    // Whether the shell took the icon is not otherwise observable from here, and Windows parks new
    // icons in the overflow flyout, where UIA does not list them until it is opened.
    qWarning("tray: icon shown=%s", icon_->isVisible() ? "true" : "false");
    retranslate();
    syncStates();
}

void TrayMenu::retranslate() {
    if (!menu_) return;
    quit_->setText(bridge_.t(QStringLiteral("tray_quit")));
    next_->setText(bridge_.t(QStringLiteral("next")));
    prev_->setText(bridge_.t(QStringLiteral("prev")));
    autoPlay_->setText(bridge_.t(QStringLiteral("tray_auto_play")));
    stopPlay_->setText(bridge_.t(QStringLiteral("tray_stop_play")));
}

void TrayMenu::syncStates() {
    if (!icon_ || !menu_) return;
    const bool up = bridge_.connected();
    const bool rotating = bridge_.rotateOn();
    for (QAction* a : {next_, prev_, autoPlay_, stopPlay_}) a->setEnabled(up);
    // The check mark is the state, so both rows go blank rather than claim "stopped" on a renderer
    // this window cannot read.
    autoPlay_->setChecked(up && rotating);
    stopPlay_->setChecked(up && !rotating);

    QString tip = QStringLiteral("Wallpaper");
    if (!up) {
        tip += QStringLiteral(" \u00b7 ") + bridge_.t(QStringLiteral("stopped"));
    } else {
        const QString name = bridge_.showingName();
        if (!name.isEmpty()) tip += QStringLiteral(" \u00b7 ") + name;
        if (rotating) tip += QStringLiteral(" \u00b7 ") + bridge_.t(QStringLiteral("auto_rotate"));
    }
    if (tip != icon_->toolTip()) icon_->setToolTip(tip);
}

bool TrayMenu::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Close && watched == window_ && icon_ && icon_->isVisible()) {
        event->ignore();   // quitting here would take the tray, and the only way back in, with it
        hideToTray();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void TrayMenu::hideToTray() {
    if (!window_) return;
    if (!icon_ || !icon_->isVisible()) { window_->close(); return; }
    window_->hide();
    if (!hinted_) {
        hinted_ = true;
        icon_->showMessage(QStringLiteral("Wallpaper"), bridge_.t(QStringLiteral("tray_hint")),
                           makeIcon(), 5000);
    }
}

void TrayMenu::showWindow() {
    if (!window_) return;
    window_->show();
    window_->raise();
    window_->requestActivate();
}

} // namespace sw
