#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

class QProcess;
class KNotification;

namespace Miryu {

class NotifierItem;

/**
 * Update check engine of the standalone miryu-software-check binary.
 *
 * Mirrored after KDE Discover's notifier (notifier/DiscoverNotifier.cpp):
 * it performs the system update probes OUTSIDE the software center GUI so
 * that "updates available" notifications keep working while Miryu Software
 * Center itself is not running.
 *
 * Two probes are executed concurrently, exactly as required:
 *
 *   - RPM updates:     `dnf5 check-update --refresh`
 *   - Flatpak updates: `flatpak update`
 *
 * Both commands are run WITHOUT a controlling terminal. `flatpak update`
 * prints the numbered update summary table and then asks
 * "Proceed with these changes to the user installation? [Y/n]" — with
 * stdin/stdout not being a TTY flatpak answers "n" by itself
 * (see flatpak's flatpak_yes_no_prompt()), so the check never installs
 * anything. The numbered rows of the summary table are counted instead.
 *
 * `dnf5 check-update` exits with code 100 when upgrades exist and prints
 * rows of the form "name.arch <version> <repository>", which are counted.
 *
 * When at least one update is found, a KNotification is raised:
 *   "X RPM package(s) and/or Y Flatpak package(s) available for update"
 * with a "View Updates" action that launches Miryu Software Center
 * (honouring the XDG activation token so the window is focused). In
 * addition, a Plasma system tray entry is kept resident (see NotifierItem)
 * with a context menu: open the software center, start the updates, quit.
 *
 * The notification frequency (daily / weekly / monthly / never) is read
 * from the shared Miryu configuration ("General" group of miryurc, key
 * "updateNotifyFrequency", written by the software center's
 * "Check Update Frequency Settings" dialog); the time of the last
 * notification is persisted in the miryuchecknotifierrc state file.
 */
class UpdateCheckNotifier : public QObject
{
    Q_OBJECT

public:
    explicit UpdateCheckNotifier(QObject *parent = nullptr);
    ~UpdateCheckNotifier() override;

    /**
     * Reads the configured notification frequency and the last notification
     * time. Returns false when the check must be skipped right now, either
     * because notifications are disabled ("never") or because the previous
     * notification is still within the configured interval (daily / weekly /
     * monthly).
     */
    static bool shouldRunCheckNow();

    /**
     * Launches both probes asynchronously. finished() is emitted once both
     * probes completed and the notification (if any) has been handled.
     */
    void start();

Q_SIGNALS:
    /**
     * Emitted after both checks finished and the notification (if any) was
     * sent. The receiver should quit the event loop afterwards.
     */
    void finished();

private:
    struct ProbeResult {
        int count = -1;        // number of available updates, -1 = probe failed
        bool done = false;     // probe has finished
    };

    static QString notificationIntervalSetting();
    static QDateTime lastNotificationTime();

    void runRpmProbe();
    void runFlatpakProbe();
    void onProbeFinished();
    void evaluateResults();
    void notifyUpdates(int rpmCount, int flatpakCount);
    void finish();

    static QByteArray stripAnsiEscapes(const QByteArray &data);
    static int parseDnf5CheckUpdateOutput(const QByteArray &output);
    static int parseFlatpakUpdateOutput(const QByteArray &output);

    QProcess *m_rpmProcess = nullptr;
    QProcess *m_flatpakProcess = nullptr;
    ProbeResult m_rpmResult;
    ProbeResult m_flatpakResult;
    KNotification *m_notification = nullptr;
    NotifierItem *m_trayItem = nullptr;
};

}
