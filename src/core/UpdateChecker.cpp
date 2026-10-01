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

    // Trigger a check whenever the timer fires. Periodic checks MUST NOT
    // refresh the software source: per the desired behaviour the repository
    // metadata is refreshed only at application startup (see start()), so
    // every periodic tick reuses the metadata cache instead of
    // re-downloading it. Applying an upgrade queue also reuses the cache
    // (the displayed update list already came from it).
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

    // Perform an immediate first check so the user doesn't have to wait
    // for the full interval to pass before seeing results. This startup
    // check is the only automatic moment the software source is refreshed
    // (repository metadata) — Backend::fetchUpdates() calls readAllRepos()
    // which only re-downloads metadata for repos whose cache has expired.
    // Applying an update queue does NOT refresh metadata: it resolves from
    // the same daemon cache that produced the update list.
    QMetaObject::invokeMethod(this, [this]() {
        checkNow(true);
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
