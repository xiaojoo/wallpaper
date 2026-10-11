#pragma once
// Wallpaper/TrayMenu.hpp - the tray icon the settings window lives in after its close button only
// hides it, and the wallpaper commands that have to work with the window shut.
//
// It sits in the settings process rather than next to the renderer's own optional icon because the
// commands are this window's: they go to the renderer over the same pipe the Bridge already owns.
#include <QObject>

class QMenu;
class QAction;
class QSystemTrayIcon;
class QWindow;

namespace sw {

class Bridge;

class TrayMenu final : public QObject {
    Q_OBJECT
public:
    explicit TrayMenu(Bridge& bridge, QObject* parent = nullptr);
    ~TrayMenu() override;

    // Built once the root window exists: the icon needs it to hide and show, and the filter needs
    // it to turn a close into a hide.
    void attach(QWindow* window);

    // Close means "put it in the tray" while the icon is up. Alt+F4 and the taskbar's own close
    // arrive here; the title bar's button asks for hideToTray() directly.
    bool eventFilter(QObject* watched, QEvent* event) override;

    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void showWindow();

private:
    void retranslate();
    void syncStates();

    Bridge& bridge_;
    QWindow* window_ = nullptr;
    QSystemTrayIcon* icon_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* quit_ = nullptr;
    QAction* next_ = nullptr;
    QAction* prev_ = nullptr;
    QAction* autoPlay_ = nullptr;
    QAction* stopPlay_ = nullptr;
    // Holds the desktop's own motion. Sits under the two rotation rows because it is the same kind of
    // thing - a switch about what plays - and greyed on a still, where there is nothing to hold.
    QAction* pauseWallpaper_ = nullptr;
    // The two desktop overlays that are worth turning on without opening the window: the ring, and
    // the shell's icon layer it stands in for. Both are checkable because both are states, and the
    // check is the only place they can be read from here.
    QAction* orbit_ = nullptr;
    QAction* pauseRing_ = nullptr;   // stops the ring turning without touching 转一圈
    QAction* hideIcons_ = nullptr;
    bool hinted_ = false;   // the "it is still running" toast is a once-per-session thing
};

} // namespace sw
