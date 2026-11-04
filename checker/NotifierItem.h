#pragma once

#include <QObject>
#include <QString>

class KStatusNotifierItem;
class QMenu;

namespace Miryu {

/**
 * Plasma system tray item of the miryu-software-check binary.
 *
 * Mirrored after KDE Discover's notifier item (notifier/NotifierItem.cpp):
 * once updates were found (and the "updates available" notification was
 * shown), a tray entry stays resident so the user can come back to the
 * pending updates at any time. The right-click context menu offers:
 *
 *   - Open Miryu Software Center
 *   - Start Updates   (opens the center on the updates page and kicks off
 *                      the update flow via `miryu-software-center --update`)
 *   - Quit
 *
 * Left-clicking the icon also opens Miryu Software Center.
 */
class NotifierItem : public QObject
{
    Q_OBJECT

public:
    /**
     * Creates the tray entry. \a message (the "X RPM and/or Y Flatpak
     * package(s) available" text) is shown as the tooltip subtitle.
     */
    explicit NotifierItem(const QString &message, QObject *parent = nullptr);
    ~NotifierItem() override;

    /**
     * Launch modes for Miryu Software Center:
     *  - Open:         just open the center (default packages page) —
     *                  the tray menu's "Open Miryu Software Center" entry
     *  - ShowUpdates:  open the center on the Updates page — tray icon
     *                  click and the notification's "View Updates" action
     *  - StartUpdates: open the Updates page and queue + apply every
     *                  available update — the tray menu's "Start Updates"
     *                  entry (`miryu-software-center --update`)
     */
    enum class LaunchMode {
        Open,
        ShowUpdates,
        StartUpdates
    };

    /**
     * Launches Miryu Software Center in the given \a mode. The XDG
     * activation token (when coming from a tray/notification click) is
     * forwarded so the window is raised and focused.
     */
    static void launchSoftwareCenter(const QString &xdgActivationToken, LaunchMode mode);

Q_SIGNALS:
    /**
     * Emitted when the user picks "Quit" in the context menu (or otherwise
     * asks the checker to go away). The process should exit afterwards.
     */
    void quitRequested();

private:
    KStatusNotifierItem *m_item = nullptr;
};

}
