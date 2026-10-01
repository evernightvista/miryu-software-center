#pragma once

#include <QObject>
#include <QList>
#include "Package.h"

class QTimer;

namespace Miryu {

class Backend;

/**
 * Periodically checks for available RPM updates via the Backend.
 *
 * The check interval is read from KSharedConfig ("General" group) so that
 * changes made in SettingsDialog take effect after the next start() /
 * checkNow() call.
 *
 * On every successful check the signal updatesAvailable(int) is emitted
 * with the number of pending updates.
 */
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(Backend *backend, QObject *parent = nullptr);
    ~UpdateChecker() override;

    /**
     * Starts the periodic update check timer and performs an immediate
     * first check. The interval is read from settings on every start().
     */
    void start();

    /**
     * Stops the periodic update check timer. The checker becomes idle
     * until start() is called again.
     */
    void stop();

    /**
     * Performs an immediate update check regardless of the timer state.
     * Safe to call while a check is already in progress (no-op then).
     *
     * refreshMetadata controls whether this check refreshes the software
     * source (repository metadata): the application startup check passes
     * true, every periodic timer check passes false, so periodic checks
     * reuse the cached metadata instead of re-downloading it.
     */
    void checkNow(bool refreshMetadata = false);

    /**
     * Returns the currently configured check interval in minutes.
     */
    int checkIntervalMinutes() const { return m_intervalMinutes; }

    /**
     * Overrides the check interval in minutes. The new value is applied
     * to the running timer immediately (if started) but is NOT written
     * back to KConfig. Use SettingsDialog to persist a change.
     */
    void setCheckIntervalMinutes(int minutes);

    /**
     * Returns the update count from the last completed check.
     */
    int lastUpdateCount() const { return m_lastCount; }

Q_SIGNALS:
    /**
     * Emitted after every successful check with the number of available
     * updates (zero included).
     */
    void updatesAvailable(int count);

private Q_SLOTS:
    void onUpdatesLoaded(const QList<Miryu::Package> &updates);

private:
    void loadSettings();

    Backend *m_backend;
    QTimer *m_timer;
    int m_intervalMinutes = 60;
    bool m_checking = false;
    int m_lastCount = -1;
};

}
