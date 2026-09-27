#include "Dnf5DaemonClient.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QDBusObjectPath>
#include <QDBusAbstractInterface>
#include <QMetaType>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QMap>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <KLocalizedString>

namespace Miryu {

// Helper: serialize a QMap<QString,QString> as a D-Bus a{ss} variant.
// QDBusAbstractInterface would do this automatically, but we're using raw
// QDBusMessage, so we need to build the argument manually.
static QVariant makeStringMap(const QMap<QString, QString> &map)
{
    QDBusArgument arg;
    arg.beginMap(QMetaType::QString, QMetaType::QString);
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        arg.beginMapEntry();
        arg << it.key() << it.value();
        arg.endMapEntry();
    }
    arg.endMap();
    return QVariant::fromValue(arg);
}

// === Core D-Bus Communication ===
// All calls use QDBusConnection::call() with QDBusMessage directly.
// This completely bypasses QDBusInterface and its introspection mechanism,
// which fails against dnf5daemon-server because the daemon does not implement
// the org.freedesktop.DBus.Introspectable interface at its object paths.

QDBusMessage Dnf5DaemonClient::callMethod(const QString &path, const QString &interface,
                                          const QString &method, const QList<QVariant> &args)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(BUS_NAME),
        path,
        interface,
        method);
    if (!args.isEmpty())
        msg.setArguments(args);
    // Use 60s timeout — some dnf5daemon operations (repo loading, metadata refresh)
    // can take longer than the default 25s.
    return m_bus.call(msg, QDBus::Block, 60000);
}

bool Dnf5DaemonClient::callSync(const QString &path, const QString &interface,
                                 const QString &method, const QList<QVariant> &args,
                                 QVariant &result, QString &error)
{
    QDBusMessage msg = callMethod(path, interface, method, args);

    // Handle all error message types
    if (msg.type() == QDBusMessage::ErrorMessage) {
        error = msg.errorMessage();
        qWarning() << "DBus call failed:" << interface << method << ":" << error;
        Q_EMIT errorOccurred(error);
        return false;
    }

    if (msg.type() == QDBusMessage::InvalidMessage) {
        // This happens when the reply times out or no reply is received.
        error = QStringLiteral("Did not receive a reply from dnf5daemon-server within the timeout period. "
                               "The service may be busy or unresponsive.");
        qWarning() << "DBus call timed out:" << interface << method;
        Q_EMIT errorOccurred(error);
        return false;
    }

    if (!msg.arguments().isEmpty())
        result = msg.arguments().first();
    return true;
}

// === Constructor / Destructor ===

Dnf5DaemonClient::Dnf5DaemonClient(QObject *parent)
    : QObject(parent)
    , m_bus(QDBusConnection::systemBus())
{
    qRegisterMetaType<Package>("Miryu::Package");
    qRegisterMetaType<Repository>("Miryu::Repository");

    // Check if the system bus itself is reachable.
    m_connected = m_bus.isConnected();

    if (!m_connected) {
        qWarning() << "Cannot connect to system D-Bus";
    } else {
        // Start dnf5daemon-server if it's not already running.
        // Use D-Bus service activation (org.freedesktop.DBus.StartServiceByName)
        // instead of `systemctl start`. Service activation launches the service
        // silently via D-Bus and does NOT require polkit authentication (systemd
        // service start would otherwise prompt for polkit on some systems).
        QDBusConnectionInterface *iface = m_bus.interface();
        if (iface && !iface->isServiceRegistered(QString::fromLatin1(BUS_NAME))) {
            qInfo() << "dnf5daemon-server not registered on D-Bus; "
                       "activating it via D-Bus service activation.";
            QDBusReply<void> reply = iface->startService(QString::fromLatin1(BUS_NAME));
            if (reply.isValid()) {
                qInfo() << "D-Bus service activation requested for dnf5daemon-server.";
                // Give it a moment to register on the bus.
                QThread::msleep(1000);
            } else {
                qWarning() << "D-Bus service activation for dnf5daemon-server failed:"
                           << reply.error().message();
            }
        }
    }
}

Dnf5DaemonClient::~Dnf5DaemonClient()
{
    // Disconnect D-Bus signal subscriptions FIRST, before closing the
    // session.  This prevents signals from being delivered to our slot
    // methods (which emit Qt signals) after we begin tearing down.
    // Without this, a signal arriving between closeSession() and the
    // QObject destructor can trigger a use-after-free in downstream
    // receivers (Backend → MainWindow).
    disconnectSignals();

    if (isSessionOpen()) {
        closeSession();
    }
}

// === Session Management ===

bool Dnf5DaemonClient::openSession(const QVariantMap &options)
{
    if (!m_connected) {
        qWarning() << "Dnf5DaemonClient: not connected to bus";
        m_lastError = QStringLiteral("Not connected to system D-Bus");
        Q_EMIT errorOccurred(m_lastError);
        return false;
    }

    m_lastError.clear();

    QVariantMap opts = options;
    if (!opts.contains(QStringLiteral("config"))) {
        // The dnf5daemon API expects config as map{string:string} (a{ss}),
        // NOT a{sv}. We must serialize it with the correct D-Bus type signature.
        QMap<QString, QString> config;
        config[QStringLiteral("optional_metadata_types")] = QStringLiteral("other");
        opts[QStringLiteral("config")] = makeStringMap(config);
    } else {
        // Convert existing config from QVariantMap to a{ss} if needed
        QVariant configVar = opts.value(QStringLiteral("config"));
        if (configVar.canConvert<QVariantMap>()) {
            QVariantMap configMap = configVar.toMap();
            QMap<QString, QString> stringConfig;
            for (auto it = configMap.constBegin(); it != configMap.constEnd(); ++it) {
                stringConfig[it.key()] = it.value().toString();
            }
            opts[QStringLiteral("config")] = makeStringMap(stringConfig);
        }
    }

    // Retry loop: the service might still be starting up.
    // Try up to 3 times with 500ms delay between attempts.
    const int maxRetries = 3;

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        qInfo() << "open_session attempt" << attempt << "of" << maxRetries;

        // Call open_session via raw QDBusMessage — no QDBusInterface, no introspection.
        QDBusMessage msg = callMethod(QString::fromLatin1(OBJECT_PATH),
                                      QString::fromLatin1(IFACE_SESSION_MANAGER),
                                      QStringLiteral("open_session"),
                                      {QVariant(opts)});

        if (msg.type() == QDBusMessage::ReplyMessage && !msg.arguments().isEmpty()) {
            // Success — extract session path from the object path return value
            QVariant arg = msg.arguments().first();
            if (arg.canConvert<QDBusObjectPath>()) {
                m_sessionPath = arg.value<QDBusObjectPath>().path();
            } else if (arg.canConvert<QString>()) {
                m_sessionPath = arg.toString();
            } else {
                qWarning() << "open_session returned unexpected type:" << arg.typeName();
                m_lastError = QStringLiteral("open_session returned unexpected type: %1")
                                     .arg(QString::fromLatin1(arg.typeName()));
                Q_EMIT errorOccurred(m_lastError);
                return false;
            }

            qInfo() << "open_session succeeded, session path:" << m_sessionPath;
            connectSignals();
            Q_EMIT sessionOpened();
            return true;
        }

        // Error case
        m_lastError = msg.errorMessage();
        qWarning() << "open_session attempt" << attempt << "failed:" << m_lastError
                   << "(type:" << msg.type() << ")";

        // If this is a "service not found" type error, retry after a delay.
        // For other errors (e.g., auth failures), no point retrying.
        bool isServiceUnknown = m_lastError.contains(QStringLiteral("ServiceUnknown"), Qt::CaseInsensitive) ||
                                m_lastError.contains(QStringLiteral("not provided"), Qt::CaseInsensitive) ||
                                m_lastError.contains(QStringLiteral("NoReply"), Qt::CaseInsensitive);

        if (attempt < maxRetries && isServiceUnknown) {
            QThread::msleep(500);
            continue;
        }

        // Non-retryable error or out of retries — report the actual D-Bus error
        break;
    }

    // Emit the actual D-Bus error so the user sees what really went wrong
    if (m_lastError.isEmpty()) {
        m_lastError = QStringLiteral("Failed to open a session with dnf5daemon-server. "
                                    "The service may not be running or may not be reachable "
                                    "via the system D-Bus. Try:\n"
                                    "  sudo systemctl start dnf5daemon-server\n"
                                    "  sudo systemctl enable dnf5daemon-server");
    }
    Q_EMIT errorOccurred(m_lastError);
    return false;
}

bool Dnf5DaemonClient::closeSession()
{
    if (m_sessionPath.isEmpty())
        return true;

    // Disconnect D-Bus signal subscriptions before closing the session
    // to prevent signals from being delivered during/after close.
    disconnectSignals();

    QDBusMessage msg = callMethod(QString::fromLatin1(OBJECT_PATH),
                                  QString::fromLatin1(IFACE_SESSION_MANAGER),
                                  QStringLiteral("close_session"),
                                  {QVariant::fromValue(QDBusObjectPath(m_sessionPath))});
    m_sessionPath.clear();

    Q_EMIT sessionClosed();
    return msg.type() != QDBusMessage::ErrorMessage;
}

void Dnf5DaemonClient::resetSession()
{
    if (m_sessionPath.isEmpty())
        return;
    // Use the raw callMethod() (NOT callSync()) on purpose: reset() is a
    // best-effort housekeeping call. After a transaction that upgraded
    // low-level packages (dbus, dnf5daemon-server itself, systemd) the
    // system bus / daemon may be momentarily unavailable, and a failing
    // reset() must NOT raise a user-facing error dialog — we just want the
    // daemon to drop its in-memory repo sack so the next query sees fresh
    // metadata. callMethod() returns the raw QDBusMessage without emitting
    // errorOccurred, so a transient D-Bus error here stays silent.
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_BASE),
                                  QStringLiteral("reset"), {});
    if (msg.type() == QDBusMessage::ErrorMessage)
        qWarning() << "reset failed (ignored):" << msg.errorMessage();
}

// === Signal Connection ===

void Dnf5DaemonClient::connectSignals()
{
    // Base interface signals — downloads and key import
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_add_new"), this, SLOT(onDownloadAddNew(QDBusObjectPath,QString,QString,qint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_progress"), this, SLOT(onDownloadProgress(QDBusObjectPath,QString,qint64,qint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_end"), this, SLOT(onDownloadEnd(QDBusObjectPath,QString,uint,QString)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_mirror_failure"), this, SLOT(onDownloadMirrorFailure(QDBusObjectPath,QString,QString,QString)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("repo_key_import_request"), this, SLOT(onRepoKeyImportRequest(QDBusObjectPath,QString,QStringList,QString,QString,qint64)));

    // Rpm interface signals — transaction progress
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_start"), this, SLOT(onTransactionActionStart(QDBusObjectPath,QString,uint,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_progress"), this, SLOT(onTransactionActionProgress(QDBusObjectPath,QString,quint64,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_stop"), this, SLOT(onTransactionActionStop(QDBusObjectPath,QString,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_before_begin"), this, SLOT(onTransactionBeforeBegin(QDBusObjectPath,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_after_complete"), this, SLOT(onTransactionAfterComplete(QDBusObjectPath,bool)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_start"), this, SLOT(onTransactionScriptStart(QDBusObjectPath,QString,uint)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_stop"), this, SLOT(onTransactionScriptStop(QDBusObjectPath,QString,uint,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_error"), this, SLOT(onTransactionScriptError(QDBusObjectPath,QString,uint,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_start"), this, SLOT(onTransactionVerifyStart(QDBusObjectPath,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_progress"), this, SLOT(onTransactionVerifyProgress(QDBusObjectPath,quint64,quint64)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_stop"), this, SLOT(onTransactionVerifyStop(QDBusObjectPath)));
    m_bus.connect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_elem_progress"), this, SLOT(onTransactionElemProgress(QDBusObjectPath,QString,quint64,quint64)));
}

void Dnf5DaemonClient::disconnectSignals()
{
    // Disconnect all D-Bus signal subscriptions that were set up in
    // connectSignals().  This must be called before closeSession() and
    // before the object is destroyed to prevent D-Bus signals from
    // being delivered to our slot methods during teardown.
    if (m_sessionPath.isEmpty())
        return;

    // Base interface signals — downloads and key import
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_add_new"), this, SLOT(onDownloadAddNew(QDBusObjectPath,QString,QString,qint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_progress"), this, SLOT(onDownloadProgress(QDBusObjectPath,QString,qint64,qint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_end"), this, SLOT(onDownloadEnd(QDBusObjectPath,QString,uint,QString)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("download_mirror_failure"), this, SLOT(onDownloadMirrorFailure(QDBusObjectPath,QString,QString,QString)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_BASE),
                QStringLiteral("repo_key_import_request"), this, SLOT(onRepoKeyImportRequest(QDBusObjectPath,QString,QStringList,QString,QString,qint64)));

    // Rpm interface signals — transaction progress
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_start"), this, SLOT(onTransactionActionStart(QDBusObjectPath,QString,uint,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_progress"), this, SLOT(onTransactionActionProgress(QDBusObjectPath,QString,quint64,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_action_stop"), this, SLOT(onTransactionActionStop(QDBusObjectPath,QString,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_before_begin"), this, SLOT(onTransactionBeforeBegin(QDBusObjectPath,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_after_complete"), this, SLOT(onTransactionAfterComplete(QDBusObjectPath,bool)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_start"), this, SLOT(onTransactionScriptStart(QDBusObjectPath,QString,uint)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_stop"), this, SLOT(onTransactionScriptStop(QDBusObjectPath,QString,uint,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_script_error"), this, SLOT(onTransactionScriptError(QDBusObjectPath,QString,uint,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_start"), this, SLOT(onTransactionVerifyStart(QDBusObjectPath,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_progress"), this, SLOT(onTransactionVerifyProgress(QDBusObjectPath,quint64,quint64)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_verify_stop"), this, SLOT(onTransactionVerifyStop(QDBusObjectPath)));
    m_bus.disconnect(QString::fromLatin1(BUS_NAME), m_sessionPath, QString::fromLatin1(IFACE_RPM),
                QStringLiteral("transaction_elem_progress"), this, SLOT(onTransactionElemProgress(QDBusObjectPath,QString,quint64,quint64)));
}

// === D-Bus Signal Handler Implementations ===

void Dnf5DaemonClient::onDownloadAddNew(const QDBusObjectPath &, const QString &id, const QString &desc, qint64 total)
{
    Q_EMIT downloadAddNew(id, desc, total);
}

void Dnf5DaemonClient::onDownloadProgress(const QDBusObjectPath &, const QString &id, qint64 total, qint64 downloaded)
{
    Q_EMIT downloadProgress(id, total, downloaded);
}

void Dnf5DaemonClient::onDownloadEnd(const QDBusObjectPath &, const QString &id, uint status, const QString &msg)
{
    Q_EMIT downloadEnd(id, status, msg);
}

void Dnf5DaemonClient::onDownloadMirrorFailure(const QDBusObjectPath &, const QString &id, const QString &msg, const QString &url)
{
    Q_EMIT downloadMirrorFailure(id, msg, url);
}

void Dnf5DaemonClient::onRepoKeyImportRequest(const QDBusObjectPath &, const QString &keyId, const QStringList &userIds,
                                               const QString &fingerprint, const QString &url, qint64 timestamp)
{
    Q_EMIT repoKeyImportRequest(keyId, userIds, fingerprint, url, timestamp);
}

void Dnf5DaemonClient::onTransactionActionStart(const QDBusObjectPath &, const QString &nevra, uint action, quint64 total)
{
    Q_EMIT transactionActionStart(nevra, action, total);
}

void Dnf5DaemonClient::onTransactionActionProgress(const QDBusObjectPath &, const QString &nevra, quint64 processed, quint64 total)
{
    Q_EMIT transactionActionProgress(nevra, processed, total);
}

void Dnf5DaemonClient::onTransactionActionStop(const QDBusObjectPath &, const QString &nevra, quint64 total)
{
    Q_EMIT transactionActionStop(nevra, total);
}

void Dnf5DaemonClient::onTransactionBeforeBegin(const QDBusObjectPath &, quint64 total)
{
    Q_EMIT transactionBeforeBegin(total);
}

void Dnf5DaemonClient::onTransactionAfterComplete(const QDBusObjectPath &, bool success)
{
    Q_EMIT transactionAfterComplete(success);
}

void Dnf5DaemonClient::onTransactionScriptStart(const QDBusObjectPath &, const QString &nevra, uint type)
{
    Q_EMIT transactionScriptStart(nevra, type);
}

void Dnf5DaemonClient::onTransactionScriptStop(const QDBusObjectPath &, const QString &nevra, uint type, quint64 rc)
{
    Q_EMIT transactionScriptStop(nevra, type, rc);
}

void Dnf5DaemonClient::onTransactionScriptError(const QDBusObjectPath &, const QString &nevra, uint type, quint64 rc)
{
    Q_EMIT transactionScriptError(nevra, type, rc);
}

void Dnf5DaemonClient::onTransactionVerifyStart(const QDBusObjectPath &, quint64 total)
{
    Q_EMIT transactionVerifyStart(total);
}

void Dnf5DaemonClient::onTransactionVerifyProgress(const QDBusObjectPath &, quint64 processed, quint64 total)
{
    Q_EMIT transactionVerifyProgress(processed, total);
}

void Dnf5DaemonClient::onTransactionVerifyStop(const QDBusObjectPath &)
{
    Q_EMIT transactionVerifyStop();
}

void Dnf5DaemonClient::onTransactionElemProgress(const QDBusObjectPath &, const QString &nevra, quint64 processed, quint64 total)
{
    Q_EMIT transactionElemProgress(nevra, processed, total);
}

// === Repository Operations ===

QList<Repository> Dnf5DaemonClient::repoList(const QStringList &attrs, const QString &enableDisable)
{
    QList<Repository> repos;
    if (m_sessionPath.isEmpty())
        return repos;

    QVariantMap options;
    options[QStringLiteral("repo_attrs")] = attrs;
    options[QStringLiteral("enable_disable")] = enableDisable;

    QVariant result;
    QString error;
    // Block the raw D-Bus error emitted by callSync so we can replace it
    // with a localized error message.
    bool wasBlocked = blockSignals(true);
    bool ok = callSync(m_sessionPath, QString::fromLatin1(IFACE_REPO),
                       QStringLiteral("list"), {QVariant(options)}, result, error);
    blockSignals(wasBlocked);
    if (!ok) {
        m_lastError = i18n("Cannot load repositories.");
        Q_EMIT errorOccurred(m_lastError);
        return repos;
    }

    const QDBusArgument &arg = result.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QVariantMap repoMap;
        arg >> repoMap;
        repos.append(Repository::fromVariantMap(repoMap));
    }
    arg.endArray();

    return repos;
}

// === Repository Enable/Disable ===

bool Dnf5DaemonClient::repoEnable(const QString &repoId, bool enabled)
{
    if (m_sessionPath.isEmpty()) {
        m_lastError = QStringLiteral("No active dnf5daemon session");
        Q_EMIT errorOccurred(m_lastError);
        return false;
    }

    // dnf5daemon's org.rpm.dnf.v0.rpm.Repo interface exposes:
    //   enable(in as repo_ids)      -> ()
    //   disable(in as repo_ids)     -> ()
    // Both take an array of repository ids. The call triggers the daemon's
    // own polkit policy (org.rpm.dnf.v0.rpm.Repo.conf_write) automatically,
    // so the authentication dialog shows a proper localized message instead
    // of a bare "pkexec" title.
    QStringList repoIds{repoId};
    QString method = enabled ? QStringLiteral("enable") : QStringLiteral("disable");

    QVariant result;
    QString error;
    if (!callSync(m_sessionPath, QString::fromLatin1(IFACE_REPO),
                  method, {QVariant(repoIds)}, result, error)) {
        m_lastError = error;
        return false;
    }
    return true;
}

// === Package Queries ===

QList<Package> Dnf5DaemonClient::packageList(const QStringList &patterns,
                                             const QStringList &packageAttrs,
                                             PackageFilter scope,
                                             const QVariantMap &extraOptions)
{
    QList<Package> packages;
    if (m_sessionPath.isEmpty())
        return packages;

    QVariantMap options;
    options[QStringLiteral("patterns")] = patterns;
    options[QStringLiteral("package_attrs")] = packageAttrs;
    options[QStringLiteral("with_src")] = false;
    options[QStringLiteral("icase")] = true;
    // NOTE: the dnf5daemon option key is "latest-limit" (hyphen), not
    // "latest_limit" (underscore). Passing latest-limit=0 disables the
    // limit so candidates from every repository are returned — otherwise
    // cross-repository upgrade candidates can be silently dropped.
    options[QStringLiteral("latest-limit")] = extraOptions.value(QStringLiteral("latest-limit"), 0);

    QString scopeStr;
    switch (scope) {
    case PackageFilter::All:        scopeStr = QStringLiteral("all"); break;
    case PackageFilter::Installed:  scopeStr = QStringLiteral("installed"); break;
    case PackageFilter::Available:  scopeStr = QStringLiteral("available"); break;
    case PackageFilter::Updates:    scopeStr = QStringLiteral("upgrades"); break;
    case PackageFilter::Upgradable: scopeStr = QStringLiteral("upgradable"); break;
    }
    options[QStringLiteral("scope")] = scopeStr;

    if (extraOptions.contains(QStringLiteral("repo")))
        options[QStringLiteral("repo")] = extraOptions.value(QStringLiteral("repo"));
    if (extraOptions.contains(QStringLiteral("arch")))
        options[QStringLiteral("arch")] = extraOptions.value(QStringLiteral("arch"));

    QVariant result;
    QString error;
    if (!callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                  QStringLiteral("list"), {QVariant(options)}, result, error)) {
        return packages;
    }

    const QDBusArgument &arg = result.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QVariantMap pkgMap;
        arg >> pkgMap;
        Package pkg = Package::fromVariantMap(pkgMap, PackageState::Available);

        bool installed = pkgMap.value(QStringLiteral("is_installed")).toBool();
        if (scope == PackageFilter::Updates || scope == PackageFilter::Upgradable)
            pkg.state = PackageState::Update;
        else if (installed)
            pkg.state = PackageState::Installed;
        else
            pkg.state = PackageState::Available;

        pkg.calcTodo();
        packages.append(pkg);
    }
    arg.endArray();

    return packages;
}

// === Package Details ===

// Helper: run dnf5 repoquery synchronously and return output lines.
static QStringList runDnf5RepoQuery(const QString &pkgName, const QStringList &extraArgs)
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    QStringList args = {QStringLiteral("repoquery")};
    args += extraArgs;
    args += QStringList{QStringLiteral("--quiet"), pkgName};

    proc.start(QStringLiteral("dnf5"), args, QIODevice::ReadOnly);
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        proc.waitForFinished(5000);
        return {};
    }
    if (proc.exitCode() != 0)
        return {};

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    QStringList lines = output.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
    QStringList result;
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            result << trimmed;
    }
    return result;
}

// Helper: run dnf5 repoquery --changelogs and parse multi-line entries.
static QStringList runDnf5RepoQueryChangelogs(const QString &pkgName)
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    QStringList args = {QStringLiteral("repoquery"),
                        QStringLiteral("--changelogs"),
                        QStringLiteral("--quiet"),
                        pkgName};

    proc.start(QStringLiteral("dnf5"), args, QIODevice::ReadOnly);
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        proc.waitForFinished(5000);
        return {};
    }
    if (proc.exitCode() != 0)
        return {};

    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    if (output.isEmpty())
        return {};

    QStringList entries;
    QString currentEntry;

    for (const QString &rawLine : output.split(QStringLiteral("\n"))) {
        QString line = rawLine;
        if (line.startsWith(QStringLiteral("* "))) {
            if (!currentEntry.trimmed().isEmpty())
                entries << currentEntry.trimmed();
            currentEntry = line;
        } else {
            if (!currentEntry.isEmpty())
                currentEntry += QStringLiteral("\n") + line;
            else
                currentEntry = line;
        }
    }
    if (!currentEntry.trimmed().isEmpty())
        entries << currentEntry.trimmed();

    return entries;
}

// [removed D-Bus helper functions - replaced by dnf5 repoquery commands]

QVariantMap Dnf5DaemonClient::getPackageDetails(const QString &pkgName, const QString &nevra)
{
    QVariantMap details;
    Q_UNUSED(nevra)

    // Use the dnf5daemon D-Bus session for package detail queries.
    // The daemon already has metadata loaded in memory, so queries are
    // instant — no need to spawn dnf5 CLI processes that reload metadata
    // each time. We only fall back to CLI for changelogs which may not
    // be available via the D-Bus list method.
    if (m_sessionPath.isEmpty())
        return details;

    QVariantMap options;
    options[QStringLiteral("patterns")] = QStringList{pkgName};
    options[QStringLiteral("package_attrs")] = QStringList{
        QStringLiteral("name"), QStringLiteral("requires"),
        QStringLiteral("provides"), QStringLiteral("files"),
        QStringLiteral("description")
    };
    options[QStringLiteral("with_src")] = false;
    options[QStringLiteral("icase")] = true;
    options[QStringLiteral("latest-limit")] = 1;
    options[QStringLiteral("scope")] = QStringLiteral("all");

    QVariant result;
    QString error;
    if (callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                 QStringLiteral("list"), {QVariant(options)}, result, error)) {
        const QDBusArgument &arg = result.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariantMap pkgMap;
            arg >> pkgMap;

            // Extract requires / provides / files as string lists.
            // dnf5daemon returns these as arrays of strings (RelDep
            // string representations and file paths), which Qt
            // deserializes into QVariant (QStringList-compatible).
            auto toStringList = [](const QVariant &var) -> QStringList {
                if (!var.isValid() || var.isNull())
                    return {};
                // Try direct conversion first (works for a(s)).
                if (var.canConvert<QStringList>())
                    return var.toStringList();
                // Fallback: try iterating as a QDBusArgument array.
                if (var.canConvert<QDBusArgument>()) {
                    const QDBusArgument a = var.value<QDBusArgument>();
                    if (a.currentType() == QDBusArgument::ArrayType) {
                        QStringList list;
                        a.beginArray();
                        while (!a.atEnd()) {
                            QString s;
                            a >> s;
                            list << s;
                        }
                        a.endArray();
                        return list;
                    }
                }
                return {};
            };

            details[QStringLiteral("requires")] = toStringList(pkgMap.value(QStringLiteral("requires")));
            details[QStringLiteral("provides")] = toStringList(pkgMap.value(QStringLiteral("provides")));
            details[QStringLiteral("files")] = toStringList(pkgMap.value(QStringLiteral("files")));
            details[QStringLiteral("description")] = pkgMap.value(QStringLiteral("description")).toString();

            break; // Only need the first match
        }
        arg.endArray();
    }

    // Changelogs are fetched via CLI as the dnf5daemon list method
    // may not return changelog data reliably.
    details[QStringLiteral("changelogs")] = runDnf5RepoQueryChangelogs(pkgName);

    return details;
}

// === Package Operations ===

bool Dnf5DaemonClient::install(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("install"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::upgrade(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("upgrade"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::remove(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("remove"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::downgrade(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("downgrade"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::reinstall(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("reinstall"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::distroSync(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("distro_sync"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error);
}

bool Dnf5DaemonClient::systemUpgrade(const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("system_upgrade"),
                    {QVariant(options)}, result, error);
}

// === Goal Operations ===

Dnf5DaemonClient::ResolveResult Dnf5DaemonClient::resolve(bool allowErasing)
{
    ResolveResult result;
    if (m_sessionPath.isEmpty())
        return result;

    QVariantMap options;
    options[QStringLiteral("allow_erasing")] = allowErasing;

    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("resolve"), {QVariant(options)});
    if (msg.type() == QDBusMessage::ErrorMessage) {
        result.error = msg.errorMessage();
        qWarning() << "resolve failed:" << result.error;
        Q_EMIT errorOccurred(result.error);
        return result;
    }

    if (msg.arguments().size() >= 2) {
        // First argument: a(sssa{sv}a{sv}) — array of transaction item structs
        const QDBusArgument &itemsArg = msg.arguments().at(0).value<QDBusArgument>();
        itemsArg.beginArray();
        while (!itemsArg.atEnd()) {
            itemsArg.beginStructure();
            QString objectType;
            QString action;
            QString reason;
            QVariantMap attrs;
            QVariantMap object;
            itemsArg >> objectType >> action >> reason >> attrs >> object;
            itemsArg.endStructure();

            QVariantList item;
            item << objectType << action << reason << attrs << object;
            result.transactionItems.append(QVariant(item));
        }
        itemsArg.endArray();

        result.resultCode = msg.arguments().at(1).toUInt();
    }

    result.success = (result.resultCode <= 1);
    return result;
}

QStringList Dnf5DaemonClient::getTransactionProblemsString()
{
    QStringList problems;
    if (m_sessionPath.isEmpty())
        return problems;

    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("get_transaction_problems_string"));
    if (msg.type() == QDBusMessage::ReplyMessage) {
        const auto &args = msg.arguments();
        for (const auto &arg : args) {
            if (arg.canConvert<QStringList>())
                problems = arg.toStringList();
            else if (arg.canConvert<QString>())
                problems << arg.toString();
        }
    }
    return problems;
}

bool Dnf5DaemonClient::doTransaction(const QVariantMap &options)
{
    if (m_sessionPath.isEmpty())
        return false;

    QVariantMap opts = options;
    if (!opts.contains(QStringLiteral("comment")))
        opts[QStringLiteral("comment")] = QStringLiteral("Miryu Software Center Transaction");

    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("do_transaction"), {QVariant(opts)});
    if (msg.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "do_transaction failed:" << msg.errorMessage();
        Q_EMIT errorOccurred(msg.errorMessage());
        return false;
    }
    return true;
}

bool Dnf5DaemonClient::cancelTransaction()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("cancel"));
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

bool Dnf5DaemonClient::resetGoal()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("reset"));
    return msg.type() == QDBusMessage::ReplyMessage;
}

// === Offline Operations ===

QVariantMap Dnf5DaemonClient::offlineGetStatus()
{
    QVariantMap result;
    if (m_sessionPath.isEmpty())
        return result;

    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_OFFLINE),
                                  QStringLiteral("get_status"));
    if (msg.type() == QDBusMessage::ReplyMessage && msg.arguments().size() >= 2) {
        bool pending = msg.arguments().at(0).toBool();
        result[QStringLiteral("pending")] = pending;
        if (pending)
            result[QStringLiteral("status")] = msg.arguments().at(1);
    }
    return result;
}

bool Dnf5DaemonClient::offlineCancel()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_OFFLINE),
                                  QStringLiteral("cancel"));
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

bool Dnf5DaemonClient::offlineClean()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_OFFLINE),
                                  QStringLiteral("clean"), {QVariant(QVariantMap())});
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

bool Dnf5DaemonClient::offlineSetFinishAction(const QString &action)
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_OFFLINE),
                                  QStringLiteral("set_finish_action"), {QVariant(action)});
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

bool Dnf5DaemonClient::offlineScheduleForNextBoot()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_OFFLINE),
                                  QStringLiteral("schedule_for_next_boot"), {QVariant(QVariantMap())});
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

// === GPG Key ===

bool Dnf5DaemonClient::confirmKey(const QString &keyId, bool confirmed)
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_REPO),
                                  QStringLiteral("confirm_key"),
                                  {QVariant(keyId), QVariant(confirmed)});
    return msg.type() == QDBusMessage::ReplyMessage;
}

// === Advisory ===

QVariantList Dnf5DaemonClient::advisoryList(const QVariantMap &options)
{
    QVariantList result;
    if (m_sessionPath.isEmpty())
        return result;

    QVariant res;
    QString error;
    if (callSync(m_sessionPath, QString::fromLatin1(IFACE_ADVISORY),
                 QStringLiteral("list"), {QVariant(options)}, res, error)) {
        result = res.toList();
    }
    return result;
}

// === Groups ===

QVariantList Dnf5DaemonClient::groupList(const QVariantMap &options)
{
    QVariantList result;
    if (m_sessionPath.isEmpty())
        return result;

    QVariant res;
    QString error;
    if (callSync(m_sessionPath, QString::fromLatin1(IFACE_GROUP),
                 QStringLiteral("list"), {QVariant(options)}, res, error)) {
        result = res.toList();
    }
    return result;
}

// === Cache ===

bool Dnf5DaemonClient::cleanCache(const QString &cacheType)
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_BASE),
                                  QStringLiteral("clean"), {QVariant(cacheType)});
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

bool Dnf5DaemonClient::readAllRepos()
{
    if (m_sessionPath.isEmpty())
        return false;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_BASE),
                                  QStringLiteral("read_all_repos"));
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

} // namespace Miryu
