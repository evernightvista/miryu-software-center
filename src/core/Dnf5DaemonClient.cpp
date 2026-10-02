#include "Dnf5DaemonClient.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QDBusAbstractInterface>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
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

// How many times a fast-fail "Not connected"-style D-Bus error is retried
// after a silent reconnect. Each retry sleeps 200ms * attempt first, giving a
// restarting dbus-daemon / dnf5daemon-server time to come back before the
// call is attempted again.
static constexpr int kMaxDisconnectRetries = 3;
// A real timeout (InvalidMessage — the daemon did not reply within the 60s
// call timeout) is retried once only: repeatedly retrying a hung daemon
// would block the caller for minutes.
static constexpr int kMaxTimeoutRetries = 1;

// Build a user-actionable message for a D-Bus connection failure instead of
// surfacing the raw, terse "Not connected to D-Bus server" text to the user.
static QString friendlyDisconnectError(const QString &detail)
{
    return i18n("The connection to the system D-Bus / dnf5daemon-server was lost"
                " (\"%1\"). The service may have restarted.\n\n"
                "Please try the operation again. If the problem persists, restart"
                " the service with:\n"
                "  sudo systemctl restart dnf5daemon-server")
        .arg(detail);
}

// === Helpers for parsing daemon attribute replies ===

// Convert a QVariant (as deserialized from a JSON value or a D-Bus a(s)
// array) to a QStringList. Handles both direct QStringList conversion and a
// QDBusArgument array.
static QStringList stringListFromVariant(const QVariant &var)
{
    if (!var.isValid() || var.isNull())
        return {};
    if (var.canConvert<QStringList>())
        return var.toStringList();
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
    // Last resort: a JSON array of strings arrives as QVariantList.
    if (var.canConvert<QVariantList>()) {
        QStringList list;
        const QVariantList vl = var.toList();
        for (const QVariant &v : vl)
            list << v.toString();
        return list;
    }
    return {};
}



// Convert a dnf5daemon "changelogs" attribute (array of [timestamp, author,
// text] tuples, as returned through list_fd / list) into the
// "* date author\ntext" strings the UI expects. Mirrors yumex-ng's
// _get_changelog() formatting. The C locale keeps day/month names English
// regardless of the session locale (the old `dnf5 repoquery --changelogs`
// fallback was forced to LANG=C for the same reason).
static QStringList formatChangelogs(const QVariant &var)
{
    QStringList out;
    const QVariantList entries = var.toList();
    for (const QVariant &entryVar : entries) {
        const QVariantList entry = entryVar.toList();
        if (entry.size() < 3)
            continue;
        const qint64 ts = entry.at(0).toLongLong();
        const QString who = entry.at(1).toString();
        const QString what = entry.at(2).toString();
        if (ts <= 0)
            continue;
        const QString date = QLocale::c().toString(QDateTime::fromSecsSinceEpoch(ts),
                                                   QStringLiteral("ddd MMM d yyyy"));
        out << QStringLiteral("* %1 %2\n%3").arg(date, who, what);
    }
    return out;
}

// Build Package objects from attribute maps returned by dnf5daemon (either
// via list_fd JSON or the classic list() D-Bus reply).
static QList<Package> packagesFromMaps(const QList<QVariantMap> &maps, PackageFilter scope)
{
    QList<Package> packages;
    for (const QVariantMap &pkgMap : maps) {
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
    return packages;
}

// === Core D-Bus Communication ===
// All calls use QDBusConnection::call() with QDBusMessage directly.
// This completely bypasses QDBusInterface and its introspection mechanism,
// which fails against dnf5daemon-server because the daemon does not implement
// the org.freedesktop.DBus.Introspectable interface at its object paths.

QDBusMessage Dnf5DaemonClient::callMethod(const QString &path, const QString &interface,
                                          const QString &method, const QList<QVariant> &args,
                                          int timeoutMs, QDBus::CallMode mode)
{
    // Self-heal a lost bus connection BEFORE taking the call mutex.
    // ensureConnected() may invoke reconnect(), which (for worker-thread
    // callers) marshals itself onto the main thread and acquires m_callMutex
    // there. If we held the mutex here, the main thread would deadlock.
    // The short window between ensureConnected() and the mutex below is
    // harmless: a concurrent reconnect just replaces m_bus, and callSync()'s
    // retry loop recovers from the resulting "Not connected" failure.
    ensureConnected();

    // Serialize all D-Bus traffic: the same session is shared by concurrent
    // QtConcurrent workers, and a reconnect() must never run while another
    // thread is mid-call on the (soon to be replaced) connection.
    QMutexLocker locker(&m_callMutex);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(BUS_NAME),
        path,
        interface,
        method);
    if (!args.isEmpty())
        msg.setArguments(args);
    // Use a caller-provided timeout: quick queries get the default 60s, while
    // polkit-gated / long-running operations (transactions, metadata refresh,
    // large Rpm.list replies) get the long 20-minute timeout — mirroring the
    // yumex-ng client, which uses a 20-minute async D-Bus timeout.
    //
    // The default mode is QDBus::Block, which is correct for calls made from
    // worker threads (resolve, list, …) that don't need to dispatch GUI
    // signals. do_transaction does NOT go through callMethod: it calls
    // m_bus.call() directly with QDBus::BlockWithGui (see doTransaction()) so
    // the daemon's progress signals reach the ProgressDialog instead of
    // leaving it stuck at "Preparing... 0%".
    return m_bus.call(msg, mode, timeoutMs);
}

bool Dnf5DaemonClient::ensureConnected()
{
    if (m_bus.isConnected()) {
        m_connected = true;
        return true;
    }
    return reconnect();
}

bool Dnf5DaemonClient::reconnect()
{
    // The actual reconnection (connectToBus + openSession) MUST run on the
    // thread that owns this object (the GUI/main thread). QDBusConnection
    // binds the new connection's socket watcher to the calling thread; a
    // worker thread has no running Qt event loop, so any connection created
    // there would never receive D-Bus signals or async replies. If we are
    // already on the owner thread, run directly. Otherwise marshal to it via
    // a blocking queued invocation.
    //
    // m_callMutex must NOT be held by the caller here: doReconnect() acquires
    // it itself, and if a worker thread held it while waiting for the main
    // thread, the main thread would deadlock on the same mutex.
    if (QThread::currentThread() != thread()) {
        bool ok = false;
        QMetaObject::invokeMethod(this, [this, &ok]() { ok = doReconnect(); },
                                  Qt::BlockingQueuedConnection);
        return ok;
    }
    return doReconnect();
}

bool Dnf5DaemonClient::doReconnect()
{
    // Must not run concurrently with another thread's D-Bus call or another
    // reconnect — replacing m_bus / clearing m_sessionPath mid-use would
    // corrupt the client state and produce spurious "Not connected" errors.
    QMutexLocker locker(&m_callMutex);

    qWarning() << "Reconnecting to the system D-Bus...";

    // Drop signal wiring on the (dead) old connection before replacing it.
    disconnectSignals();
    m_sessionPath.clear();

    // QDBusConnection::systemBus() returns the process-wide cached internal
    // connection, which Qt does NOT reconnect after it drops. Opening a fresh
    // connection via connectToBus() creates a brand-new socket to the system
    // bus, so a restarted dbus-daemon / dnf5daemon-server is picked up.
    ++m_reconnectCount;
    m_bus = QDBusConnection::connectToBus(
        QDBusConnection::SystemBus,
        QStringLiteral("miryu-dnf5-%1").arg(m_reconnectCount));
    m_connected = m_bus.isConnected();

    if (!m_connected) {
        qWarning() << "Fresh system bus connection failed:" << m_bus.lastError().message();
        return false;
    }

    // Re-activate dnf5daemon-server on the freshly connected bus.
    QDBusConnectionInterface *iface = m_bus.interface();
    if (iface && !iface->isServiceRegistered(QString::fromLatin1(BUS_NAME))) {
        QDBusReply<void> reply = iface->startService(QString::fromLatin1(BUS_NAME));
        if (!reply.isValid())
            qWarning() << "Re-activation of dnf5daemon-server failed:" << reply.error().message();
    }

    // The previous session object no longer exists after a daemon restart;
    // open a new one (this also re-wires the per-session signal connections).
    // Recovery must stay silent: a session re-open failure here is expected
    // while the daemon is still starting. Blocking signals keeps it from
    // popping a scary "Not connected to D-Bus server" dialog in the middle of
    // the self-heal; the callers (callSync() / openSession()) report the
    // final error only after their own retries are exhausted.
    const bool wasBlocked = blockSignals(true);
    openSession();
    blockSignals(wasBlocked);
    return m_connected;
}

bool Dnf5DaemonClient::looksLikeDisconnect(const QDBusMessage &m)
{
    // InvalidMessage = no reply within the call timeout; the daemon may have
    // died mid-call or be restarting.
    if (m.type() == QDBusMessage::InvalidMessage)
        return true;
    if (m.type() != QDBusMessage::ErrorMessage)
        return false;
    const QString em = m.errorMessage();
    return em.contains(QLatin1String("Not connected to D-Bus"), Qt::CaseInsensitive) ||
           em.contains(QLatin1String("NoReply"), Qt::CaseInsensitive) ||
           em.contains(QLatin1String("ServiceUnknown"), Qt::CaseInsensitive) ||
           em.contains(QLatin1String("disconnected"), Qt::CaseInsensitive);
}

// Detect polkit authentication failures (user cancelled the auth dialog or
// was not authorized). These errors must NOT be re-broadcast via
// errorOccurred() because the caller already handles them and would pop a
// second "not authorized" dialog on top of the caller's own message.
static bool isAuthError(const QString &error)
{
    const QString e = error.toLower();
    return e.contains(QStringLiteral("not authorized")) ||
           e.contains(QStringLiteral("notauthorised")) ||
           e.contains(QStringLiteral("polkit")) ||
           e.contains(QStringLiteral("policykit")) ||
           e.contains(QStringLiteral("authentication required")) ||
           e.contains(QStringLiteral("auth cancelled")) ||
           e.contains(QStringLiteral("auth dialog dismissed")) ||
           e.contains(QStringLiteral("cancelled"));
}

bool Dnf5DaemonClient::callSync(const QString &path, const QString &interface,
                                 const QString &method, const QList<QVariant> &args,
                                 QVariant &result, QString &error, int timeoutMs)
{
    QDBusMessage msg = callMethod(path, interface, method, args, timeoutMs);

    // A transient "not connected" / timeout can happen when the bus or the
    // daemon just restarted (e.g. after a transaction upgraded dbus,
    // dnf5daemon-server or systemd). Re-acquire the connection and retry with
    // a short backoff before giving up, so a scary, user-facing "Not connected
    // to D-Bus server" dialog is turned into a silent recovery. Recovery never
    // emits errorOccurred() itself — only the final failure after all retries
    // is reported, and even then with an actionable message.
    // Note: we call reconnect() directly (not ensureConnected()) because the
    // cached handle can report isConnected()==true while every actual send
    // fails.
    const int maxRetries = (msg.type() == QDBusMessage::InvalidMessage)
        ? kMaxTimeoutRetries : kMaxDisconnectRetries;
    for (int attempt = 1; attempt <= maxRetries && looksLikeDisconnect(msg); ++attempt) {
        qWarning() << "DBus call" << interface << method << "failed (attempt" << attempt
                   << "of" << maxRetries << "):" << msg.errorMessage();
        // Give a restarting bus / daemon a moment before retrying — an
        // immediate retry usually hits the same recovery window.
        QThread::msleep(200 * attempt);
        if (!reconnect()) {
            qWarning() << "Reconnect failed; giving up on" << interface << method;
            break;
        }
        msg = callMethod(path, interface, method, args, timeoutMs);
    }

    // Handle all error message types
    if (msg.type() == QDBusMessage::ErrorMessage) {
        error = msg.errorMessage();
        qWarning() << "DBus call failed:" << interface << method << ":" << error;
        // Replace a raw, unhelpful "Not connected to D-Bus server" with an
        // actionable message when the failure is (still) a disconnect.
        if (looksLikeDisconnect(msg))
            error = friendlyDisconnectError(error);
        // Polkit/auth failures are handled by the caller (which shows its own
        // user-facing message). Do NOT emit errorOccurred here, otherwise
        // MainWindow::onError would pop a second dialog on top of the
        // caller's.
        if (!isAuthError(error))
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

    // NOTE: QDBusConnection has no disconnected() signal to hook; a dropped
    // connection is detected lazily when a call fails with "Not connected",
    // at which point callSync() forces reconnect().

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
    // The cached m_connected flag can go stale (e.g. the bus dropped after
    // construction or after a system upgrade). Re-acquire the bus first so a
    // transient outage self-heals instead of failing the session open
    // immediately.
    if (!ensureConnected()) {
        qWarning() << "Dnf5DaemonClient: not connected to bus";
        m_lastError = i18n("Cannot connect to the D-Bus system bus.\n\n"
                           "Please make sure the D-Bus system bus is running "
                           "(dbus-broker / dbus-daemon).");
        Q_EMIT errorOccurred(m_lastError);
        return false;
    }

    // If a session was already opened while re-acquiring the bus
    // (ensureConnected() → reconnect() → openSession()), reuse it instead of
    // opening a second one (which would leak the first).
    if (!m_sessionPath.isEmpty())
        return true;

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
    QDBusMessage msg;

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        qInfo() << "open_session attempt" << attempt << "of" << maxRetries;

        // Call open_session via raw QDBusMessage — no QDBusInterface, no introspection.
        msg = callMethod(QString::fromLatin1(OBJECT_PATH),
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

        // If this is a transient service/bus error, retry after a delay.
        // For other errors (e.g., auth failures), no point retrying.
        bool isRetryable = looksLikeDisconnect(msg) ||
                           m_lastError.contains(QStringLiteral("not provided"), Qt::CaseInsensitive);

        if (attempt < maxRetries && isRetryable) {
            QThread::msleep(500);
            continue;
        }

        // Non-retryable error or out of retries — report the actual D-Bus
        // error so the user sees what really went wrong.
        break;
    }

    // Emit the actual D-Bus error so the user sees what really went wrong
    if (m_lastError.isEmpty()) {
        m_lastError = QStringLiteral("Failed to open a session with dnf5daemon-server. "
                                    "The service may not be running or may not be reachable "
                                    "via the system D-Bus. Try:\n"
                                    "  sudo systemctl start dnf5daemon-server\n"
                                    "  sudo systemctl enable dnf5daemon-server");
    } else if (looksLikeDisconnect(msg)) {
        // The raw error is a terse disconnect message; replace it with an
        // actionable one.
        m_lastError = friendlyDisconnectError(m_lastError);
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

bool Dnf5DaemonClient::reopenSession()
{
    // Hold the call mutex for the whole dance so no concurrent QtConcurrent
    // worker can race between our teardown and the new openSession().
    QMutexLocker locker(&m_callMutex);

    // Mirror reconnect()'s teardown: drop signal subscriptions wired on the
    // old session path and forget that path. We deliberately do NOT call
    // close_session on the (possibly dead) path: after a transaction that
    // restarted dnf5daemon-server that path is invalid, and calling it via
    // callMethod() would force ensureConnected() -> reconnect() to open a
    // new session that close_session would then immediately destroy. The
    // daemon garbage-collects orphaned sessions on client disconnect, so
    // simply forgetting the path is safe.
    disconnectSignals();
    m_sessionPath.clear();

    // openSession() re-acquires the bus (ensureConnected() -> reconnect()
    // when the system bus itself dropped, which is rarer) and opens a fresh
    // session on it. Block signals so a failure here (the daemon may still
    // be restarting) cannot pop a scary errorOccurred dialog — the caller's
    // subsequent callSync()-based queries will still recover per-call as a
    // fallback.
    const bool wasBlocked = blockSignals(true);
    const bool ok = openSession();
    blockSignals(wasBlocked);
    return ok;
}

bool Dnf5DaemonClient::restartDaemonServer()
{
    // 1) Restart the dnf5daemon-server systemd unit. The shipped polkit
    //    rules file (data/50-miryu-dnf5daemon.rules) allows active local
    //    users to restart ONLY dnf5daemon-server.service without an
    //    authentication prompt, so `systemctl restart` completes without a
    //    polkit dialog. systemctl can be missing (non-systemd) or the unit
    //    can be unavailable; that is fine — the reconnect below still heals
    //    the stale session path, and the daemon re-registers itself via
    //    D-Bus activation when the first query comes in.
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    proc.start(QStringLiteral("systemctl"),
               {QStringLiteral("restart"), QStringLiteral("dnf5daemon-server")});
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        proc.waitForFinished(5000);
        qWarning() << "systemctl restart dnf5daemon-server timed out; "
                      "reconnecting to D-Bus anyway";
    } else if (proc.exitCode() != 0) {
        qWarning() << "systemctl restart dnf5daemon-server failed (exit"
                   << proc.exitCode() << "):"
                   << QString::fromUtf8(proc.readAllStandardError()).trimmed()
                   << "- reconnecting to D-Bus anyway";
    }

    // 2) Wait briefly for the daemon to re-register on the bus after the
    //    restart (systemd returns as soon as the unit is active; the D-Bus
    //    name may take a moment longer). If the transaction upgraded dbus
    //    itself, the bus is gone too — the poll then simply runs its course
    //    and the reconnect below opens a brand-new system-bus connection.
    for (int i = 0; i < 50; ++i) {
        QDBusConnectionInterface *iface = m_bus.interface();
        if (iface && iface->isServiceRegistered(QString::fromLatin1(BUS_NAME)))
            break;
        QThread::msleep(200);
    }

    // 3) Promptly re-acquire the D-Bus connection and open a fresh session.
    //    The old session path died with the restarted daemon process, so
    //    every subsequent D-Bus query would otherwise fail against the stale
    //    handle — or crash the app while unwinding a signal emitted on a
    //    dead session. reconnect() stays silent on transient failure (the
    //    daemon may still be starting) and the per-call callSync() recovery
    //    remains as a final fallback.
    return reconnect();
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
    // Record the authoritative outcome reported by the daemon. If the
    // do_transaction method reply is subsequently lost, doTransaction()
    // falls back to this value so a genuinely successful transaction is
    // not reported as a failure.
    m_transactionCompleted = true;
    m_transactionSuccess = success;
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
    // of a bare "pkexec" title. The daemon blocks the call while the user
    // authenticates (hardcoded 2-minute polkit window), so use the long
    // timeout.
    QStringList repoIds{repoId};
    QString method = enabled ? QStringLiteral("enable") : QStringLiteral("disable");

    QVariant result;
    QString error;
    if (!callSync(m_sessionPath, QString::fromLatin1(IFACE_REPO),
                  method, {QVariant(repoIds)}, result, error,
                  kLongCallTimeoutMs)) {
        m_lastError = error;
        return false;
    }
    return true;
}

// === Package Queries ===

// Build the a{sv} options map used by the Rpm.list D-Bus method.
static QVariantMap buildRpmListOptions(const QStringList &patterns,
                                       const QStringList &packageAttrs,
                                       const QString &scope,
                                       const QVariantMap &extraOptions)
{
    QVariantMap options;
    options[QStringLiteral("patterns")] = patterns;
    options[QStringLiteral("package_attrs")] = packageAttrs;
    options[QStringLiteral("with_src")] = false;
    // Disable provides / filename / binary pattern matching, as the reference
    // yumex-ng client does: the patterns we issue are name-based globs ("*",
    // "*firefox*"), and matching them against every package's provides / file
    // list is pure waste on large queries. The with_* flags only affect
    // pattern matching, not which attributes are returned.
    options[QStringLiteral("with_provides")] = false;
    options[QStringLiteral("with_filenames")] = false;
    options[QStringLiteral("with_binaries")] = false;
    options[QStringLiteral("icase")] = true;
    // NOTE: the dnf5daemon option key is "latest-limit" (hyphen), not
    // "latest_limit" (underscore). Default is 1 (newest EVR per name.arch),
    // matching the reference yumex-ng client: the browser still lists every
    // available package, but each package appears once with its newest
    // version, so the result set stays small. latest-limit=0 (return EVERY
    // version candidate of every package) multiplies the result set by the
    // number of versions/repos per package — on a full "all packages" query
    // that is tens of thousands of records, which balloons the daemon's
    // D-Bus marshalling and memory footprint (observed: dnf5daemon-server
    // RSS climbing past 1 GB after a search + clear cycle) and makes the
    // reply slow to build. Callers that genuinely need all candidates pass
    // latest-limit=0 explicitly. Accept both spellings so a caller cannot
    // silently lose its value.
    options[QStringLiteral("latest-limit")] =
        extraOptions.value(QStringLiteral("latest-limit"),
                           extraOptions.value(QStringLiteral("latest_limit"), 1));
    options[QStringLiteral("scope")] = scope;

    // repo / arch must be arrays of strings ("as") per the D-Bus API: a bare
    // QString would marshal as "s" and make the daemon reject (or silently
    // ignore) the filter. Accept both QStringList and a single QString.
    if (extraOptions.contains(QStringLiteral("repo"))) {
        const QVariant repo = extraOptions.value(QStringLiteral("repo"));
        if (repo.canConvert<QStringList>())
            options[QStringLiteral("repo")] = repo.toStringList();
        else if (repo.canConvert<QString>())
            options[QStringLiteral("repo")] = QStringList{repo.toString()};
    }
    if (extraOptions.contains(QStringLiteral("arch"))) {
        const QVariant arch = extraOptions.value(QStringLiteral("arch"));
        if (arch.canConvert<QStringList>())
            options[QStringLiteral("arch")] = arch.toStringList();
        else if (arch.canConvert<QString>())
            options[QStringLiteral("arch")] = QStringList{arch.toString()};
    }
    return options;
}

QList<QVariantMap> Dnf5DaemonClient::rpmListViaDbus(const QVariantMap &options)
{
    QList<QVariantMap> maps;
    if (m_sessionPath.isEmpty())
        return maps;

    QVariant result;
    QString error;
    // The classic list() marshals the whole reply over D-Bus and may return a
    // large package set on the legacy fallback path, so give it the long
    // timeout.
    if (!callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                  QStringLiteral("list"), {QVariant(options)}, result, error,
                  kLongCallTimeoutMs)) {
        return maps;
    }

    const QDBusArgument &arg = result.value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QVariantMap pkgMap;
        arg >> pkgMap;
        maps.append(pkgMap);
    }
    arg.endArray();
    return maps;
}

QList<Package> Dnf5DaemonClient::packageList(const QStringList &patterns,
                                             const QStringList &packageAttrs,
                                             PackageFilter scope,
                                             const QVariantMap &extraOptions)
{
    QList<Package> packages;
    if (m_sessionPath.isEmpty())
        return packages;

    QString scopeStr;
    switch (scope) {
    case PackageFilter::All:        scopeStr = QStringLiteral("all"); break;
    case PackageFilter::Installed:  scopeStr = QStringLiteral("installed"); break;
    case PackageFilter::Available:  scopeStr = QStringLiteral("available"); break;
    case PackageFilter::Updates:    scopeStr = QStringLiteral("upgrades"); break;
    case PackageFilter::Upgradable: scopeStr = QStringLiteral("upgradable"); break;
    }

    // Preferred transport: the classic Rpm.list D-Bus method — the same
    // simple, synchronous path the upstream miryu-software-center uses and
    // the one verified to list every available package on real systems (the
    // reference build that "correctly lists all available rpm packages" uses
    // this exact call). The list_fd pipe transport added in earlier revisions
    // was retired: its asynchronous pipe + 30 s server write-window semantics
    // proved fragile against slow metadata loads and repeatedly ended in
    // "The package list transfer from dnf5daemon-server was interrupted or
    // timed out." Rpm.list has no pipe, no per-chunk timeout and no async
    // window — the whole reply arrives within the call timeout below.
    return packagesFromMaps(rpmListViaDbus(buildRpmListOptions(patterns, packageAttrs, scopeStr, extraOptions)),
                            scope);
}

// === Package Details ===

// Helper: run dnf5 repoquery synchronously and return output lines.
// Helper: run dnf5 repoquery --changelogs and parse multi-line entries.
//
// Changelogs of *available* (not-yet-installed) packages come from the
// repository's "other" metadata (other.xml), which libdnf5 does NOT load by
// default (optional_metadata_types defaults to comps,updateinfo) — neither in
// dnf5daemon-server nor in the plain dnf5 CLI. The --setopt below explicitly
// asks this one-off CLI process to load "other" so the query actually returns
// changelogs. The process exits right after the query, so the extra metadata
// never becomes resident in dnf5daemon-server's memory.
static QStringList runDnf5RepoQueryChangelogs(const QString &pkgName)
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LANG"), QStringLiteral("C"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    proc.setProcessEnvironment(env);

    QStringList args = {QStringLiteral("repoquery"),
                        QStringLiteral("--changelogs"),
                        // Load the repo "other" metadata (package changelogs)
                        // for this process. Keep the defaults so comps /
                        // updateinfo behaviour is unchanged.
                        QStringLiteral("--setopt=optional_metadata_types=comps,updateinfo,other"),
                        QStringLiteral("--quiet"),
                        pkgName};

    proc.start(QStringLiteral("dnf5"), args, QIODevice::ReadOnly);
    // The first run may have to download other.xml (tens of MB), so allow a
    // generous window instead of the default 30 s.
    if (!proc.waitForFinished(120000)) {
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

QVariantMap Dnf5DaemonClient::getPackageDetails(const QString &pkgName, const QString &nevra)
{
    QVariantMap details;
    Q_UNUSED(nevra)

    // Use the dnf5daemon D-Bus session for package detail queries.
    // The daemon already has metadata loaded in memory, so queries are
    // instant — no need to spawn dnf5 CLI processes that reload metadata
    // each time. The changelogs attribute is requested from the daemon
    // directly (as the reference yumex-ng client does); the CLI fallback is
    // kept only for daemons that do not populate it.
    if (m_sessionPath.isEmpty())
        return details;

    const QVariantMap options = buildRpmListOptions(
        QStringList{pkgName},
        QStringList{QStringLiteral("name"), QStringLiteral("requires"),
                    QStringLiteral("provides"), QStringLiteral("files"),
                    QStringLiteral("description"), QStringLiteral("changelogs")},
        QStringLiteral("all"),
        {{QStringLiteral("latest_limit"), 1}});

    // Classic Rpm.list D-Bus method — same reasoning as packageList(): the
    // synchronous call is what the upstream/verified build uses, and it has
    // none of the list_fd pipe transport's timeout fragility.
    const QList<QVariantMap> maps = rpmListViaDbus(options);

    if (!maps.isEmpty()) {
        const QVariantMap &pkgMap = maps.first();

        details[QStringLiteral("requires")] = stringListFromVariant(pkgMap.value(QStringLiteral("requires")));
        details[QStringLiteral("provides")] = stringListFromVariant(pkgMap.value(QStringLiteral("provides")));
        details[QStringLiteral("files")] = stringListFromVariant(pkgMap.value(QStringLiteral("files")));
        details[QStringLiteral("description")] = pkgMap.value(QStringLiteral("description")).toString();
        // dnf5daemon returns changelogs as [timestamp, author, text] tuples;
        // format them the way the CLI fallback used to, so the UI stays the
        // same. Mirrors yumex-ng's _get_changelog().
        details[QStringLiteral("changelogs")] = formatChangelogs(pkgMap.value(QStringLiteral("changelogs")));
    }

    // Fallback: some daemon versions / metadata do not populate changelogs
    // through the D-Bus API — fetch them via `dnf5 repoquery --changelogs`.
    if (details.value(QStringLiteral("changelogs")).toStringList().isEmpty())
        details[QStringLiteral("changelogs")] = runDnf5RepoQueryChangelogs(pkgName);

    return details;
}

// === Package Operations ===

bool Dnf5DaemonClient::install(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    // Goal-setting calls may block on polkit user interaction (hardcoded
    // 2-minute window), so use the long timeout.
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("install"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::upgrade(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("upgrade"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::remove(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("remove"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::downgrade(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("downgrade"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::reinstall(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("reinstall"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::distroSync(const QStringList &pkgSpecs, const QVariantMap &options)
{
    QVariant result;
    QString error;
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("distro_sync"),
                    {QVariant(pkgSpecs), QVariant(options)}, result, error,
                    kLongCallTimeoutMs);
}

bool Dnf5DaemonClient::systemUpgrade(const QVariantMap &options)
{
    QVariant result;
    QString error;
    // system_upgrade prepares a full release upgrade — long-running work plus
    // possible polkit interaction; use the long timeout.
    return callSync(m_sessionPath, QString::fromLatin1(IFACE_RPM),
                    QStringLiteral("system_upgrade"),
                    {QVariant(options)}, result, error, kLongCallTimeoutMs);
}

// === Goal Operations ===

Dnf5DaemonClient::ResolveResult Dnf5DaemonClient::resolve(bool allowErasing)
{
    ResolveResult result;
    if (m_sessionPath.isEmpty())
        return result;

    QVariantMap options;
    options[QStringLiteral("allow_erasing")] = allowErasing;

    // Resolving a large transaction (e.g. a full distro-sync) can take a
    // while; use the long timeout, mirroring yumex-ng's 20-minute async call.
    //
    // callMethod() invokes ensureConnected() internally, which may silently
    // reconnect() and open a brand-new session whose goal is empty. If that
    // happens, the resolve call below runs against the fresh (goal-less)
    // session and the daemon returns a successful but *empty* transaction
    // (resultCode 0, no items) — the misleading "0 B / empty summary" bug.
    // Detect the mid-call reconnect by comparing the reconnect counter before
    // and after callMethod(); on a mismatch surface an actionable error so the
    // caller re-runs the whole transaction (the goal-setting callSync steps
    // then self-heal and rebuild the goal on the fresh session).
    const int reconnectCountBefore = m_reconnectCount;
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_GOAL),
                                  QStringLiteral("resolve"), {QVariant(options)},
                                  kLongCallTimeoutMs);

    if (m_reconnectCount != reconnectCountBefore) {
        result.error = friendlyDisconnectError(
            QStringLiteral("The dnf5daemon-server session was reset while resolving the transaction. "
                           "The prepared package changes were lost; please try again."));
        qWarning() << "resolve: session reconnected during call"
                   << "(before=" << reconnectCountBefore
                   << "after=" << m_reconnectCount << ")";
        Q_EMIT errorOccurred(result.error);
        return result;
    }

    // NOTE: resolve() deliberately does NOT retry after a reconnect. The goal
    // (install/upgrade/remove specs) is bound to the session, and reconnect()
    // opens a brand-new session whose goal is empty — retrying resolve there
    // would report a misleading empty transaction ("0 B / empty summary").
    // On a disconnect we surface an actionable error and let the caller
    // re-run the whole transaction: the goal-setting step (callSync) then
    // self-heals and rebuilds the goal on the fresh session.
    if (msg.type() == QDBusMessage::ErrorMessage) {
        result.error = msg.errorMessage();
        qWarning() << "resolve failed:" << result.error;
        if (looksLikeDisconnect(msg))
            result.error = friendlyDisconnectError(result.error);
        Q_EMIT errorOccurred(result.error);
        return result;
    }

    // InvalidMessage means the call got no usable reply (e.g. the connection
    // dropped mid-flight or the daemon vanished without a clean D-Bus error).
    // Without this guard msg.arguments() is empty, resultCode stays 0 and we
    // would wrongly report success with an empty transaction — the "0 B /
    // empty summary" bug. Surface an actionable error instead.
    if (msg.type() != QDBusMessage::ReplyMessage) {
        result.error = friendlyDisconnectError(
            QStringLiteral("The dnf5daemon-server did not return a valid transaction summary. "
                           "Please try again."));
        qWarning() << "resolve: unexpected reply type" << static_cast<int>(msg.type());
        Q_EMIT errorOccurred(result.error);
        return result;
    }

    if (msg.arguments().size() < 2) {
        result.error = friendlyDisconnectError(
            QStringLiteral("The dnf5daemon-server returned a malformed transaction summary. "
                           "Please try again."));
        qWarning() << "resolve: reply has" << msg.arguments().size() << "arguments (expected >= 2)";
        Q_EMIT errorOccurred(result.error);
        return result;
    }

    {
        // First argument: a(sssa{sv}a{sv}) — array of transaction item structs.
        // Qt returns complex container types as a QDBusArgument that must be
        // walked manually. Guard against a mis-demmarshalled argument (which
        // would make value<QDBusArgument>() return an empty arg and silently
        // produce zero items).
        const QVariant itemsVariant = msg.arguments().at(0);
        if (!itemsVariant.canConvert<QDBusArgument>()) {
            result.error = friendlyDisconnectError(
                QStringLiteral("The dnf5daemon-server returned a transaction summary "
                               "in an unexpected format. Please try again."));
            qWarning() << "resolve: arg0 cannot convert to QDBusArgument, typeName="
                       << itemsVariant.typeName();
            Q_EMIT errorOccurred(result.error);
            return result;
        }
        const QDBusArgument &itemsArg = itemsVariant.value<QDBusArgument>();
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

    // do_transaction downloads and installs the resolved package set — this
    // can easily exceed the default 60s (a large kernel / desktop update
    // downloads for minutes). Use the long timeout, mirroring yumex-ng's
    // 20-minute call.
    //
    // HOW PROGRESS SIGNALS REACH THE UI (mirroring yumex-ng):
    // yumex-ng calls do_transaction with reply_handler/error_handler and runs
    // a GLib.MainLoop while waiting; that loop dispatches the daemon's
    // download_progress / transaction_action_* signals to the UI. The Qt
    // equivalent is QDBus::BlockWithGui: it blocks for the reply but keeps
    // pumping the Qt event loop, so every D-Bus signal the daemon emits
    // during the transaction is delivered to our slots and forwarded to the
    // ProgressDialog. (QDBus::Block, by contrast, does NOT dispatch signals,
    // which is why the dialog used to be stuck at "Preparing...".)
    //
    // doTransaction() is always invoked on the GUI/main thread (Transaction-
    // Manager marshals it via Qt::BlockingQueuedConnection), so BlockWithGui
    // is valid here.
    //
    // The resolved transaction is bound to the session that resolve() ran on.
    // A reconnect during the call would open a fresh session with no resolved
    // transaction, so the daemon returns "Transaction has to be resolved
    // first." Detect a reconnect and report a clear, actionable error.
    const int reconnectCountBefore = m_reconnectCount;

    // Reset the authoritative transaction-outcome record. The daemon emits
    // TransactionAfterComplete(success) right before it sends the method
    // reply; if the reply is lost in transit (transient D-Bus hiccup, heavy
    // system I/O stalling the bus, …) we still have this signal to decide
    // the real outcome. See onTransactionAfterComplete().
    m_transactionCompleted = false;
    m_transactionSuccess = false;

    // Self-heal the connection BEFORE taking the call mutex (see callMethod
    // for why: reconnect() marshals to the main thread and acquires the mutex
    // itself, so holding it here would deadlock). doTransaction runs on the
    // main thread, so reconnect() executes doReconnect() inline.
    ensureConnected();

    QDBusMessage reply;
    {
        QMutexLocker locker(&m_callMutex);
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QString::fromLatin1(BUS_NAME),
            m_sessionPath,
            QString::fromLatin1(IFACE_GOAL),
            QStringLiteral("do_transaction"));
        msg.setArguments({QVariant(opts)});
        reply = m_bus.call(msg, QDBus::BlockWithGui, kLongCallTimeoutMs);
    }

    // If the method reply was lost but the daemon's TransactionAfterComplete
    // signal reported success, the transaction genuinely ran to completion —
    // do not surface a false failure. This is the common case behind
    // "dnf5daemon-server says success but the app says the transaction
    // failed": a long do_transaction (minutes of downloading + installing)
    // can lose its D-Bus reply while the daemon still emits the completion
    // signal. Trust the signal.
    auto daemonReportedSuccess = [this]() {
        return m_transactionCompleted && m_transactionSuccess;
    };

    if (m_reconnectCount != reconnectCountBefore) {
        if (daemonReportedSuccess()) {
            qWarning() << "do_transaction: session reconnected during call"
                       << "(before=" << reconnectCountBefore
                       << "after=" << m_reconnectCount
                       << ") but daemon reported success via"
                       << "TransactionAfterComplete; treating as success";
            return true;
        }
        QString error = friendlyDisconnectError(
            QStringLiteral("The dnf5daemon-server session was reset while running the transaction. "
                           "The resolved transaction was lost; please try again."));
        qWarning() << "do_transaction: session reconnected during call"
                   << "(before=" << reconnectCountBefore
                   << "after=" << m_reconnectCount << ")";
        Q_EMIT errorOccurred(error);
        return false;
    }

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (daemonReportedSuccess()) {
            qWarning() << "do_transaction: D-Bus reply was an error ("
                       << reply.errorMessage()
                       << ") but daemon reported success via"
                       << "TransactionAfterComplete; treating as success";
            return true;
        }
        QString error = reply.errorMessage();
        qWarning() << "do_transaction failed:" << error;
        // do_transaction executes the goal that was resolved on THIS session;
        // a reconnect would open an empty session, so there is nothing to
        // auto-retry. Report the failure with an actionable message instead
        // of the raw "Not connected to D-Bus server" text.
        if (looksLikeDisconnect(reply))
            error = friendlyDisconnectError(error);
        // Store the real error so runTransaction() can surface it to the UI
        // instead of a generic "Transaction execution failed".
        m_lastError = error;
        // Polkit/auth failures are handled by the caller's own user-facing
        // message; do not emit errorOccurred or MainWindow::onError would
        // stack a second dialog on top of the caller's.
        if (!isAuthError(error))
            Q_EMIT errorOccurred(error);
        return false;
    }

    // Same InvalidMessage / non-reply guard as resolve(): a dropped connection
    // that yields no usable reply must not be reported as success — UNLESS the
    // daemon's TransactionAfterComplete signal confirms the transaction
    // succeeded (the reply was merely lost in transit).
    if (reply.type() != QDBusMessage::ReplyMessage) {
        if (daemonReportedSuccess()) {
            qWarning() << "do_transaction: unexpected reply type"
                       << static_cast<int>(reply.type())
                       << "but daemon reported success via"
                       << "TransactionAfterComplete; treating as success";
            return true;
        }
        QString error = friendlyDisconnectError(
            QStringLiteral("The dnf5daemon-server did not confirm the transaction. "
                           "Please try again."));
        qWarning() << "do_transaction: unexpected reply type" << static_cast<int>(reply.type());
        Q_EMIT errorOccurred(error);
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
    // confirm_key is polkit-gated (hardcoded 2-minute auth window), so use
    // the long timeout.
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_REPO),
                                  QStringLiteral("confirm_key"),
                                  {QVariant(keyId), QVariant(confirmed)},
                                  kLongCallTimeoutMs);
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
    // read_all_repos explicitly loads repository metadata; on a cold / expired
    // cache it downloads metadata and can run for minutes, so use the long
    // timeout.
    QDBusMessage msg = callMethod(m_sessionPath, QString::fromLatin1(IFACE_BASE),
                                  QStringLiteral("read_all_repos"), {},
                                  kLongCallTimeoutMs);
    return msg.type() == QDBusMessage::ReplyMessage &&
           !msg.arguments().isEmpty() && msg.arguments().first().toBool();
}

} // namespace Miryu
