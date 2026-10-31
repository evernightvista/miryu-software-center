#pragma once

#include <QObject>
#include <QHash>
#include "Dnf5DaemonClient.h"
#include "Package.h"
#include "Transaction.h"
#include "Enums.h"

namespace Miryu {

class TransactionManager : public QObject
{
    Q_OBJECT

public:
    explicit TransactionManager(Dnf5DaemonClient *client, QObject *parent = nullptr);

    TransactionResult buildTransaction(const QList<Package> &packages, const TransactionOptions &opts = {});
    TransactionResult runTransaction(const TransactionOptions &opts = {});
    TransactionResult depsolve(const QList<Package> &packages);

    QList<Repository> repositories() const { return m_repos; }
    void loadRepositories();
    QHash<QString, int> repoPriorities() const { return m_repoPriorities; }

    QList<Package> filterUpdates(const QList<Package> &updates,
                                 const QList<Package> &obsoletes = {});

    // Returns true when any of the solver's resolve-log strings describes a
    // broken dependency (unsatisfiable require, package conflict, missing
    // provider, ...). Used to block a transaction that the solver marked as
    // WARNING (resolved but with skipped broken-dependency packages) instead
    // of letting it proceed and leave the system half-updated.
    static bool hasBrokenDependencyProblems(const QStringList &problems);

private:
    Dnf5DaemonClient *m_client;
    QList<Package> m_lastTransaction;
    QList<Repository> m_repos;
    QHash<QString, int> m_repoPriorities;
    QHash<QString, QString> m_installedEvr;

    TransactionResult buildResult(const QVariantList &transactionItems, uint resultCode);
    bool buildTransactions(const QList<Package> &packages, const TransactionOptions &opts);
    void fetchInstalledEvr();

    // Returns "Failed to prepare transaction", or a friendlier message when a
    // mid-build reconnect invalidated the goal state on the old session.
    QString transactionBuildError(int reconnectCountBefore) const;
};

}
