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

private:
    Dnf5DaemonClient *m_client;
    QList<Package> m_lastTransaction;
    QList<Repository> m_repos;
    QHash<QString, int> m_repoPriorities;
    QHash<QString, QString> m_installedEvr;

    TransactionResult buildResult(const QVariantList &transactionItems, uint resultCode);
    bool buildTransactions(const QList<Package> &packages, const TransactionOptions &opts);
    void fetchInstalledEvr();
};

}
