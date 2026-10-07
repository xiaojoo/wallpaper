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
    bool hinted_ = false;   // the "it is still running" toast is a once-per-session thing
};

} // namespace sw
