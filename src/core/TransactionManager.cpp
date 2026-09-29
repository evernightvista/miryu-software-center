#include "TransactionManager.h"

#include <QDebug>

namespace Miryu {

TransactionManager::TransactionManager(Dnf5DaemonClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
}

void TransactionManager::loadRepositories()
{
    m_repos = m_client->repoList();
    m_repoPriorities.clear();
    for (const auto &repo : m_repos) {
        if (repo.enabled)
            m_repoPriorities[repo.id] = repo.priority;
    }
}

void TransactionManager::fetchInstalledEvr()
{
    m_installedEvr.clear();
    QStringList attrs = {QStringLiteral("name"), QStringLiteral("arch"), QStringLiteral("evr")};
    QList<Package> installed = m_client->packageList({QStringLiteral("*")}, attrs, PackageFilter::Installed);
    for (const auto &pkg : installed) {
        m_installedEvr[pkg.na()] = pkg.evr();
    }
}

QList<Package> TransactionManager::filterUpdates(const QList<Package> &updates,
                                                  const QList<Package> &obsoletes)
{
    // Make sure we know the currently installed EVR per name+arch so we can
    // discard candidates that do not actually represent an upgrade (e.g. an
    // obsoletes entry that returned the already-installed old version instead
    // of the replacing candidate).
    if (m_installedEvr.isEmpty())
        fetchInstalledEvr();

    // Merge upgrades and obsoletes first. Obsoletes represent packages that
    // are being replaced across repositories (e.g. steam moving from
    // rpmfusion-nonfree-updates-testing to terra); without merging them the
    // update list silently drops these cross-repository replacements.
    QList<Package> merged = updates;
    merged += obsoletes;

    // Group by name+arch so that different architectures of the same package
    // (e.g. steam.x86_64 and steam-arch-transition.noarch) are kept separately,
    // while multiple repository candidates for the same name+arch are
    // de-duplicated by keeping the best one (highest repo priority, then
    // highest EVR).
    QHash<QString, QList<Package>> byNa;
    for (const auto &pkg : merged) {
        // Skip candidates that are not newer than what is already installed
        // for this name+arch — they are not real updates.
        const QString installedEvr = m_installedEvr.value(pkg.na());
        if (!installedEvr.isEmpty() && !(pkg.evr() > installedEvr))
            continue;
        byNa[pkg.na()].append(pkg);
    }

    QList<Package> result;
    for (const auto &na : byNa.keys()) {
        auto &pkgs = byNa[na];
        int minPriority = 99;
        for (const auto &p : pkgs)
            minPriority = qMin(minPriority, m_repoPriorities.value(p.repo, 99));

        QString bestEvr;
        Package bestPkg;
        for (const auto &p : pkgs) {
            if (m_repoPriorities.value(p.repo, 99) == minPriority) {
                if (bestEvr.isEmpty() || p.evr() > bestEvr) {
                    bestEvr = p.evr();
                    bestPkg = p;
                }
            }
        }
        if (!bestPkg.name.isEmpty())
            result.append(bestPkg);
    }
    return result;
}

bool TransactionManager::buildTransactions(const QList<Package> &packages, const TransactionOptions &opts)
{
    // Clear any goal left over from a previous buildTransaction() /
    // runTransaction() call so the new package specs do not accumulate on
    // top of the old ones.
    //
    // Repository metadata is intentionally NOT refreshed here. The software
    // source (cleanCache + resetSession + readAllRepos) used to be
    // force-refreshed before building the goal, which meant every click on
    // "Apply" (buildTransaction) refreshed the software source, and every
    // click on "OK" to accept the changes (runTransaction) refreshed it a
    // second time — a slow, redundant metadata download each time.
    //
    // Per the desired behaviour, the software source is refreshed only at
    // application startup (UpdateChecker → `dnf check-update --refresh`) and
    // when the user explicitly clicks "更新"/Refresh (onRefresh), never
    // while building or running a transaction. The daemon's sack, loaded
    // at startup / last refresh, is reused as-is here.
    m_client->resetGoal();

    QStringList toInstall, toUpdate, toRemove, toDowngrade, toReinstall, toDistroSync;

    if (opts.command == TransactionCommand::IsFile) {
        for (const auto &pkg : packages)
            toInstall.append(pkg.name);
        return m_client->install(toInstall);
    }

    if (opts.command == TransactionCommand::SystemUpgrade) {
        QVariantMap options;
        options[QStringLiteral("mode")] = opts.parameter.isEmpty() ? QStringLiteral("distrosync") : opts.parameter;
        return m_client->systemUpgrade(options);
    }

    if (opts.command == TransactionCommand::SystemDistroSync) {
        if (!m_installedEvr.isEmpty())
            fetchInstalledEvr();
        for (const auto &pkg : packages)
            toDistroSync.append(pkg.name);
        return m_client->distroSync(toDistroSync);
    }

    for (const auto &pkg : packages) {
        switch (pkg.todo) {
        case PackageTodo::Install:    toInstall.append(pkg.name); break;
        case PackageTodo::Update:     toUpdate.append(pkg.name); break;
        case PackageTodo::Remove:     toRemove.append(pkg.name); break;
        case PackageTodo::Downgrade:  toDowngrade.append(pkg.name); break;
        case PackageTodo::Reinstall:  toReinstall.append(pkg.name); break;
        case PackageTodo::DistroSync: toDistroSync.append(pkg.name); break;
        case PackageTodo::None:       break;
        }
    }

    bool ok = true;
    if (!toInstall.isEmpty())
        ok = m_client->install(toInstall) && ok;
    if (!toUpdate.isEmpty())
        ok = m_client->upgrade(toUpdate) && ok;
    if (!toRemove.isEmpty())
        ok = m_client->remove(toRemove) && ok;
    if (!toDowngrade.isEmpty())
        ok = m_client->downgrade(toDowngrade) && ok;
    if (!toReinstall.isEmpty())
        ok = m_client->reinstall(toReinstall) && ok;
    if (!toDistroSync.isEmpty())
        ok = m_client->distroSync(toDistroSync) && ok;

    return ok;
}

TransactionResult TransactionManager::buildResult(const QVariantList &transactionItems, uint resultCode)
{
    TransactionResult result;

    if (resultCode == 0 || resultCode == 1) {
        result.completed = true;
    } else {
        result.completed = false;
        result.error = QStringLiteral("Transaction resolving failed");
    }

    if (resultCode == 1) {
        result.problems = m_client->getTransactionProblemsString();
    }

    // Parse transaction items into action-based groups
    static const QHash<QString, QString> actionMap = {
        {QStringLiteral("Install"),    QStringLiteral("install")},
        {QStringLiteral("Upgrade"),    QStringLiteral("upgrade")},
        {QStringLiteral("Downgrade"),  QStringLiteral("downgrade")},
        {QStringLiteral("Reinstall"),  QStringLiteral("reinstall")},
        {QStringLiteral("Remove"),     QStringLiteral("remove")},
        {QStringLiteral("Replaced"),   QStringLiteral("replaced")},
        {QStringLiteral("Change"),     QStringLiteral("change")},
        {QStringLiteral("Obsoleted"),  QStringLiteral("obsoleted")},
        {QStringLiteral("Reason Change"), QStringLiteral("reason_change")},
    };

    for (const auto &item : transactionItems) {
        QVariantList parts = item.toList();
        if (parts.size() < 5)
            continue;

        QString actionStr = parts[1].toString();
        QString actionKey = actionMap.value(actionStr, actionStr.toLower());

        QString nevra;
        QString repo;
        qint64 size = 0;

        QVariantMap objMap = parts[4].toMap();
        nevra = objMap.value(QStringLiteral("full_nevra")).toString();
        if (nevra.isEmpty())
            nevra = objMap.value(QStringLiteral("nevra")).toString();
        repo = objMap.value(QStringLiteral("repo_id")).toString();
        size = objMap.value(QStringLiteral("install_size")).toLongLong();
        if (size == 0)
            size = objMap.value(QStringLiteral("download_size")).toLongLong();

        QVariantList entry;
        QVariantList nevraRepo;
        nevraRepo.append(nevra);
        nevraRepo.append(repo);
        entry.append(QVariant(nevraRepo));
        entry.append(size);

        QVariantMap &dataMap = result.data;
        QVariantList list = dataMap.value(actionKey).toList();
        list.append(QVariant(entry));
        dataMap[actionKey] = list;
    }

    return result;
}

TransactionResult TransactionManager::buildTransaction(const QList<Package> &packages, const TransactionOptions &opts)
{
    TransactionResult result;

    m_lastTransaction = packages;

    const int genBefore = m_client->reconnectCount();
    if (!buildTransactions(packages, opts)) {
        result.error = transactionBuildError(genBefore);
        return result;
    }

    bool allowErasing = false;
    if (opts.command == TransactionCommand::SystemUpgrade ||
        opts.command == TransactionCommand::SystemDistroSync ||
        opts.command == TransactionCommand::IsFile)
        allowErasing = true;

    // Check if any package is being removed or downgraded
    for (const auto &pkg : packages) {
        if (pkg.todo == PackageTodo::Remove || pkg.todo == PackageTodo::Downgrade)
            allowErasing = true;
    }

    auto resolveResult = m_client->resolve(allowErasing);
    if (!resolveResult.success) {
        result.error = resolveResult.error;
        result.problems = m_client->getTransactionProblemsString();
        return result;
    }

    result = buildResult(resolveResult.transactionItems, resolveResult.resultCode);
    return result;
}

TransactionResult TransactionManager::runTransaction(const TransactionOptions &opts)
{
    TransactionResult result;

    const int genBefore = m_client->reconnectCount();
    if (!buildTransactions(m_lastTransaction, opts)) {
        result.error = transactionBuildError(genBefore);
        return result;
    }

    bool allowErasing = false;
    if (opts.command == TransactionCommand::SystemUpgrade ||
        opts.command == TransactionCommand::SystemDistroSync ||
        opts.command == TransactionCommand::IsFile)
        allowErasing = true;
    for (const auto &pkg : m_lastTransaction) {
        if (pkg.todo == PackageTodo::Remove || pkg.todo == PackageTodo::Downgrade)
            allowErasing = true;
    }

    auto resolveResult = m_client->resolve(allowErasing);
    if (!resolveResult.success) {
        result.error = resolveResult.error;
        result.problems = m_client->getTransactionProblemsString();
        return result;
    }

    QVariantMap transOptions;
    if (opts.offline)
        transOptions[QStringLiteral("offline")] = true;

    if (m_client->doTransaction(transOptions)) {
        result.completed = true;
        // The transaction just ran. If it upgraded dbus, dnf5daemon-server
        // or systemd, the daemon process is replaced and the session path
        // we hold is now invalid — every subsequent D-Bus query
        // (onRefresh -> readAllRepos / packageList / repoList) would hit
        // "Not connected to D-Bus server" and recover one by one via
        // callSync()'s retry loop. Reopen a fresh session here, once and
        // silently, so the caller's post-transaction queries run against
        // a valid session. reopenSession() is a no-op-safe best effort:
        // on failure it stays silent and the per-call recovery still kicks
        // in as a fallback.
        m_client->reopenSession();
    } else {
        result.completed = false;
        result.error = QStringLiteral("Transaction execution failed");
    }

    // resetGoal() clears the goal on the (possibly new) session; on a
    // freshly reopened session the goal is empty so this is a harmless
    // no-op. On a session that survived the transaction it clears the
    // consumed goal as before.
    m_client->resetGoal();
    return result;
}

TransactionResult TransactionManager::depsolve(const QList<Package> &packages)
{
    TransactionResult result;

    const int genBefore = m_client->reconnectCount();
    if (!buildTransactions(packages, {})) {
        result.error = transactionBuildError(genBefore);
        return result;
    }

    auto resolveResult = m_client->resolve(false);
    if (!resolveResult.success) {
        result.error = resolveResult.error;
        return result;
    }

    result = buildResult(resolveResult.transactionItems, resolveResult.resultCode);
    result.completed = resolveResult.success;
    m_client->resetGoal();
    return result;
}

QString TransactionManager::transactionBuildError(int reconnectCountBefore) const
{
    // If the client re-connected while the goal was being set up, the specs
    // added to the OLD session were lost — retrying resolve on the new
    // (goal-less) session would yield an incomplete transaction. Ask the user
    // to retry the whole operation instead.
    if (m_client->reconnectCount() != reconnectCountBefore) {
        return QStringLiteral("The connection to dnf5daemon-server was interrupted "
                              "while preparing the transaction. Please try again.");
    }
    return QStringLiteral("Failed to prepare transaction");
}

}
