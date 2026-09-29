#include "Backend.h"

#include <QtConcurrent>
#include <QDebug>
#include <QMetaType>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>

#include <KLocalizedString>

namespace Miryu {

// Parse the EVR column ("[epoch:]version-release") into its components.
// dnf5 prints the epoch prefix only when it is non-zero.
static void splitEvr(const QString &evr, QString &epoch, QString &version, QString &release)
{
    QString vrel = evr;
    epoch.clear();
    version.clear();
    release.clear();

    const int colon = evr.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        epoch = evr.left(colon);
        vrel = evr.mid(colon + 1);
    }
    const int dash = vrel.lastIndexOf(QLatin1Char('-'));
    if (dash >= 0) {
        version = vrel.left(dash);
        release = vrel.mid(dash + 1);
    } else {
        version = vrel;
    }
    if (epoch.isEmpty() || epoch == QStringLiteral("0"))
        epoch = QStringLiteral("0");
}

// Parse a humanized size token ("25 k", "1.5 M", "0 B") into bytes. Handles a
// glued "25k" form as well; returns 0 when the token is not a size.
static qint64 parseHumanSize(const QString &numToken, const QString &unitToken)
{
    QString num = numToken.trimmed();
    QString unit = unitToken.trimmed();
    if (unit.isEmpty()) {
        // Glued form like "25k": split off the trailing unit letters.
        int i = num.size();
        while (i > 0 && num.at(i - 1).isLetter())
            --i;
        if (i < num.size()) {
            unit = num.mid(i);
            num = num.left(i);
        }
    }
    bool ok = false;
    const double value = num.toDouble(&ok);
    if (!ok || value < 0)
        return 0;

    qint64 factor = 1;
    if (unit == QStringLiteral("k") || unit == QStringLiteral("K") ||
        unit == QStringLiteral("KB") || unit == QStringLiteral("KiB")) {
        factor = 1024;
    } else if (unit == QStringLiteral("M") || unit == QStringLiteral("MB") ||
               unit == QStringLiteral("MiB")) {
        factor = 1024LL * 1024;
    } else if (unit == QStringLiteral("G") || unit == QStringLiteral("GB") ||
               unit == QStringLiteral("GiB")) {
        factor = 1024LL * 1024 * 1024;
    }
    return static_cast<qint64>(value * factor);
}

// Update listing: run `dnf5 update --refresh` in dry-run mode (--assumeno) and
// parse the planned transaction. Running the exact command the user runs makes
// the GUI list precisely what dnf5 would update: metadata is refreshed on every
// check, and obsoletes / replacements / new dependencies are included, which a
// bare `dnf5 repoquery --upgrades` (without --refresh) misses. --assumeno
// answers "no" to the confirmation prompt, so nothing is downloaded or applied.
// The D-Bus path (dnf5daemon rpm.list scope=upgrades + goal resolve) is not
// used here: it can come back empty when the daemon's in-memory sack is stale
// or the session is unresponsive, even though `dnf5 update --refresh` clearly
// lists updates.
static QList<Package> fetchUpdatesViaCli()
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    // Keep the transaction table on single lines when stdout is a pipe.
    env.insert(QStringLiteral("DNF5_FORCE_COLUMNS"), QStringLiteral("200"));
    proc.setProcessEnvironment(env);

    proc.start(QStringLiteral("dnf5"),
               {QStringLiteral("update"),
                QStringLiteral("--refresh"),
                QStringLiteral("--assumeno")},
               QIODevice::ReadOnly);
    // Metadata refresh on a cold cache can take a while; allow up to 5 minutes.
    if (!proc.waitForFinished(300000)) {
        proc.kill();
        proc.waitForFinished(5000);
        qWarning() << "dnf5 update --refresh timed out";
        return {};
    }

    // The exit code is not authoritative: 0 = nothing to do, 1 = the dry-run
    // transaction was aborted (updates exist), other non-zero = real error.
    // Errors leave stdout without package rows, so parsing stdout is enough.
    QList<Package> updates;
    QSet<QString> seen; // de-duplicate by NEVRA
    QString section;    // current transaction section ("Installing:", ...)

    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &rawLine : out.split(QStringLiteral("\n"))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        // Section headers ("Installing:", "Upgrading:", "Obsoleting:", ...)
        // end with ':' and are short; every following row belongs to that
        // section until the next header.
        if (line.endsWith(QLatin1Char(':')) && line.size() < 40) {
            section = line;
            continue;
        }

        // A package row: name  arch  evr  repo  [size unit]. The EVR column
        // always contains '-', which filters out the column header
        // ("Package Arch Version Repository Size") and summary lines. RPM
        // names/archs never contain ':', so a leading "Word:" token (e.g. a
        // warning line that reached stdout) is rejected as well.
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() < 4 || !f[2].contains(QLatin1Char('-')))
            continue;
        if (f[0].endsWith(QLatin1Char(':')) || f[1].endsWith(QLatin1Char(':')))
            continue;
        // Removals are consequences of the update, not updates the user can
        // queue; dnf5 prints them in the "Removing:" section.
        if (section.startsWith(QStringLiteral("Removing"), Qt::CaseInsensitive))
            continue;

        Package p;
        p.name = f[0];
        p.arch = f[1];
        splitEvr(f[2], p.epoch, p.version, p.release);
        p.repo = f[3];
        if (f.size() >= 6)
            p.size = parseHumanSize(f[4], f[5]);
        else if (f.size() >= 5)
            p.size = parseHumanSize(f[4], QString());

        // Map dnf5's own section to the queue action: "Installing:" rows are
        // packages that are not installed yet, "Downgrading:" rows are
        // downgrades, and everything else (Upgrading / Obsoleting / Replacing)
        // is a regular update.
        if (section.startsWith(QStringLiteral("Installing"), Qt::CaseInsensitive))
            p.state = PackageState::Available;
        else if (section.startsWith(QStringLiteral("Downgrading"), Qt::CaseInsensitive))
            p.state = PackageState::Downgrade;
        else
            p.state = PackageState::Update;
        p.calcTodo();

        if (p.name.isEmpty() || p.repo.isEmpty())
            continue;
        if (seen.contains(p.nevra()))
            continue;
        seen.insert(p.nevra());
        updates.append(p);
    }
    return updates;
}

Backend::Backend(QObject *parent)
    : QObject(parent)
    , m_client(new Dnf5DaemonClient(this))
    , m_transactionManager(new TransactionManager(m_client, this))
    , m_cache(new PackageCache(this))
    , m_lastError()
{
    qRegisterMetaType<Package>("Miryu::Package");
    qRegisterMetaType<Repository>("Miryu::Repository");
    qRegisterMetaType<TransactionResult>("Miryu::TransactionResult");
    qRegisterMetaType<PackageFilter>("Miryu::PackageFilter");
    qRegisterMetaType<SearchField>("Miryu::SearchField");
    qRegisterMetaType<QList<Package>>("QList<Miryu::Package>");
    qRegisterMetaType<QList<Repository>>("QList<Miryu::Repository>");

    // Capture the actual D-Bus error from the client so initialize()
    // can report it to the caller instead of a generic message.
    connect(m_client, &Dnf5DaemonClient::errorOccurred, this, [this](const QString &err) {
        m_lastError = err;
    });

    connectClientSignals();
}

Backend::~Backend()
{
    // Disconnect Qt signal connections from Dnf5DaemonClient to this
    // Backend BEFORE closing the session.  This prevents any in-flight
    // D-Bus signal (already dispatched but not yet delivered) from
    // reaching our lambda handlers during teardown.
    if (m_client)
        m_client->disconnect(this);

    if (m_client->isSessionOpen())
        m_client->closeSession();
}

void Backend::connectClientSignals()
{
    connect(m_client, &Dnf5DaemonClient::downloadAddNew, this, [this](const QString &, const QString &desc, qint64 total) {
        Q_EMIT downloadProgress(desc, total, 0);
    });
    connect(m_client, &Dnf5DaemonClient::downloadProgress, this, [this](const QString &, qint64 total, qint64 downloaded) {
        int percent = total > 0 ? static_cast<int>(downloaded * 100 / total) : 0;
        Q_EMIT transactionProgress(i18n("Downloading"), percent);
    });
    connect(m_client, &Dnf5DaemonClient::downloadEnd, this, [this](const QString &, uint status, const QString &) {
        if (status == 0)
            Q_EMIT transactionProgress(i18n("Download complete"), 100);
    });

    connect(m_client, &Dnf5DaemonClient::transactionActionStart, this, [this](const QString &nevra, uint action, quint64 total) {
        Q_UNUSED(total)
        QString actionStr = actionToString(static_cast<TransactionActionType>(action));
        Q_EMIT transactionProgress(i18n("%1 %2", actionStr, nevra), 0);
    });
    connect(m_client, &Dnf5DaemonClient::transactionActionProgress, this, [this](const QString &nevra, quint64 processed, quint64 total) {
        int percent = total > 0 ? static_cast<int>(processed * 100 / total) : 0;
        Q_EMIT transactionProgress(i18n("Processing %1", nevra), percent);
    });
    connect(m_client, &Dnf5DaemonClient::transactionVerifyStart, this, [this](quint64) {
        Q_EMIT transactionProgress(i18n("Verifying Packages"), 0);
    });
    connect(m_client, &Dnf5DaemonClient::transactionVerifyProgress, this, [this](quint64 processed, quint64 total) {
        int percent = total > 0 ? static_cast<int>(processed * 100 / total) : 0;
        Q_EMIT transactionProgress(i18n("Verifying"), percent);
    });
    connect(m_client, &Dnf5DaemonClient::transactionBeforeBegin, this, [this](quint64) {
        Q_EMIT transactionProgress(i18n("Applying Transaction"), 0);
    });
    connect(m_client, &Dnf5DaemonClient::transactionScriptStart, this, [this](const QString &, uint) {
        Q_EMIT transactionProgress(i18n("Running scripts"), 0);
    });

    connect(m_client, &Dnf5DaemonClient::errorOccurred, this, &Backend::errorOccurred);
    connect(m_client, &Dnf5DaemonClient::repoKeyImportRequest, this, [this](const QString &keyId, const QStringList &userIds,
                                                                             const QString &fingerprint, const QString &url, qint64) {
        qInfo() << "Repository key import request:" << keyId << userIds << fingerprint << url;
    });
}

bool Backend::initialize()
{
    if (m_initialized)
        return true;

    m_lastError.clear();

    if (!m_client->isConnected()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot connect to system D-Bus. "
                                            "Is the D-Bus system bus running?"));
        return false;
    }

    if (!m_client->openSession()) {
        // m_lastError was set by the errorOccurred signal handler above.
        // Pass the actual D-Bus error through instead of a generic message.
        QString err = m_lastError.isEmpty()
            ? QStringLiteral("Failed to open dnf5daemon session (no error details available)")
            : m_lastError;
        qWarning() << "Backend::initialize: openSession failed:" << err;
        Q_EMIT errorOccurred(err);
        return false;
    }

    m_transactionManager->loadRepositories();

    m_initialized = true;
    Q_EMIT initialized();
    return true;
}

QList<Package> Backend::fetchPackages(PackageFilter filter)
{
    QStringList attrs = {
        QStringLiteral("name"), QStringLiteral("evr"), QStringLiteral("arch"),
        QStringLiteral("repo_id"), QStringLiteral("summary"), QStringLiteral("install_size"),
        QStringLiteral("is_installed"), QStringLiteral("description"), QStringLiteral("url"),
        QStringLiteral("license"), QStringLiteral("version"), QStringLiteral("release"),
        QStringLiteral("epoch")
    };

    QList<Package> packages = m_client->packageList({QStringLiteral("*")}, attrs, filter);
    return packages;
}

QList<Package> Backend::fetchSearchResults(const QString &query, SearchField field, PackageFilter scope)
{
    QStringList attrs = {
        QStringLiteral("name"), QStringLiteral("evr"), QStringLiteral("arch"),
        QStringLiteral("repo_id"), QStringLiteral("summary"), QStringLiteral("install_size"),
        QStringLiteral("is_installed"), QStringLiteral("description"), QStringLiteral("url"),
        QStringLiteral("license"), QStringLiteral("version"), QStringLiteral("release"),
        QStringLiteral("epoch")
    };

    QVariantMap extra;
    extra[QStringLiteral("latest_limit")] = 0;

    // Use fuzzy glob patterns so that searching e.g. "firefox" matches
    // "*firefox*" instead of requiring an exact package name match.
    QStringList patterns;
    if (query.isEmpty()) {
        patterns = QStringList{QStringLiteral("*")};
    } else {
        // Build a glob pattern that matches any package whose name contains
        // the query string (case-insensitive on the daemon side).
        patterns = QStringList{QStringLiteral("*") + query + QStringLiteral("*")};
    }

    QList<Package> packages = m_client->packageList(patterns, attrs, scope, extra);

    // When a specific field is requested, the glob pattern above already
    // narrows the results on the daemon side; we still post-filter in memory
    // to ensure the match is in the requested field (case-insensitive).
    if (field != SearchField::All) {
        QList<Package> filtered;
        for (const auto &pkg : packages) {
            bool match = false;
            switch (field) {
            case SearchField::Name:
                match = pkg.name.contains(query, Qt::CaseInsensitive);
                break;
            case SearchField::Summary:
                match = pkg.summary.contains(query, Qt::CaseInsensitive);
                break;
            case SearchField::Description:
                match = pkg.description.contains(query, Qt::CaseInsensitive);
                break;
            default:
                match = true;
                break;
            }
            if (match)
                filtered.append(pkg);
        }
        return filtered;
    }

    return packages;
}

QList<Repository> Backend::fetchRepositories()
{
    return m_client->repoList();
}

QList<Package> Backend::fetchUpdates()
{
    // fetchUpdates() runs on a QtConcurrent worker thread, typically right
    // after a transaction completes (onRefresh → loadUpdates). At that point
    // the dnf5daemon / system bus may be momentarily unstable (e.g. the
    // transaction just upgraded dbus, dnf5daemon-server or systemd). Any
    // D-Bus error raised by the client during this background probe would
    // otherwise be forwarded (errorOccurred → Backend::errorOccurred →
    // MainWindow::onError → KMessageBox) and pop a scary "Not connected to
    // D-Bus server" dialog right after a successful update. Block all
    // signals on the client for the duration of the probe so transient
    // errors stay silent; the caller surfaces the result via
    // updatesLoaded() (an empty list on failure) instead.
    const bool wasBlocked = m_client->blockSignals(true);

    // The authoritative update list is whatever `dnf5 update --refresh` itself
    // reports: run it as a subprocess (dry-run, see fetchUpdatesViaCli()) and
    // parse its planned transaction. The command refreshes repository metadata
    // on every check — exactly like `dnf5 update --refresh` in a terminal — so
    // no separate makecache pass is needed, and it is immune to the
    // dnf5daemon D-Bus race, so the GUI lists exactly the updates dnf5 detects.
    QList<Package> result = fetchUpdatesViaCli();

    // Restore signal delivery — real transactions and user-initiated
    // operations after this point must still be able to surface errors.
    m_client->blockSignals(wasBlocked);

    return result;
}

void Backend::loadPackages(PackageFilter filter, bool reset)
{
    if (!reset && m_cache->hasFilter(filter)) {
        Q_EMIT packagesLoaded(filter, m_cache->getPackages(filter));
        return;
    }

    auto *watcher = new QFutureWatcher<QList<Package>>(this);
    connect(watcher, &QFutureWatcher<QList<Package>>::finished, this, [this, watcher, filter]() {
        QList<Package> packages = watcher->result();
        m_cache->setPackages(filter, packages);
        Q_EMIT packagesLoaded(filter, packages);
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this, filter]() {
        return fetchPackages(filter);
    }));
}

void Backend::searchPackages(const QString &query, SearchField field, PackageFilter scope)
{
    auto *watcher = new QFutureWatcher<QList<Package>>(this);
    connect(watcher, &QFutureWatcher<QList<Package>>::finished, this, [this, watcher]() {
        QList<Package> packages = watcher->result();
        Q_EMIT searchCompleted(packages);
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this, query, field, scope]() {
        return fetchSearchResults(query, field, scope);
    }));
}

void Backend::loadRepositories()
{
    auto *watcher = new QFutureWatcher<QList<Repository>>(this);
    connect(watcher, &QFutureWatcher<QList<Repository>>::finished, this, [this, watcher]() {
        QList<Repository> repos = watcher->result();
        Q_EMIT repositoriesLoaded(repos);
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        return fetchRepositories();
    }));
}

void Backend::loadUpdates()
{
    auto *watcher = new QFutureWatcher<QList<Package>>(this);
    connect(watcher, &QFutureWatcher<QList<Package>>::finished, this, [this, watcher]() {
        QList<Package> updates = watcher->result();
        Q_EMIT updatesLoaded(updates);
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        return fetchUpdates();
    }));
}

TransactionResult Backend::buildTransaction(const QList<Package> &packages, const TransactionOptions &opts)
{
    return m_transactionManager->buildTransaction(packages, opts);
}

TransactionResult Backend::runTransaction(const TransactionOptions &opts)
{
    return m_transactionManager->runTransaction(opts);
}

TransactionResult Backend::depsolve(const QList<Package> &packages)
{
    return m_transactionManager->depsolve(packages);
}

}
