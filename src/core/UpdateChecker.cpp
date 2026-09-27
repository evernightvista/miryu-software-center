#include "UpdateChecker.h"
#include "Backend.h"
#include "Package.h"

#include <QTimer>
#include <QLoggingCategory>

#include <KSharedConfig>
#include <KConfigGroup>
#include <KNotification>
#include <KLocalizedString>

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

    // Trigger a check whenever the timer fires.
    connect(m_timer, &QTimer::timeout, this, &UpdateChecker::checkNow);

    // Receive the result of the async loadUpdates() call.
    connect(m_backend, &Backend::updatesLoaded,
            this, &UpdateChecker::onUpdatesLoaded);

    qCDebug(MIRYU_UPDATE_CHECKER) << "UpdateChecker created, interval =" << m_intervalMinutes
                                  << "minutes, notifications =" << m_notificationsEnabled;
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

    m_notificationsEnabled = group.readEntry(
        QStringLiteral("updateNotificationsEnabled"), true);
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
    // for the full interval to pass before seeing results.
    QMetaObject::invokeMethod(this, &UpdateChecker::checkNow, Qt::QueuedConnection);
}

void UpdateChecker::stop()
{
    if (m_timer->isActive()) {
        m_timer->stop();
        qCDebug(MIRYU_UPDATE_CHECKER) << "Timer stopped";
    }
}

void UpdateChecker::checkNow()
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
    qCDebug(MIRYU_UPDATE_CHECKER) << "Starting update check";
    m_backend->loadUpdates();
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

    if (m_notificationsEnabled && count > 0)
        showNotification(count);
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

void UpdateChecker::setNotificationsEnabled(bool enabled)
{
    m_notificationsEnabled = enabled;
}

void UpdateChecker::showNotification(int count)
{
    // Using the instance-based API rather than the deprecated static
    // KNotification::event() so that the NotificationFlags can be set
    // explicitly and the object lifetime is controlled by "this".
    auto *notification = new KNotification(
        QStringLiteral("updatesAvailable"),
        KNotification::CloseOnTimeout,
        this);

    notification->setTitle(i18n("Updates Available"));
    notification->setText(i18np("%1 update is available for your system.",
                                "%1 updates are available for your system.",
                                count));
    notification->setIconName(QStringLiteral("system-software-update"));

    // Allow the notification to appear in the system tray / notification
    // daemon even when the main window is hidden.
    notification->setFlags(KNotification::CloseOnTimeout);

    notification->sendEvent();

    qCDebug(MIRYU_UPDATE_CHECKER) << "Notification sent for" << count << "updates";
}

}
