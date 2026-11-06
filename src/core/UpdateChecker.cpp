#include "UpdateChecker.h"
#include "Backend.h"
#include "Package.h"

#include <QTimer>
#include <QLoggingCategory>

#include <KSharedConfig>
#include <KConfigGroup>

Q_LOGGING_CATEGORY(MIRYU_UPDATE_CHECKER, "miryu.updatechecker")

namespace Miryu {

// Default check interval in minutes when the config key is missing.
static constexpr int kDefaultCheckInterval = 60;
// KConfig group that stores all Miryu settings.
static const QString kConfigGroup = QStringLiteral("General");

UpdateChecker::UpdateChecker(Backend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_timer(new QTimer(this))
{
    Q_ASSERT_X(backend, "UpdateChecker", "Backend pointer must not be null");

    m_timer->setTimerType(Qt::VeryCoarseTimer);
    m_timer->setSingleShot(false);

    loadSettings();

    // Periodic checks always reuse the metadata cache. A full refresh is
    // reserved for startup when the daemon was initially stopped (see start())
    // and the user's explicit Refresh action.
    connect(m_timer, &QTimer::timeout, this, [this]() {
        checkNow(false);
    });

    // Receive the result of the async loadUpdates() call.
    connect(m_backend, &Backend::updatesLoaded,
            this, &UpdateChecker::onUpdatesLoaded);

    qCDebug(MIRYU_UPDATE_CHECKER) << "UpdateChecker created, interval =" << m_intervalMinutes
                                  << "minutes";
}

UpdateChecker::~UpdateChecker()
{
    stop();
}

void UpdateChecker::loadSettings()
{
    KConfigGroup group = KSharedConfig::openConfig()->group(kConfigGroup);

    m_intervalMinutes = group.readEntry(
        QStringLiteral("updateCheckInterval"), kDefaultCheckInterval);

    // Guard against zero or negative intervals — a zero interval would
    // otherwise cause the QTimer to fire continuously and peg the CPU.
    if (m_intervalMinutes < 1)
        m_intervalMinutes = kDefaultCheckInterval;
}

void UpdateChecker::start()
{
    // Reload settings so that changes made in SettingsDialog are picked up
    // without having to restart the whole application.
    loadSettings();

    if (m_intervalMinutes < 1)
        m_intervalMinutes = kDefaultCheckInterval;

    m_timer->setInterval(m_intervalMinutes * 60 * 1000); // minutes → ms
    m_timer->start();

    qCDebug(MIRYU_UPDATE_CHECKER) << "Timer started, firing every" << m_intervalMinutes << "min";

    // Always check for updates immediately, but force a full metadata refresh
    // only when dnf5daemon-server was not running before this process started.
    // If it was already active, its repository cache is reused instead.
    const bool refreshMetadata = !m_backend->client()->daemonWasRunningAtStartup();
    qCDebug(MIRYU_UPDATE_CHECKER) << "Startup check:"
                                  << (refreshMetadata ? "refreshing metadata" : "using cached metadata");
    QMetaObject::invokeMethod(this, [this, refreshMetadata]() {
        checkNow(refreshMetadata);
    }, Qt::QueuedConnection);
}

void UpdateChecker::stop()
{
    if (m_timer->isActive()) {
        m_timer->stop();
        qCDebug(MIRYU_UPDATE_CHECKER) << "Timer stopped";
    }
}

void UpdateChecker::checkNow(bool refreshMetadata)
{
    if (!m_backend || !m_backend->isInitialized()) {
        qCDebug(MIRYU_UPDATE_CHECKER) << "Backend not initialized, skipping check";
        return;
    }

    // Avoid overlapping checks — the dnf5daemon session is shared and
    // concurrent synchronous D-Bus calls could race.
    if (m_checking) {
        qCDebug(MIRYU_UPDATE_CHECKER) << "A check is already in progress, skipping";
        return;
    }

    m_checking = true;
    qCDebug(MIRYU_UPDATE_CHECKER) << "Starting update check"
                                  << (refreshMetadata ? "(refreshing metadata)" : "(using cached metadata)");
    m_backend->loadUpdates(refreshMetadata);
}

void UpdateChecker::onUpdatesLoaded(const QList<Miryu::Package> &updates)
{
    // The Backend emits updatesLoaded for every loadUpdates() call, including
    // those triggered by the MainWindow. We always process the result here
    // so that the signal count stays fresh even when the user clicks
    // "Updates" in the sidebar.
    m_checking = false;

    const int count = updates.size();
    qCDebug(MIRYU_UPDATE_CHECKER) << "Update check finished, count =" << count;

    m_lastCount = count;
    Q_EMIT updatesAvailable(count);
}

void UpdateChecker::setCheckIntervalMinutes(int minutes)
{
    if (minutes < 1)
        minutes = kDefaultCheckInterval;

    m_intervalMinutes = minutes;

    if (m_timer->isActive()) {
        m_timer->setInterval(m_intervalMinutes * 60 * 1000);
        // Restart so the new interval takes effect from now.
        m_timer->start();
    }
}

}
