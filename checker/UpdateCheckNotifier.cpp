#include "UpdateCheckNotifier.h"

#include "NotifierItem.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KNotification>
#include <KSharedConfig>

namespace Miryu {

// The state file remembers when the last "updates available" notification
// was shown, so the notification frequency configured in Miryu Software
// Center (daily / weekly / monthly) is honoured across sessions. Mirrored
// after Discover's discovernotifierstaterc (notifier/DiscoverNotifier.cpp).
static const QString kStateFileName = QStringLiteral("miryuchecknotifierrc");
static const QString kStateGroup = QStringLiteral("Global");
static const QString kLastNotificationKey = QStringLiteral("LastNotificationTime");

// Group of the shared Miryu configuration (the same file and group the
// software center itself uses — the main application runs with
// applicationName "miryu", hence the "miryurc" file name).
static const QString kMiryuConfigFile = QStringLiteral("miryurc");
static const QString kConfigGroup = QStringLiteral("General");
static const QString kNotifyFrequencyKey = QStringLiteral("updateNotifyFrequency");

// Safety timeout for both probes. `dnf5 check-update --refresh` and
// `flatpak update` both (re)download repository metadata, which can take a
// couple of minutes on a slow link; ten minutes is generous enough.
static constexpr int kProbeTimeoutMs = 10 * 60 * 1000;

UpdateCheckNotifier::UpdateCheckNotifier(QObject *parent)
    : QObject(parent)
{
}

UpdateCheckNotifier::~UpdateCheckNotifier()
{
    for (QProcess *p : {m_rpmProcess, m_flatpakProcess}) {
        if (p && p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
}

QString UpdateCheckNotifier::notificationIntervalSetting()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(kMiryuConfigFile);
    KConfigGroup group(config, kConfigGroup);
    const QString value = group.readEntry(kNotifyFrequencyKey, QStringLiteral("daily")).toLower();
    if (value == QLatin1String("weekly") || value == QLatin1String("monthly") ||
        value == QLatin1String("never"))
        return value;
    return QStringLiteral("daily");
}

QDateTime UpdateCheckNotifier::lastNotificationTime()
{
    KConfig stateConfig(kStateFileName, KConfig::SimpleConfig,
                        QStandardPaths::GenericStateLocation);
    KConfigGroup group(&stateConfig, kStateGroup);
    return group.readEntry(kLastNotificationKey, QDateTime());
}

bool UpdateCheckNotifier::shouldRunCheckNow()
{
    // "不通知" — the user opted out of update notifications entirely: do not
    // even probe the system (dnf5 --refresh / flatpak update cost network
    // traffic), the in-app checker of the software center remains available.
    const QString frequency = notificationIntervalSetting();
    if (frequency == QLatin1String("never"))
        return false;

    const QDateTime last = lastNotificationTime();
    if (!last.isValid())
        return true;

    qint64 intervalSeconds = 24 * 3600;                       // daily (default)
    if (frequency == QLatin1String("weekly"))
        intervalSeconds = 7 * 24 * 3600;
    else if (frequency == QLatin1String("monthly"))
        intervalSeconds = 30 * 24 * 3600;

    // Too soon since the previous notification — stay quiet for now.
    return last.addSecs(intervalSeconds) <= QDateTime::currentDateTimeUtc();
}

void UpdateCheckNotifier::start()
{
    runRpmProbe();
    runFlatpakProbe();

    // Watchdog: if a probe hangs (e.g. unreachable mirrors), treat it as
    // failed after the timeout and continue with whatever finished.
    QTimer *watchdog = new QTimer(this);
    watchdog->setSingleShot(true);
    connect(watchdog, &QTimer::timeout, this, [this]() {
        qWarning() << "Update probes timed out after" << (kProbeTimeoutMs / 1000) << "s";
        for (QProcess *p : {m_rpmProcess, m_flatpakProcess}) {
            if (p && p->state() != QProcess::NotRunning) {
                p->kill();
            }
        }
    });
    watchdog->start(kProbeTimeoutMs);
}

// ---------------------------------------------------------------------------
// Probes
// ---------------------------------------------------------------------------

static QProcessEnvironment probeEnvironment()
{
    // Force a C locale so both tools emit stable, parseable table output
    // (same trick FlatpakBackend uses for the GUI's flatpak invocations).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    return env;
}

void UpdateCheckNotifier::runRpmProbe()
{
    // Required check command: `dnf5 check-update --refresh`.
    // Exit code 100 = updates available, 0 = system up to date.
    m_rpmProcess = new QProcess(this);
    m_rpmProcess->setProcessEnvironment(probeEnvironment());

    // Handle startup failures (dnf5 not installed) — otherwise the probe
    // would never finish and the checker would hang forever.
    connect(m_rpmProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning() << "dnf5 could not be started (not installed?)";
            m_rpmResult.count = -1;
            m_rpmResult.done = true;
            evaluateResults();
        }
    });

    connect(m_rpmProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdateCheckNotifier::onProbeFinished);

    m_rpmProcess->start(QStringLiteral("dnf5"), QStringList{
        QStringLiteral("check-update"),
        QStringLiteral("--refresh"),
    });
}

void UpdateCheckNotifier::runFlatpakProbe()
{
    // Required check command: plain `flatpak update`. Run without a TTY:
    // flatpak prints the numbered update summary and then automatically
    // answers "n" to its own "Proceed with these changes …?" prompt
    // (flatpak_yes_no_prompt() returns FALSE when stdin/stdout is not a
    // terminal), so this probe is strictly read-only.
    m_flatpakProcess = new QProcess(this);
    m_flatpakProcess->setProcessEnvironment(probeEnvironment());

    connect(m_flatpakProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            qWarning() << "flatpak could not be started (not installed?)";
            m_flatpakResult.count = -1;
            m_flatpakResult.done = true;
            evaluateResults();
        }
    });

    connect(m_flatpakProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdateCheckNotifier::onProbeFinished);

    m_flatpakProcess->start(QStringLiteral("flatpak"), QStringList{
        QStringLiteral("update"),
    });
}

void UpdateCheckNotifier::onProbeFinished()
{
    auto *process = qobject_cast<QProcess *>(sender());
    if (!process)
        return;

    const QByteArray stdoutData = process->readAllStandardOutput();

    if (process == m_rpmProcess) {
        const int exitCode = process->exitCode();
        if (process->exitStatus() == QProcess::NormalExit && (exitCode == 0 || exitCode == 100)) {
            m_rpmResult.count = (exitCode == 100)
                ? parseDnf5CheckUpdateOutput(stdoutData)
                : 0;
            qInfo() << "dnf5 check-update finished, exit" << exitCode
                    << "updates:" << m_rpmResult.count;
        } else {
            qWarning() << "dnf5 check-update failed, exit" << exitCode
                       << QString::fromUtf8(process->readAllStandardError()).trimmed();
            m_rpmResult.count = -1;
        }
        m_rpmResult.done = true;
    } else if (process == m_flatpakProcess) {
        const int rows = parseFlatpakUpdateOutput(stdoutData);
        const int exitCode = process->exitCode();
        if (process->exitStatus() != QProcess::NormalExit) {
            m_flatpakResult.count = -1;
        } else if (exitCode == 0 || rows > 0) {
            // Exit 0 = "Nothing to do." (0 updates). A non-zero exit is also
            // the EXPECTED path when updates were listed: flatpak aborts the
            // transaction after answering "n" to its proceed prompt, in which
            // case the already-printed row count is authoritative.
            m_flatpakResult.count = rows;
        } else {
            qWarning() << "flatpak update failed, exit" << exitCode
                       << QString::fromUtf8(process->readAllStandardError()).trimmed();
            m_flatpakResult.count = -1;
        }
        m_flatpakResult.done = true;
    }

    evaluateResults();
}

void UpdateCheckNotifier::evaluateResults()
{
    if (!m_rpmResult.done || !m_flatpakResult.done)
        return;

    const int rpmCount = qMax(m_rpmResult.count, 0);
    const int flatpakCount = qMax(m_flatpakResult.count, 0);

    qInfo() << "Update check complete: rpm =" << rpmCount << "flatpak =" << flatpakCount;

    if (rpmCount > 0 || flatpakCount > 0) {
        notifyUpdates(rpmCount, flatpakCount);
        // Do not emit finished() yet: the process (and its tray entry) stays
        // resident while updates are pending. It exits when the user engages
        // (open center / start updates / notification action) or quits via
        // the tray menu.
    } else {
        Q_EMIT finished();
    }
}

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

void UpdateCheckNotifier::notifyUpdates(int rpmCount, int flatpakCount)
{
    QString message;
    if (rpmCount > 0 && flatpakCount > 0) {
        message = i18n("%1 RPM package(s) and/or %2 Flatpak package(s) available for update",
                       rpmCount, flatpakCount);
    } else if (rpmCount > 0) {
        message = i18n("%1 RPM package(s) available for update", rpmCount);
    } else {
        message = i18n("%1 Flatpak package(s) available for update", flatpakCount);
    }

    qInfo() << "Showing update notification:" << message;

    // Remember "now" as the last notification time so the configured
    // notification frequency (daily / weekly / monthly) is honoured —
    // same approach as DiscoverNotifier::notifyAboutUpdates().
    {
        KConfig stateConfig(kStateFileName, KConfig::SimpleConfig,
                            QStandardPaths::GenericStateLocation);
        KConfigGroup group(&stateConfig, kStateGroup);
        group.writeEntry(kLastNotificationKey, QDateTime::currentDateTimeUtc());
        group.sync();
    }

    m_notification = KNotification::event(
        QStringLiteral("Update"),
        message,
        QString(),
        QStringLiteral("system-software-update"),
        KNotification::CloseOnTimeout,
        QStringLiteral("miryu-software-check"));
    // Keep the notification around (with its action button) until the user
    // interacts with it instead of letting the idle timeout discard it.
    m_notification->setHint(QStringLiteral("resident"), true);

    const QString viewAction = i18n("View Updates");
    const auto showUpdates = [this]() {
        const QString token =
            m_notification ? m_notification->xdgActivationToken() : QString();
        NotifierItem::launchSoftwareCenter(token, NotifierItem::LaunchMode::ShowUpdates);
        // The user engaged — give the async launch job a moment to flush its
        // DBus message, then take the tray entry (and this process) down.
        QTimer::singleShot(500, this, &UpdateCheckNotifier::finish);
    };

    if (auto *defaultAction = m_notification->addDefaultAction(viewAction))
        connect(defaultAction, &KNotificationAction::activated, this, showUpdates);
    if (auto *action = m_notification->addAction(viewAction))
        connect(action, &KNotificationAction::activated, this, showUpdates);

    // Keep the process (and its tray entry) alive while updates are pending.
    // The tray item offers: open the software center / start the updates /
    // quit. It only goes away when the user engages or quits explicitly.
    m_trayItem = new NotifierItem(message, this);
    connect(m_trayItem, &NotifierItem::quitRequested, this, &UpdateCheckNotifier::finish);
}

void UpdateCheckNotifier::finish()
{
    Q_EMIT finished();
}

// ---------------------------------------------------------------------------
// Output parsing
// ---------------------------------------------------------------------------

QByteArray UpdateCheckNotifier::stripAnsiEscapes(const QByteArray &data)
{
    // Defensive: both tools colourise their output on a TTY; a QProcess pipe
    // normally disables colours, but strip any escape sequence anyway.
    static const QRegularExpression ansiPattern(QStringLiteral("\x1B\\[[0-9;]*[A-Za-z]"));
    QString text = QString::fromUtf8(data);
    text.remove(ansiPattern);
    return text.toUtf8();
}

/**
 * Counts the update rows printed by `dnf5 check-update --refresh`.
 *
 * Output layout (libdnf5-cli/output/package_list_sections.cpp): section
 * headings ("Upgrades", "Obsoleting packages") on their own line, followed
 * by unnumbered rows "name.arch  evr  repository"; obsoleted packages are
 * indented by four spaces. A leading "Last metadata expiration check: …"
 * banner may precede the table.
 */
int UpdateCheckNotifier::parseDnf5CheckUpdateOutput(const QByteArray &output)
{
    const QStringList lines = QString::fromUtf8(stripAnsiEscapes(output))
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    int count = 0;
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QStringLiteral("Last metadata expiration")))
            continue;
        if (line == QLatin1String("Upgrades") || line.startsWith(QLatin1String("Obsoleting packages")))
            continue;

        const QStringList columns = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        // A package row has at least three columns: name.arch, evr, repo.
        if (columns.size() < 3)
            continue;
        // name.arch tokens always contain a dot; the evr column starts with
        // a digit (optionally "epoch:" prefixed). Headings never match both.
        if (!columns.at(0).contains(QLatin1Char('.')))
            continue;
        const QString evr = columns.at(1);
        if (!evr.isEmpty() && evr.at(0).isDigit())
            ++count;
    }
    return count;
}

/**
 * Counts the numbered rows of the summary table printed by `flatpak update`,
 * e.g.:
 *
 *   Update summary:
 *       1.   Blender   org.blender.Blender   x86_64   stable   u   flathub   250.4 MB
 *       2.   GIMP      org.gimp.GIMP         x86_64   stable   u   flathub   120.1 MB
 *
 * Rows always start with "<n>." regardless of flatpak version; with several
 * installations flatpak prints one table per installation, which the row
 * numbering covers as well.
 */
int UpdateCheckNotifier::parseFlatpakUpdateOutput(const QByteArray &output)
{
    static const QRegularExpression rowPattern(QStringLiteral("^\\s*\\d+\\.\\s"));

    const QStringList lines = QString::fromUtf8(stripAnsiEscapes(output))
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    int count = 0;
    for (const QString &line : lines) {
        if (rowPattern.match(line).hasMatch())
            ++count;
    }
    return count;
}

}
