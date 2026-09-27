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

// Fallback update listing: ask dnf5 itself which packages would upgrade.
// The D-Bus path (dnf5daemon rpm.list scope=upgrades + goal resolve) can come
// back empty when the daemon's in-memory sack is stale or the session is
// unresponsive, even though `dnf5 upgrade` clearly lists updates. Running the
// same dnf5 the user runs and parsing its structured queryformat output
// guarantees the GUI lists exactly what dnf5 detects.
static QList<Package> fetchUpdatesViaCli()
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    // name \t epoch \t version \t release \t arch \t reponame \t downloadsize
    const QString qf = QStringLiteral(
        "%{name}\t%{epoch}\t%{version}\t%{release}\t%{arch}\t%{reponame}\t%{downloadsize}");
    proc.start(QStringLiteral("dnf5"),
               {QStringLiteral("-q"), QStringLiteral("repoquery"),
                QStringLiteral("--upgrades"),
                QStringLiteral("--qf"), qf},
               QIODevice::ReadOnly);
    // repoquery can take a moment on a cold cache; allow up to 2 minutes.
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        proc.waitForFinished(5000);
        return {};
    }
    // dnf5 exits 0 when no upgrades and 100 when upgrades exist; either way we
    // parse stdout.

    QList<Package> updates;
    const QString out = QString::fromUtf8(proc.readAllStandardOutput());
    for (const QString &line : out.split(QStringLiteral("\n"), Qt::SkipEmptyParts)) {
        const QStringList f = line.split(QStringLiteral("\t"));
        if (f.size() < 6)
            continue;
        Package p;
        p.name = f[0].trimmed();
        p.epoch = f[1].trimmed();
        p.version = f[2].trimmed();
        p.release = f[3].trimmed();
        p.arch = f[4].trimmed();
        p.repo = f[5].trimmed();
        if (f.size() >= 7)
            p.size = f[6].trimmed().toLongLong();
        if (p.name.isEmpty())
            continue;
        p.state = PackageState::Update;
        p.calcTodo();
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

bool Backend::refreshMetadata()
{
    // Equivalent to `dnf5 makecache --refresh` (the metadata-refresh stage of
    // `dnf update --refresh`). Running this before querying for updates
    // guarantees the on-disk metadata is current, so a daemon that cached a
    // stale sack at startup will see newly published packages after the
    // resetSession() call in fetchUpdates(). The process runs
    // non-interactively; dnf5 will itself trigger polkit when it needs root
    // to write to the system cache.
    QProcess proc;
    proc.setProgram(QStringLiteral("dnf5"));
    proc.setArguments({QStringLiteral("makecache"), QStringLiteral("--refresh")});
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    // makecache can take a while on a cold cache; allow up to 5 minutes.
    if (!proc.waitForFinished(300000)) {
        qWarning() << "dnf5 makecache --refresh timed out";
        proc.kill();
        proc.waitForFinished(5000);
        return false;
    }
    if (proc.exitCode() != 0) {
        qWarning() << "dnf5 makecache --refresh failed:" << proc.exitCode()
                   << proc.readAllStandardOutput();
        return false;
    }
    return true;
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

    // Refresh repository metadata ONLY once per application run: the first
    // update check (normally fired by UpdateChecker right after startup) or a
    // user-triggered "Refresh Metadata" action. Clicking the Updates tab must
    // NOT re-run `dnf5 makecache --refresh` on every visit — repeated
    // refreshes reload the daemon's repo sack over and over, which grows
    // dnf5daemon-server memory. Note: the flag is set before the call so
    // exactly one automatic refresh happens per run even if it fails (the
    // user can always refresh manually).
    if (!m_metadataRefreshed) {
        m_metadataRefreshed = true;
        refreshMetadata();
    }

    // The dnf5daemon D-Bus probe (rpm.list scope=upgrades + upgrade(@System)
    // + goal.resolve) runs from concurrent QtConcurrent worker threads. On
    // this system a stale/racing session makes every call fail with
    // "Not connected to D-Bus server", triggering a reconnect storm that keeps
    // the probe from ever completing. The authoritative update list is whatever
    // dnf5 itself reports, so we run it as a subprocess and parse its structured
    // queryformat output. This is immune to the D-Bus race and guarantees the
    // GUI lists exactly the updates `dnf5 update --refresh` finds.
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
