#pragma once

#include <QObject>
#include <QFutureWatcher>
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
    void loadUpdates();

    // Sync operations (call from worker threads)
    QList<Package> fetchPackages(PackageFilter filter);
    QList<Package> fetchSearchResults(const QString &query, SearchField field, PackageFilter scope);
    QList<Repository> fetchRepositories();
    QList<Package> fetchUpdates();

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
    void errorOccurred(const QString &error);
    void initialized();

private:
    Dnf5DaemonClient *m_client;
    TransactionManager *m_transactionManager;
    PackageCache *m_cache;
    bool m_initialized = false;
    QString m_lastError;

    void connectClientSignals();
};

}
