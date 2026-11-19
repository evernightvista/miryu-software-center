#pragma once

#include <QObject>
#include <QProcess>
#include <QFutureWatcher>
#include <QSet>
#include <QMap>
#include <QQueue>
#include "FlatpakApp.h"

class QNetworkAccessManager;

namespace Miryu {

/*!
 * \class FlatpakBackend
 * \brief Backend that wraps the \c flatpak CLI tool.
 *
 * Provides asynchronous access to installed flatpak applications, remote
 * repositories, the flatpak store (search), and package lifecycle operations
 * (install / uninstall / update / run).
 *
 * All long-running operations use QProcess with progress reporting via
 * signals.  Query operations (list, remotes, search) use QtConcurrent to
 * avoid blocking the UI thread.
 */
class FlatpakBackend : public QObject
{
    Q_OBJECT

public:
    explicit FlatpakBackend(QObject *parent = nullptr);

    bool isAvailable() const;

    // Async queries (emit signals on completion)
    void loadInstalledApps();
    // Load only the installed refs (applications and runtimes) that have an
    // update available, parsed from the summary printed by `flatpak update`
    // (works on older flatpak releases that lack `flatpak list --updates`).
    // Used by the Updates page so Flatpak updates are listed next to RPM
    // updates.
    void loadUpdatableApps();
    void loadRemotes();
    void loadPopularApps();
    void searchApps(const QString &keyword);

    // Package operations (async, emit signals on progress / completion)
    void installApp(const QString &remote, const QString &appId);
    void uninstallApp(const QString &appId, bool deleteData = false);
    void updateApp(const QString &appId);
    void runApp(const QString &appId);

    // Batch update through the normal async operation queue. Progress and
    // completion are reported by operationProgress/operationFinished with
    // operation "update-batch" and appId set to the comma-separated IDs.
    void updateApps(const QStringList &appIds);

Q_SIGNALS:
    void installedAppsLoaded(const QList<FlatpakApp> &apps);
    void updatableAppsLoaded(const QList<FlatpakApp> &apps);
    void remotesLoaded(const QMap<QString, QString> &remotes);
    void popularAppsLoaded(const QList<FlatpakApp> &apps);
    void searchCompleted(const QList<FlatpakApp> &apps);

    void operationStarted(const QString &appId, const QString &operation);
    void operationProgress(const QString &appId, const QString &message, int percent);
    void operationFinished(const QString &appId, const QString &operation, bool success, const QString &message);
    void errorOccurred(const QString &error);

private:
    bool m_available;
    QNetworkAccessManager *m_networkManager;

    QString executeSync(const QStringList &args, bool &ok);
    void executeAsync(const QString &appId, const QString &operation, const QStringList &args);
    void startNextOperation();

    struct OperationRequest {
        QString appId;
        QString operation;
        QStringList args;
    };
    QQueue<OperationRequest> m_pendingOperations;
    bool m_operationRunning = false;

    QList<FlatpakApp> parseListOutput(const QByteArray &output);
    QList<FlatpakApp> parseSearchOutput(const QByteArray &output);
    QMap<QString, QString> parseRemotesOutput(const QByteArray &output);

    // One ref that `flatpak update` wants to update. The branch is parsed from
    // the same summary row so the updatable ref can be matched against the
    // installed / available listings (needed to report correct versions).
    struct UpdatableRef {
        QString appId;
        QString branch;
    };
    // Refs with a pending update, taken from the numbered summary printed by
    // plain `flatpak update`. Used instead of `flatpak list --updates`, which
    // older flatpak releases reject with "Unknown option --updates".
    QList<UpdatableRef> fetchUpdatableRefs();

    void checkAvailability();
};

}
