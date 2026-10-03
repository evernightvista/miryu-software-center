#pragma once

#include <QObject>
#include <QFutureWatcher>
#include <QHash>
#include <QString>
#include "Dnf5DaemonClient.h"
#include "TransactionManager.h"
#include "PackageCache.h"
#include "Package.h"
#include "Repository.h"
#include "Transaction.h"
#include "Enums.h"

namespace Miryu {

class Backend : public QObject
{
    Q_OBJECT

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend();

    Dnf5DaemonClient *client() const { return m_client; }
    TransactionManager *transactionManager() const { return m_transactionManager; }
    PackageCache *cache() const { return m_cache; }

    bool initialize();
    bool isInitialized() const { return m_initialized; }
    QString lastError() const { return m_lastError; }

    // Async package operations
    void loadPackages(PackageFilter filter, bool reset = false);
    void searchPackages(const QString &query, SearchField field = SearchField::All,
                        PackageFilter scope = PackageFilter::All);
    void loadRepositories();
    // Load the update list. When refreshMetadata is true the daemon is asked
    // to reload its on-disk cache (readAllRepos) before querying, and the CLI
    // cross-check runs `dnf5 check-upgrade --refresh` to force a fresh
    // metadata download into the system cache — this reliably detects every
    // published update (e.g. third-party-repo packages like microsoft-edge-
    // stable) and drives a daemon-cache refresh when the daemon's list is
    // incomplete. When refreshMetadata is false the CLI omits --refresh and
    // reuses the cached system metadata, avoiding the rpmdb/metadata lock
    // conflict that would otherwise stall an in-progress transaction at
    // "Preparing...". Per the desired behaviour the software source is
    // refreshed ONLY at application startup (UpdateChecker calls
    // loadUpdates(true)) and the explicit Refresh action. Periodic checks,
    // page switches and post-transaction reloads pass false so they reuse the
    // cached metadata.
    void loadUpdates(bool refreshMetadata = false);

    // Sync operations (call from worker threads)
    QList<Package> fetchPackages(PackageFilter filter);
    QList<Package> fetchSearchResults(const QString &query, SearchField field, PackageFilter scope);
    QList<Repository> fetchRepositories();
    QList<Package> fetchUpdates(bool refreshMetadata = false);

    // Transaction operations
    TransactionResult buildTransaction(const QList<Package> &packages, const TransactionOptions &opts = {});
    TransactionResult runTransaction(const TransactionOptions &opts = {});
    TransactionResult depsolve(const QList<Package> &packages);

Q_SIGNALS:
    void packagesLoaded(PackageFilter filter, const QList<Package> &packages);
    void searchCompleted(const QList<Package> &packages);
    void repositoriesLoaded(const QList<Repository> &repos);
    void updatesLoaded(const QList<Package> &updates);
    void transactionProgress(const QString &message, int percent);
    void downloadProgress(const QString &downloadId, qint64 total, qint64 downloaded);
    // Aggregated, phase-aware progress for the modal ProgressDialog. During
    // the download phase this is the overall percent across every active
    // download (sum(downloaded) / sum(total)); during the install /
    // transaction phase it is the overall percent across all transaction
    // actions (completed actions + fraction of the current action). The
    // phase string identifies which phase the dialog should be describing
    // ("download" / "install" / "prepare" / "verify").
    void overallProgress(int percent, const QString &phase, const QString &message);
    void errorOccurred(const QString &error);
    void initialized();

private:
    Dnf5DaemonClient *m_client;
    TransactionManager *m_transactionManager;
    PackageCache *m_cache;
    bool m_initialized = false;
    QString m_lastError;
    // Maps dnf5daemon download ids to the package description captured in
    // downloadAddNew, so downloadProgress(id, ...) can show which package is
    // currently being downloaded (the progress signal carries only the id).
    QHash<QString, QString> m_downloadDescs;

    // --- Aggregated progress state -------------------------------------
    // Download phase: a running table of every active download keyed by the
    // dnf5daemon download id, plus the rolling byte totals so the overall
    // percent can be computed in O(1) on every progress tick instead of
    // walking the hash.
    struct DownloadStat { qint64 total = 0; qint64 downloaded = 0; };
    QHash<QString, DownloadStat> m_activeDownloads;
    qint64 m_downloadTotalBytes = 0;
    qint64 m_downloadDownloadedBytes = 0;

    // Transaction (install) phase: the daemon reports the total number of
    // actions in transaction_before_begin and then a per-action progress
    // stream. We convert that into a single overall percent.
    quint64 m_transactionTotalActions = 0;
    quint64 m_transactionActionsCompleted = 0;
    quint64 m_transactionCurrentActionProcessed = 0;
    quint64 m_transactionCurrentActionTotal = 0;
    // Verify sub-phase: reported separately by transaction_verify_* signals.
    quint64 m_transactionVerifyTotal = 0;
    quint64 m_transactionVerifyProcessed = 0;
    bool m_inVerifyPhase = false;
    // True once transaction_before_begin fires — used to switch the dialog
    // from the download phase to the install phase.
    bool m_inTransactionPhase = false;

    void resetProgressState();
    int computeOverallDownloadPercent() const;
    int computeOverallTransactionPercent() const;
    void emitOverallProgress();

    void connectClientSignals();
};

}
