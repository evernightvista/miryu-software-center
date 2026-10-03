#include "Backend.h"

#include <QtConcurrent>
#include <QDebug>
#include <QMetaType>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>
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

// Parse the JSON output of `dnf5 check-upgrade --json`. The document is an
// object whose keys are output sections ("Upgrading packages", "Obsoleting
// packages", …) and whose values are arrays of package objects with name,
// arch, evr, repository (and optionally obsoletes). This is far more robust
// than parsing the text columns, whose layout changes across dnf5 versions.
static QList<Package> parseCheckUpgradeJson(const QString &json)
{
    QList<Package> updates;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return updates;

    QSet<QString> seen;
    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!it.value().isArray())
            continue;
        const QJsonArray pkgs = it.value().toArray();
        for (const QJsonValue &v : pkgs) {
            if (!v.isObject())
                continue;
            const QJsonObject obj = v.toObject();

            Package p;
            p.name = obj.value(QStringLiteral("name")).toString();
            p.arch = obj.value(QStringLiteral("arch")).toString();
            splitEvr(obj.value(QStringLiteral("evr")).toString(),
                     p.epoch, p.version, p.release);
            p.repo = obj.value(QStringLiteral("repository")).toString();
            p.state = PackageState::Update;
            p.calcTodo();

            if (p.name.isEmpty() || p.repo.isEmpty())
                continue;
            if (seen.contains(p.nevra()))
                continue;
            seen.insert(p.nevra());
            updates.append(p);
        }
    }
    return updates;
}

// Parse the classic text output of `dnf check-update` / `dnf check-upgrade`
// (name.arch  evr  repo columns). Used as a fallback for dnf4 or older dnf5
// builds that do not support --json.
static QList<Package> parseCheckUpgradeText(const QString &out)
{
    QList<Package> updates;
    QSet<QString> seen;
    for (const QString &rawLine : out.split(QStringLiteral("\n"))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        // A package row: name.arch  evr  repo [advisory...]. The evr column
        // always contains '-' ("version-release"), which filters out the
        // "Last metadata expiration check:" line and summary lines.
        const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() < 3 || !f[1].contains(QLatin1Char('-')))
            continue;

        // name and arch are joined by '.' in the text output; split on the
        // LAST dot so package names containing dots (e.g. python3.11) are
        // preserved.
        const int dot = f[0].lastIndexOf(QLatin1Char('.'));
        if (dot < 1 || dot >= f[0].size() - 1)
            continue;

        Package p;
        p.name = f[0].left(dot);
        p.arch = f[0].mid(dot + 1);
        splitEvr(f[1], p.epoch, p.version, p.release);
        p.repo = f[2];

        // Every row printed is an available update to an installed package.
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

// Update listing (cross-check source): run `dnf5 check-upgrade` (or the dnf4
// `check-update` shim) and parse its output.
//
// The command lists every installed package that has a newer version
// available in a repo, with exit code semantics (0 = no updates, 100 =
// updates available, 1 = error).
//
// When `refresh` is true (application startup or an explicit user refresh) we
// pass --refresh, which forces every enabled repo to re-download its metadata
// before checking. This guarantees the CLI sees the latest published updates
// (e.g. microsoft-edge-stable) even when the local cache is "fresh but
// stale".
//
// When `refresh` is false (periodic background checks) we deliberately omit
// --refresh: the command then reuses the system cache at /var/cache/dnf/
// (kept fresh by dnf-makecache.timer on most systems). This avoids two
// problems: (1) a long metadata download on every periodic tick, and (2) a
// lock conflict with an in-progress dnf5daemon transaction — `dnf5 check-upgrade
// --refresh` holds the rpmdb/metadata read lock while downloading, and
// do_transaction needs the exclusive lock to install, so running both
// concurrently leaves the progress dialog stuck at "Preparing...".
//
// On dnf5 we request --json output, which is robust against column-layout
// changes. dnf4 (the `dnf` shim) does not support --json, so we fall back to
// text parsing.
//
// NOTE: the CLI result is used only as a cross-check / safety net. The
// authoritative update list — the one the transaction resolves from — still
// comes from the daemon's packageList(PackageFilter::Updates), so the
// displayed list and the resolved transaction share the same sack.
static QList<Package> fetchUpdatesViaCli(bool refresh)
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    // Prefer dnf5 (the native package manager on the target distros); fall
    // back to the `dnf` compatibility shim if dnf5 is not on PATH.
    const bool hasDnf5 =
        !QStandardPaths::findExecutable(QStringLiteral("dnf5")).isEmpty();
    const QString dnfBin = hasDnf5 ? QStringLiteral("dnf5") : QStringLiteral("dnf");
    // dnf5's command is `check-upgrade`; dnf4 uses `check-update`.
    const QString checkCmd = hasDnf5 ? QStringLiteral("check-upgrade")
                                     : QStringLiteral("check-update");

    auto run = [&](const QStringList &args) -> bool {
        proc.start(dnfBin, args, QIODevice::ReadOnly);
        if (!proc.waitForFinished(300000)) {
            proc.kill();
            proc.waitForFinished(5000);
            qWarning() << dnfBin << checkCmd << "timed out";
            return false;
        }
        return true;
    };

    // Build the argument list. --refresh is only added when the caller
    // explicitly requested a metadata refresh (startup / explicit refresh),
    // never for periodic background checks — see the function doc comment.
    QStringList args = {checkCmd};
    if (refresh)
        args << QStringLiteral("--refresh");
    if (hasDnf5)
        args << QStringLiteral("--json");

    if (!run(args))
        return {};

    // Exit codes: 0 = no updates, 100 = updates available, 1 = real error.
    int exitCode = proc.exitCode();
    QString out = QString::fromUtf8(proc.readAllStandardOutput());

    // Some older dnf5 builds reject --json; retry without it (text output).
    // Keep the same --refresh choice as the first attempt.
    if (hasDnf5 && exitCode == 1) {
        QStringList retryArgs = {checkCmd};
        if (refresh)
            retryArgs << QStringLiteral("--refresh");
        if (!run(retryArgs))
            return {};
        exitCode = proc.exitCode();
        out = QString::fromUtf8(proc.readAllStandardOutput());
    }

    if (exitCode == 1) {
        qWarning() << dnfBin << checkCmd << "failed (exit 1):"
                   << QString::fromUtf8(proc.readAllStandardError());
        return {};
    }

    QList<Package> updates;
    if (hasDnf5 && out.trimmed().startsWith(QLatin1Char('{')))
        updates = parseCheckUpgradeJson(out);
    if (updates.isEmpty())
        updates = parseCheckUpgradeText(out);
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
    connect(m_client, &Dnf5DaemonClient::downloadAddNew, this, [this](const QString &id, const QString &desc, qint64 total) {
        // Record the package description (desc) keyed by download id so the
        // subsequent downloadProgress(id, ...) signals can surface which
        // package is currently being downloaded, not just "Downloading".
        m_downloadDescs.insert(id, desc);
        // Track the new download for the overall-percent aggregation.
        if (!m_activeDownloads.contains(id)) {
            m_activeDownloads.insert(id, {total, 0});
            m_downloadTotalBytes += total;
        }
        // Entering the download phase (download_add_new only fires while
        // downloading packages). Make sure the dialog reflects that even
        // if no transaction_before_begin has been received yet.
        m_inTransactionPhase = false;
        Q_EMIT downloadProgress(desc, total, 0);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::downloadProgress, this, [this](const QString &id, qint64 total, qint64 downloaded) {
        // Resolve the download id back to the package description captured in
        // downloadAddNew; if it is missing (e.g. a progress signal arrived
        // before add_new), fall back to the id itself.
        const QString desc = m_downloadDescs.value(id, id);
        // Keep the rolling byte totals in sync so the overall percent can be
        // computed without walking the whole hash on every tick.
        auto it = m_activeDownloads.find(id);
        if (it != m_activeDownloads.end()) {
            // The daemon may revise the total mid-download; subtract the old
            // contribution first and re-add the new one so the totals never
            // drift.
            m_downloadDownloadedBytes -= it->downloaded;
            m_downloadTotalBytes -= it->total;
            it->total = total;
            it->downloaded = downloaded;
            m_downloadDownloadedBytes += downloaded;
            m_downloadTotalBytes += total;
        }
        // The dialog / status bar compose the full "Downloading <pkg> +
        // percent" display from this signal themselves (MainWindow::
        // onDownloadProgress), so there is a single source of truth for which
        // package is currently being downloaded — see yumex-ng, which
        // re-asserts the package name on every download_progress signal.
        Q_EMIT downloadProgress(desc, total, downloaded);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::downloadEnd, this, [this](const QString &id, uint status, const QString &) {
        m_downloadDescs.remove(id);
        // Drop the finished download from the aggregation table. Its bytes
        // are subtracted from the running totals so the next progress tick
        // reports the right overall percent for the remaining downloads.
        auto it = m_activeDownloads.find(id);
        if (it != m_activeDownloads.end()) {
            m_downloadDownloadedBytes -= it->downloaded;
            m_downloadTotalBytes -= it->total;
            m_activeDownloads.erase(it);
        }
        if (status == 0)
            Q_EMIT transactionProgress(i18n("Download complete"), 100);
        emitOverallProgress();
    });

    connect(m_client, &Dnf5DaemonClient::transactionActionStart, this, [this](const QString &nevra, uint action, quint64 total) {
        Q_UNUSED(total)
        // Reset the per-action counters; transactionActionProgress will fill
        // them in. The action's total is also reported in start (same value
        // as the subsequent progress signals) so capture it once here.
        m_transactionCurrentActionProcessed = 0;
        m_transactionCurrentActionTotal = total;
        m_inVerifyPhase = false;
        QString actionStr = actionToString(static_cast<TransactionActionType>(action));
        Q_EMIT transactionProgress(i18n("%1 %2", actionStr, nevra), 0);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionActionProgress, this, [this](const QString &nevra, quint64 processed, quint64 total) {
        m_transactionCurrentActionProcessed = processed;
        m_transactionCurrentActionTotal = total;
        int percent = total > 0 ? static_cast<int>(processed * 100 / total) : 0;
        Q_EMIT transactionProgress(i18n("Processing %1", nevra), percent);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionActionStop, this, [this](const QString &nevra, quint64 total) {
        Q_UNUSED(nevra)
        Q_UNUSED(total)
        // The action is done; bump the completed counter and reset the
        // per-action progress so the next action starts from 0%.
        if (m_transactionTotalActions > 0
            && m_transactionActionsCompleted < m_transactionTotalActions)
            ++m_transactionActionsCompleted;
        m_transactionCurrentActionProcessed = 0;
        m_transactionCurrentActionTotal = 0;
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionVerifyStart, this, [this](quint64 total) {
        // Verification is a sub-phase inside the transaction phase; track it
        // separately so the overall percent keeps advancing through verify.
        m_inVerifyPhase = true;
        m_transactionVerifyTotal = total;
        m_transactionVerifyProcessed = 0;
        Q_EMIT transactionProgress(i18n("Verifying Packages"), 0);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionVerifyProgress, this, [this](quint64 processed, quint64 total) {
        m_transactionVerifyTotal = total;
        m_transactionVerifyProcessed = processed;
        int percent = total > 0 ? static_cast<int>(processed * 100 / total) : 0;
        Q_EMIT transactionProgress(i18n("Verifying"), percent);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionVerifyStop, this, [this]() {
        m_inVerifyPhase = false;
        m_transactionVerifyProcessed = m_transactionVerifyTotal;
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionBeforeBegin, this, [this](quint64 total) {
        // The transaction (install) phase begins. Reset the action counters
        // and remember the total so overall percent = completed / total.
        m_inTransactionPhase = true;
        m_inVerifyPhase = false;
        m_transactionTotalActions = total;
        m_transactionActionsCompleted = 0;
        m_transactionCurrentActionProcessed = 0;
        m_transactionCurrentActionTotal = 0;
        Q_EMIT transactionProgress(i18n("Applying Transaction"), 0);
        emitOverallProgress();
    });
    connect(m_client, &Dnf5DaemonClient::transactionAfterComplete, this, [this](bool) {
        // Clear the aggregation state so the next transaction starts fresh.
        resetProgressState();
    });
    connect(m_client, &Dnf5DaemonClient::transactionScriptStart, this, [this](const QString &, uint) {
        Q_EMIT transactionProgress(i18n("Running scripts"), 0);
        emitOverallProgress();
    });

    connect(m_client, &Dnf5DaemonClient::errorOccurred, this, &Backend::errorOccurred);
    connect(m_client, &Dnf5DaemonClient::repoKeyImportRequest, this, [this](const QString &keyId, const QStringList &userIds,
                                                                             const QString &fingerprint, const QString &url, qint64) {
        qInfo() << "Repository key import request:" << keyId << userIds << fingerprint << url;
    });
}

void Backend::resetProgressState()
{
    m_activeDownloads.clear();
    m_downloadTotalBytes = 0;
    m_downloadDownloadedBytes = 0;
    m_transactionTotalActions = 0;
    m_transactionActionsCompleted = 0;
    m_transactionCurrentActionProcessed = 0;
    m_transactionCurrentActionTotal = 0;
    m_transactionVerifyTotal = 0;
    m_transactionVerifyProcessed = 0;
    m_inVerifyPhase = false;
    m_inTransactionPhase = false;
    // Re-emit the overall progress so the ProgressDialog immediately flips
    // to the "Preparing..." indeterminate state. Without this push the
    // dialog would keep showing whatever percent the *previous* transaction
    // had reached (e.g. "Installing... 87%") for the whole duration of the
    // goal-resolution / metadata-loading prelude of the new transaction —
    // the user would think the new operation was almost done before it had
    // even started. emitOverallProgress() now reports the "prepare" phase
    // (because every flag above was just cleared), which the dialog renders
    // as a busy / indeterminate bar.
    emitOverallProgress();
}

int Backend::computeOverallDownloadPercent() const
{
    // Aggregated download percent across every active download: the sum of
    // bytes downloaded so far divided by the sum of every download's total.
    // When the daemon has not reported any download yet (or every total is
    // zero) return 0 so the dialog shows a sensible "0%" instead of NaN.
    if (m_downloadTotalBytes <= 0)
        return 0;
    qint64 pct = m_downloadDownloadedBytes * 100 / m_downloadTotalBytes;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return static_cast<int>(pct);
}

int Backend::computeOverallTransactionPercent() const
{
    // Overall transaction percent = (completed actions + fraction of the
    // current action) / total actions. The verify sub-phase is folded in as
    // a final fraction so the percent keeps advancing through verification.
    if (m_transactionTotalActions == 0)
        return 0;
    // Fraction of the current action (0..1) as a permyriad so integer math
    // keeps precision before the final division.
    quint64 currentFraction = 0;
    if (m_inVerifyPhase) {
        if (m_transactionVerifyTotal > 0)
            currentFraction = m_transactionVerifyProcessed * 10000 / m_transactionVerifyTotal;
    } else if (m_transactionCurrentActionTotal > 0) {
        currentFraction = m_transactionCurrentActionProcessed * 10000
                          / m_transactionCurrentActionTotal;
    }
    quint64 scaled = (m_transactionActionsCompleted * 10000 + currentFraction)
                     / m_transactionTotalActions;
    quint64 pct = scaled / 10000;
    if (pct > 100)
        pct = 100;
    return static_cast<int>(pct);
}

void Backend::emitOverallProgress()
{
    // Pick the phase that the dialog should be describing based on which
    // signals have arrived so far. Once transaction_before_begin fires the
    // download phase is over and the install phase begins; verification is
    // a tail sub-phase of install.
    if (m_inTransactionPhase) {
        int percent = computeOverallTransactionPercent();
        QString phase = m_inVerifyPhase ? QStringLiteral("verify") : QStringLiteral("install");
        QString msg = m_inVerifyPhase ? i18n("Verifying packages...")
                                      : i18n("Installing packages...");
        Q_EMIT overallProgress(percent, phase, msg);
    } else if (!m_activeDownloads.isEmpty() || m_downloadTotalBytes > 0) {
        int percent = computeOverallDownloadPercent();
        Q_EMIT overallProgress(percent, QStringLiteral("download"),
                              i18n("Downloading packages..."));
    } else {
        // Neither phase has started yet — the dialog is in the "Preparing"
        // state (goal resolution / metadata read inside do_transaction).
        Q_EMIT overallProgress(0, QStringLiteral("prepare"), i18n("Preparing..."));
    }
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

    // Deliberately NO background sack warm-up here. dnf5daemon-server runs
    // every metadata-requiring call (read_all_repos, list_fd, ...) while
    // holding a single global libdnf5 mutex; a background read_all_repos
    // whose metadata download stalls (unreachable mirror, hanging DNS) then
    // blocks EVERY subsequent list_fd on that same mutex, and the client
    // only learns about it after its full fd timeout (observed in the field
    // as "timed out waiting for package stream (1200000 ms without data)"
    // repeated for every query). It is better to let the first real query
    // trigger fill_sack() itself: if the metadata download is merely slow,
    // the client-side per-poll timeout below tolerates it as long as chunks
    // keep flowing, and if the query times out, the download typically
    // completes in the daemon anyway — the user's retry then finds the sack
    // READY and streams immediately. ERROR-locked sacks are healed by the
    // resetSession() self-heal in fetchPackages().


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

    // Self-heal: an empty result for a full package scope is never
    // legitimate (the system always has packages). The daemon returns an
    // empty stream WITHOUT any error when its fill_sack() throws inside
    // list_fd — most commonly because the session sack is in a permanent
    // ERROR state (a previous metadata load failed, e.g. an untrusted repo
    // key in non-interactive mode). Reset the session and retry once so the
    // browser heals instead of silently showing an empty list. Updates /
    // Upgradable scopes are excluded: zero updates is a normal outcome there.
    // resetSession() is harmless while a transaction runs (it fails and the
    // next query retries), and retrying is cheap — the second call typically
    // finds the sack freshly READY and streams the list immediately.
    if (packages.isEmpty() && filter != PackageFilter::Updates
        && filter != PackageFilter::Upgradable) {
        qWarning() << "fetchPackages: empty list for filter" << int(filter)
                   << "- resetting daemon session and retrying once";
        m_client->resetSession();
        packages = m_client->packageList({QStringLiteral("*")}, attrs, filter);
    }
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
    // No latest-limit override: the default is 1 (newest EVR per name.arch,
    // like the reference yumex-ng client). Search results stay small — every
    // version of every match (latest-limit=0) would balloon the daemon's
    // reply and memory footprint for no GUI benefit.

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

QList<Package> Backend::fetchUpdates(bool refreshMetadata)
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

    // The authoritative update list comes from the *daemon* itself
    // (packageList with PackageFilter::Updates), exactly as yumex-ng does
    // (package_list_fd(scope="upgrades")). This is critical: the daemon
    // maintains its own metadata cache at /var/cache/dnf5daemon-server/,
    // which is *separate* from the system dnf cache at /var/cache/dnf/ that
    // `dnf5 check-upgrade` reads. Reading the update list from the daemon
    // guarantees the displayed list and the resolved transaction always
    // share the same sack.
    //
    // refreshMetadata is true at application startup and when the user
    // clicks the explicit Refresh action — the two moments the software
    // source is force-refreshed. To guarantee every repo re-downloads its
    // metadata (not just the ones whose cache has expired per
    // metadata_expire) we first call cleanCache("expire-cache"), which
    // marks every repo's on-disk cache as expired, then readAllRepos()
    // re-fetches them into /var/cache/dnf5daemon-server/. This mirrors
    // `dnf5 check-upgrade --refresh` and ensures newly published updates
    // (e.g. third-party-repo packages like microsoft-edge-stable) are
    // always picked up. resetSession() drops any stale in-memory sack so
    // the subsequent packageList() query rebuilds the sack from the
    // freshly refreshed on-disk cache.
    if (refreshMetadata) {
        m_client->cleanCache(QStringLiteral("expire-cache"));
        m_client->readAllRepos();
        m_client->resetSession();
    }

    static const QStringList detailAttrs = {
        QStringLiteral("name"), QStringLiteral("evr"), QStringLiteral("arch"),
        QStringLiteral("repo_id"), QStringLiteral("summary"), QStringLiteral("install_size"),
        QStringLiteral("is_installed"), QStringLiteral("description"), QStringLiteral("url"),
        QStringLiteral("license"), QStringLiteral("version"), QStringLiteral("release"),
        QStringLiteral("epoch")
    };

    QList<Package> result =
        m_client->packageList({QStringLiteral("*")}, detailAttrs, PackageFilter::Updates);

    // Cross-check the daemon's list against `dnf5 check-upgrade`. When
    // refreshMetadata is true (startup / explicit refresh) the CLI is invoked
    // with --refresh so it re-downloads every repo's metadata into the system
    // cache and reliably sees newly published updates (e.g. microsoft-edge-
    // stable). On periodic checks refreshMetadata is false and the CLI reuses
    // the cached system metadata — this avoids the rpmdb/metadata lock
    // conflict that would otherwise stall an in-progress transaction at
    // "Preparing...". The daemon keeps its own cache at
    // /var/cache/dnf5daemon-server/ and readAllRepos() only re-downloads
    // metadata for repos whose cache has *expired*, so a repo whose cache is
    // "fresh" but stale still drops that package's update from the daemon's
    // list; comparing it against the CLI result lets us detect and heal the
    // divergence (refresh daemon cache + re-query, with a CLI-only safety net).
    const QList<Package> cliUpdates = fetchUpdatesViaCli(refreshMetadata);

    auto collectNa = [](const QList<Package> &pkgs) {
        QSet<QString> na;
        na.reserve(pkgs.size());
        for (const auto &p : pkgs)
            na.insert(p.na());
        return na;
    };

    QSet<QString> daemonNa = collectNa(result);
    int missingCount = 0;
    for (const auto &p : cliUpdates) {
        if (!daemonNa.contains(p.na()))
            ++missingCount;
    }
    const bool daemonIncomplete = missingCount > 0;

    // The daemon's update list is missing packages that `dnf5 check-upgrade`
    // (with --refresh on explicit refreshes, cached metadata otherwise)
    // knows about. Its on-disk metadata cache is stale, so force every repo
    // to re-download (expire-cache marks all caches expired, readAllRepos
    // then re-fetches them) and re-query from a freshly rebuilt sack. This
    // guarantees the authoritative list — the one the transaction resolves
    // from — matches what dnf5 displays.
    if (daemonIncomplete) {
        const QString cliCmd = refreshMetadata
                                   ? QStringLiteral("`dnf5 check-upgrade --refresh`")
                                   : QStringLiteral("`dnf5 check-upgrade`");
        qWarning() << "Daemon update list is missing" << missingCount
                   << "package(s) known to" << cliCmd << ";"
                   << "refreshing daemon metadata cache and re-querying";
        m_client->cleanCache(QStringLiteral("expire-cache"));
        m_client->readAllRepos();
        m_client->resetSession();
        result = m_client->packageList({QStringLiteral("*")}, detailAttrs, PackageFilter::Updates);
        daemonNa = collectNa(result);
    }

    // Final safety net: if a repo still fails to load (transient network
    // error, unreachable mirror, …) the daemon may still omit some CLI
    // updates. Append those entries so the user at least sees them; the
    // transaction resolves from the daemon's sack and will simply skip any
    // package it cannot resolve rather than erroring out.
    for (const auto &p : cliUpdates) {
        if (!daemonNa.contains(p.na())) {
            result.append(p);
            daemonNa.insert(p.na());
        }
    }

    // Daemon rows come back with state == Installed (rpm.list reports the
    // installed package that has an upgrade available); mark every entry as
    // an update so it renders in the update list and can be queued.
    for (Package &p : result) {
        p.state = PackageState::Update;
        p.calcTodo();
    }

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
        // Do not cache an empty listing: an empty result here is almost always
        // the symptom of a failed/interrupted transfer (stale daemon sack,
        // truncated list_fd pipe), and caching it would lock the browser into
        // an empty list until the next forced reset. Re-querying on the next
        // page switch costs a few seconds and lets a transient failure heal.
        // (Only the update checker legitimately reports "no packages", and it
        // does not go through this cache.)
        if (!packages.isEmpty())
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

void Backend::loadUpdates(bool refreshMetadata)
{
    auto *watcher = new QFutureWatcher<QList<Package>>(this);
    connect(watcher, &QFutureWatcher<QList<Package>>::finished, this, [this, watcher]() {
        QList<Package> updates = watcher->result();
        Q_EMIT updatesLoaded(updates);
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this, refreshMetadata]() {
        return fetchUpdates(refreshMetadata);
    }));
}

TransactionResult Backend::buildTransaction(const QList<Package> &packages, const TransactionOptions &opts)
{
    return m_transactionManager->buildTransaction(packages, opts);
}

TransactionResult Backend::runTransaction(const TransactionOptions &opts)
{
    // Reset the aggregated progress state before every run so leftover
    // counters from a previous transaction (e.g. one that failed before
    // transaction_after_complete fired) cannot bleed into the new one and
    // freeze the dialog at "Installing..." 100% before any install signal
    // arrives. resetProgressState() also drops m_inTransactionPhase so the
    // very first emitOverallProgress() reports the "prepare" phase — which
    // is what the dialog should show while do_transaction is still in its
    // goal-resolution / metadata-loading prelude.
    resetProgressState();
    return m_transactionManager->runTransaction(opts);
}

TransactionResult Backend::depsolve(const QList<Package> &packages)
{
    return m_transactionManager->depsolve(packages);
}

}
