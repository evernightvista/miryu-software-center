#include "TransactionManager.h"

#include <QDebug>
#include <QMetaObject>

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
    // Drop the daemon's in-memory base (and thus its repo sack) before
    // building the goal. For upgrade queues buildTransaction() has already
    // called readAllRepos() to sync the on-disk system cache; reset() here
    // forces the subsequent fill_sack() inside resolve() to reload from that
    // fresh cache instead of reusing a stale startup sack. Without this the
    // daemon could resolve only a subset of the queued upgrades — e.g. 71
    // queued upgrades collapsing to 13 in the transaction summary. reset()
    // itself does no network I/O; the cache reload inside fill_sack() reads
    // from disk.
    m_client->resetSession();

    // Clear any goal left over from a previous buildTransaction() /
    // runTransaction() call so the new package specs do not accumulate on
    // top of the old ones.
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
        // dnf5daemon returns both download_size (RPM payload, what must be
        // fetched) and install_size (on-disk footprint after install).
        // For a "Total Download Size" summary the download size is the
        // relevant figure; fall back to install_size only when the daemon
        // did not provide a download size (e.g. @commandline packages).
        size = objMap.value(QStringLiteral("download_size")).toLongLong();
        if (size == 0)
            size = objMap.value(QStringLiteral("install_size")).toLongLong();

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

    // No metadata refresh here.
    //
    // The update list the user is about to apply was produced by the daemon's
    // own packageList(PackageFilter::Updates) call (see Backend::fetchUpdates),
    // so the displayed packages and the daemon's on-disk metadata cache are
    // guaranteed to describe the same world. buildTransactions() calls
    // resetSession() to drop the in-memory sack, and resolve()'s fill_sack()
    // rebuilds it from that same on-disk cache — so the resolved transaction
    // always matches the queued packages. Forcing a cleanCache("expire-cache")
    // + readAllRepos() re-download here would only stall the UI for many
    // seconds without changing the result, because the cache already matches
    // what we are about to resolve.
    //
    // The software source is refreshed only at:
    //   1. Application startup (UpdateChecker → Backend::fetchUpdates(true),
    //      which calls readAllRepos() so only expired metadata is
    //      re-downloaded).
    //   2. The explicit "Refresh Metadata" action (which uses
    //      cleanCache("expire-cache") + readAllRepos() to force a full
    //      re-download).

    const int genBefore = m_client->reconnectCount();
    const bool buildOk = buildTransactions(packages, opts);
    // A reconnect at any point during goal setup invalidates the specs added
    // to the old session (reconnect() opens a brand-new, goal-less session).
    // Even when buildTransactions reports success, a mid-way reconnect leaves
    // the goal incomplete; resolve() would then return a misleading empty
    // transaction ("0 B / empty summary"). Detect both failure and a
    // generation bump and bail out with a retry-friendly message.
    if (!buildOk || m_client->reconnectCount() != genBefore) {
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
    const bool buildOk = buildTransactions(m_lastTransaction, opts);
    // See buildTransaction() for why a mid-goal reconnect (even when
    // buildTransactions succeeds) must abort instead of resolving an empty
    // goal on the freshly opened session.
    if (!buildOk || m_client->reconnectCount() != genBefore) {
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

    // runTransaction() runs on a worker thread (QtConcurrent::run), but the
    // D-Bus progress signals emitted during do_transaction are dispatched to
    // the thread that owns Dnf5DaemonClient — the GUI/main thread. If we
    // called doTransaction() here on the worker thread with QDBus::Block,
    // the connection's internal lock would be held by the worker and the
    // main thread's signal queue could starve, leaving the progress dialog
    // stuck at "Preparing..." with no package name.
    //
    // doTransaction() itself uses QDBus::BlockWithGui, which only helps if
    // it runs on the SAME thread the signals are delivered to. So marshal
    // the call onto the main thread via a blocking queued invocation: the
    // worker waits, the main thread executes do_transaction, and while it
    // waits for the daemon's reply BlockWithGui drains the incoming signal
    // queue so download / install progress reaches the UI.
    bool transactionOk = false;
    QMetaObject::invokeMethod(m_client, [this, &transOptions, &transactionOk]() {
        transactionOk = m_client->doTransaction(transOptions);
    }, Qt::BlockingQueuedConnection);

    if (transactionOk) {
        result.completed = true;
        // The transaction just ran (install / upgrade / downgrade / reinstall
        // / remove). Restart the dnf5daemon-server service unit and promptly
        // re-acquire the D-Bus connection:
        //
        //  - The restart gives the daemon a clean in-memory state (rpmdb,
        //    repo sack, goal) instead of leaving it to serve queries from a
        //    sack that no longer matches the packages on disk. It runs via
        //    `systemctl restart dnf5daemon-server` without a polkit
        //    authentication dialog (shipped polkit rules file
        //    data/50-miryu-dnf5daemon.rules grants restarting only this
        //    unit).
        //  - The daemon process is replaced by the restart, so the session
        //    path we hold is now invalid: every subsequent D-Bus query
        //    (onRefresh -> resetSession / packageList / repoList) would hit
        //    "Not connected to D-Bus server" and recover one by one via
        //    callSync()'s retry loop, and a signal arriving on the dead
        //    session could even crash the app. restartDaemonServer()
        //    reconnects promptly and silently, once, so the caller's
        //    post-transaction queries run against a valid session.
        //    Best-effort: on failure it stays silent and the per-call
        //    recovery still kicks in as a fallback.
        m_client->restartDaemonServer();
    } else {
        result.completed = false;
        // Surface the real daemon error (e.g. polkit "Not authorized")
        // instead of a generic message, so the UI can detect auth
        // cancellation and show a friendly single dialog.
        result.error = m_client->lastError().isEmpty()
            ? QStringLiteral("Transaction execution failed")
            : m_client->lastError();
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
    const bool buildOk = buildTransactions(packages, {});
    // See buildTransaction() for why a mid-goal reconnect (even when
    // buildTransactions succeeds) must abort instead of resolving an empty
    // goal on the freshly opened session.
    if (!buildOk || m_client->reconnectCount() != genBefore) {
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
