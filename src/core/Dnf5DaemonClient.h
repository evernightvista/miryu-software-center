#pragma once

#include <QObject>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QStringList>
#include "Package.h"
#include "Repository.h"
#include "Transaction.h"
#include "Enums.h"

namespace Miryu {

class Dnf5DaemonClient : public QObject
{
    Q_OBJECT

public:
    static constexpr const char *BUS_NAME = "org.rpm.dnf.v0";
    static constexpr const char *OBJECT_PATH = "/org/rpm/dnf/v0";

    explicit Dnf5DaemonClient(QObject *parent = nullptr);
    ~Dnf5DaemonClient();

    bool isConnected() const { return m_connected; }
    bool isSessionOpen() const { return !m_sessionPath.isEmpty(); }
    QString sessionPath() const { return m_sessionPath; }
    QString lastError() const { return m_lastError; }

    // Session management
    bool openSession(const QVariantMap &options = {});
    bool closeSession();
    void resetSession();

    // Repository operations
    QList<Repository> repoList(const QStringList &attrs = QStringList{QStringLiteral("name"), QStringLiteral("enabled"), QStringLiteral("priority")},
                               const QString &enableDisable = QStringLiteral("all"));
    // Enable or disable a repository. This calls the dnf5daemon
    // org.rpm.dnf.v0.rpm.Repo enable/disable D-Bus methods, which trigger
    // the daemon's own polkit policy (org.rpm.dnf.v0.rpm.Repo.conf_write)
    // with a proper localized authentication message.
    bool repoEnable(const QString &repoId, bool enabled);

    // Package queries
    QList<Package> packageList(const QStringList &patterns,
                               const QStringList &packageAttrs = QStringList{QStringLiteral("name"), QStringLiteral("evr"), QStringLiteral("arch"), QStringLiteral("repo_id"), QStringLiteral("summary"), QStringLiteral("install_size"), QStringLiteral("is_installed")},
                               PackageFilter scope = PackageFilter::All,
                               const QVariantMap &extraOptions = {});

    // Fetch extended package details (requires, provides, files, changelog,
    // description) for a single package.
    // pkgName is used as the pattern for dnf5daemon's list method (which
    // matches against package names, not full NEVRA strings).
    // nevra is used to verify we got the correct package from the results.
    // Returns a QVariantMap with keys matching dnf5daemon attrs.
    QVariantMap getPackageDetails(const QString &pkgName, const QString &nevra);

    // Package operations
    bool install(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool upgrade(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool remove(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool downgrade(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool reinstall(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool distroSync(const QStringList &pkgSpecs, const QVariantMap &options = {});
    bool systemUpgrade(const QVariantMap &options = {});

    // Goal operations
    struct ResolveResult {
        QVariantList transactionItems;
        uint resultCode = 0;
        bool success = false;
        QString error;
    };
    ResolveResult resolve(bool allowErasing = false);
    QStringList getTransactionProblemsString();
    bool doTransaction(const QVariantMap &options = {});
    bool cancelTransaction();
    bool resetGoal();

    // Offline operations
    QVariantMap offlineGetStatus();
    bool offlineCancel();
    bool offlineClean();
    bool offlineSetFinishAction(const QString &action);
    bool offlineScheduleForNextBoot();

    // GPG key confirmation
    bool confirmKey(const QString &keyId, bool confirmed);

    // Advisory
    QVariantList advisoryList(const QVariantMap &options = {});

    // Groups
    QVariantList groupList(const QVariantMap &options = {});

    // Cache operations
    bool cleanCache(const QString &cacheType = QStringLiteral("all"));
    bool readAllRepos();

Q_SIGNALS:
    // Download signals (simplified — session path stripped)
    void downloadAddNew(const QString &downloadId, const QString &description, qint64 totalToDownload);
    void downloadProgress(const QString &downloadId, qint64 total, qint64 downloaded);
    void downloadEnd(const QString &downloadId, uint transferStatus, const QString &message);
    void downloadMirrorFailure(const QString &downloadId, const QString &message, const QString &url);
    void repoKeyImportRequest(const QString &keyId, const QStringList &userIds,
                              const QString &keyFingerprint, const QString &keyUrl, qint64 timestamp);

    // Transaction signals (simplified — session path stripped)
    void transactionActionStart(const QString &nevra, uint action, quint64 total);
    void transactionActionProgress(const QString &nevra, quint64 processed, quint64 total);
    void transactionActionStop(const QString &nevra, quint64 total);
    void transactionBeforeBegin(quint64 total);
    void transactionAfterComplete(bool success);
    void transactionScriptStart(const QString &nevra, uint scriptletType);
    void transactionScriptStop(const QString &nevra, uint scriptletType, quint64 returnCode);
    void transactionScriptError(const QString &nevra, uint scriptletType, quint64 returnCode);
    void transactionVerifyStart(quint64 total);
    void transactionVerifyProgress(quint64 processed, quint64 total);
    void transactionVerifyStop();
    void transactionElemProgress(const QString &nevra, quint64 processed, quint64 total);

    void errorOccurred(const QString &error);
    void sessionOpened();
    void sessionClosed();

private:
    // D-Bus interface names (stored as strings, no QDBusInterface to avoid introspection)
    static constexpr const char *IFACE_SESSION_MANAGER = "org.rpm.dnf.v0.SessionManager";
    static constexpr const char *IFACE_BASE = "org.rpm.dnf.v0.Base";
    static constexpr const char *IFACE_REPO = "org.rpm.dnf.v0.rpm.Repo";
    static constexpr const char *IFACE_RPM = "org.rpm.dnf.v0.rpm.Rpm";
    static constexpr const char *IFACE_GOAL = "org.rpm.dnf.v0.Goal";
    static constexpr const char *IFACE_OFFLINE = "org.rpm.dnf.v0.Offline";
    static constexpr const char *IFACE_GROUP = "org.rpm.dnf.v0.comps.Group";
    static constexpr const char *IFACE_ADVISORY = "org.rpm.dnf.v0.Advisory";

    QDBusConnection m_bus;
    QString m_sessionPath;
    bool m_connected = false;
    QString m_lastError;

    // Low-level D-Bus call using QDBusMessage directly (no introspection)
    QDBusMessage callMethod(const QString &path, const QString &interface,
                           const QString &method, const QList<QVariant> &args = {});
    bool callSync(const QString &path, const QString &interface,
                  const QString &method, const QList<QVariant> &args,
                  QVariant &result, QString &error);

    void connectSignals();
    void disconnectSignals();

    // Internal D-Bus signal handlers (receive full signal with session path)
private Q_SLOTS:
    void onDownloadAddNew(const QDBusObjectPath &session, const QString &id, const QString &desc, qint64 total);
    void onDownloadProgress(const QDBusObjectPath &session, const QString &id, qint64 total, qint64 downloaded);
    void onDownloadEnd(const QDBusObjectPath &session, const QString &id, uint status, const QString &msg);
    void onDownloadMirrorFailure(const QDBusObjectPath &session, const QString &id, const QString &msg, const QString &url);
    void onRepoKeyImportRequest(const QDBusObjectPath &session, const QString &keyId, const QStringList &userIds,
                                const QString &fingerprint, const QString &url, qint64 timestamp);
    void onTransactionActionStart(const QDBusObjectPath &session, const QString &nevra, uint action, quint64 total);
    void onTransactionActionProgress(const QDBusObjectPath &session, const QString &nevra, quint64 processed, quint64 total);
    void onTransactionActionStop(const QDBusObjectPath &session, const QString &nevra, quint64 total);
    void onTransactionBeforeBegin(const QDBusObjectPath &session, quint64 total);
    void onTransactionAfterComplete(const QDBusObjectPath &session, bool success);
    void onTransactionScriptStart(const QDBusObjectPath &session, const QString &nevra, uint type);
    void onTransactionScriptStop(const QDBusObjectPath &session, const QString &nevra, uint type, quint64 rc);
    void onTransactionScriptError(const QDBusObjectPath &session, const QString &nevra, uint type, quint64 rc);
    void onTransactionVerifyStart(const QDBusObjectPath &session, quint64 total);
    void onTransactionVerifyProgress(const QDBusObjectPath &session, quint64 processed, quint64 total);
    void onTransactionVerifyStop(const QDBusObjectPath &session);
    void onTransactionElemProgress(const QDBusObjectPath &session, const QString &nevra, quint64 processed, quint64 total);
};

}
