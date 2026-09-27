#include "FlatpakBackend.h"

#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QDebug>
#include <QtConcurrent>
#include <QJsonObject>
#include <QRegularExpression>
#include <QLatin1Char>
#include <memory>

namespace Miryu {

// Column specifications used when invoking flatpak.
// Using --columns= ensures deterministic, tab-separated output.
static const char *const kListColumns =
    "name,application,version,branch,arch,origin,installation,size,description";
static const char *const kSearchColumns =
    "name,application,version,branch,remote,description";

FlatpakBackend::FlatpakBackend(QObject *parent)
    : QObject(parent)
    , m_available(false)
{
    qRegisterMetaType<FlatpakApp>("Miryu::FlatpakApp");
    qRegisterMetaType<QList<FlatpakApp>>("QList<Miryu::FlatpakApp>");

    checkAvailability();
}

void FlatpakBackend::checkAvailability()
{
    QString path = QStandardPaths::findExecutable(QStringLiteral("flatpak"));
    m_available = !path.isEmpty();
    if (!m_available)
        qWarning() << "flatpak not found, Flatpak features will be disabled";
}

bool FlatpakBackend::isAvailable() const
{
    return m_available;
}

QString FlatpakBackend::executeSync(const QStringList &args, bool &ok)
{
    if (!m_available) {
        ok = false;
        return QStringLiteral("flatpak not found");
    }

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    proc.setProcessEnvironment(env);

    proc.start(QStringLiteral("flatpak"), args);
    if (!proc.waitForStarted(5000)) {
        ok = false;
        return QStringLiteral("Failed to start flatpak");
    }

    // 30 min timeout for install/update operations, 60s for queries
    int timeout = 1800000;
    for (const auto &arg : args) {
        if (arg == QStringLiteral("list") || arg == QStringLiteral("search") ||
            arg == QStringLiteral("remotes")) {
            timeout = 60000;
            break;
        }
    }

    if (!proc.waitForFinished(timeout)) {
        proc.kill();
        proc.waitForFinished(3000);
        ok = false;
        return QStringLiteral("flatpak timed out");
    }

    ok = (proc.exitCode() == 0);
    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    if (!ok) {
        QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (!err.isEmpty())
            return err;
    }
    return output;
}

// ---------------------------------------------------------------------------
// Output parsing helpers
// ---------------------------------------------------------------------------

/*!
 * Parse the tab-separated output of:
 *   flatpak list --app --columns=name,application,version,branch,arch,origin,installation,size,description
 *
 * Returns a list of FlatpakApp objects with \c installed set to true.
 */
QList<FlatpakApp> FlatpakBackend::parseListOutput(const QByteArray &output)
{
    QList<FlatpakApp> apps;

    QString text = QString::fromUtf8(output);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        QString line = rawLine;
        // Remove trailing carriage return (in case of CRLF)
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        // Skip header lines that contain column names
        if (line.contains(QStringLiteral("application"), Qt::CaseInsensitive) &&
            line.contains(QStringLiteral("version"), Qt::CaseInsensitive) &&
            line.contains(QStringLiteral("branch"), Qt::CaseInsensitive))
            continue;

        QStringList fields = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        if (fields.size() < 8)
            continue;

        // If the description column contained tab characters, merge the
        // trailing fields back into the last column.
        if (fields.size() > 9) {
            fields[8] = fields.mid(8).join(QStringLiteral("\t"));
            fields = fields.mid(0, 9);
        } else if (fields.size() == 8) {
            // description column missing — pad with an empty value
            fields.append(QString());
        }

        QJsonObject obj;
        obj[QStringLiteral("name")] = fields.value(0);
        obj[QStringLiteral("application")] = fields.value(1);
        obj[QStringLiteral("version")] = fields.value(2);
        obj[QStringLiteral("branch")] = fields.value(3);
        obj[QStringLiteral("arch")] = fields.value(4);
        obj[QStringLiteral("origin")] = fields.value(5);
        obj[QStringLiteral("installation")] = fields.value(6);
        obj[QStringLiteral("size")] = fields.value(7);
        obj[QStringLiteral("description")] = fields.value(8);

        apps.append(FlatpakApp::fromListJson(obj));
    }

    return apps;
}

/*!
 * Parse the tab-separated output of:
 *   flatpak search --columns=name,application,version,branch,remote,description <keyword>
 *
 * Returns a list of FlatpakApp objects with \c installed set to false.
 */
QList<FlatpakApp> FlatpakBackend::parseSearchOutput(const QByteArray &output)
{
    QList<FlatpakApp> apps;

    QString text = QString::fromUtf8(output);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        // Skip header lines
        if (line.contains(QStringLiteral("application"), Qt::CaseInsensitive) &&
            line.contains(QStringLiteral("remote"), Qt::CaseInsensitive))
            continue;

        QStringList fields = line.split(QLatin1Char('\t'), Qt::KeepEmptyParts);
        if (fields.size() < 5)
            continue;

        // Merge trailing fields into description
        if (fields.size() > 6) {
            fields[5] = fields.mid(5).join(QStringLiteral("\t"));
            fields = fields.mid(0, 6);
        } else if (fields.size() == 5) {
            fields.append(QString());
        }

        QJsonObject obj;
        obj[QStringLiteral("name")] = fields.value(0);
        obj[QStringLiteral("application")] = fields.value(1);
        obj[QStringLiteral("version")] = fields.value(2);
        obj[QStringLiteral("branch")] = fields.value(3);
        obj[QStringLiteral("remote")] = fields.value(4);
        obj[QStringLiteral("description")] = fields.value(5);

        apps.append(FlatpakApp::fromSearchJson(obj));
    }

    return apps;
}

/*!
 * Parse the output of \c flatpak list --app --updates --columns=application
 * and return the set of application IDs that have updates available.
 */
QSet<QString> FlatpakBackend::parseUpgradableOutput(const QByteArray &output)
{
    QSet<QString> ids;

    QString text = QString::fromUtf8(output);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        QString line = rawLine.trimmed();
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.isEmpty())
            continue;
        // Skip headers
        if (line.contains(QStringLiteral("application"), Qt::CaseInsensitive))
            continue;
        ids.insert(line);
    }

    return ids;
}

/*!
 * Parse the output of \c flatpak remotes and return a list of remote names.
 */
QStringList FlatpakBackend::parseRemotesOutput(const QByteArray &output)
{
    QStringList remotes;

    QString text = QString::fromUtf8(output);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;

        // flatpak remotes output may be "name\ttitle" or just "name"
        QStringList fields = line.split(QLatin1Char('\t'));
        QString name = fields.value(0).trimmed();
        if (!name.isEmpty())
            remotes.append(name);
    }

    return remotes;
}

// ---------------------------------------------------------------------------
// Async query operations
// ---------------------------------------------------------------------------

void FlatpakBackend::loadInstalledApps()
{
    auto *watcher = new QFutureWatcher<QList<FlatpakApp>>(this);
    connect(watcher, &QFutureWatcher<QList<FlatpakApp>>::finished, this, [this, watcher]() {
        Q_EMIT installedAppsLoaded(watcher->result());
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        // Load installed apps
        bool ok;
        QString output = executeSync(
            {QStringLiteral("list"), QStringLiteral("--app"),
             QStringLiteral("--columns=") + QString::fromLatin1(kListColumns)},
            ok);
        if (!ok) {
            Q_EMIT errorOccurred(output);
            return QList<FlatpakApp>();
        }

        QList<FlatpakApp> apps = parseListOutput(output.toUtf8());

        // Query upgradable apps and merge status
        QString upgOutput = executeSync(
            {QStringLiteral("list"), QStringLiteral("--app"), QStringLiteral("--updates"),
             QStringLiteral("--columns=application")},
            ok);
        if (ok) {
            QSet<QString> upgradableIds = parseUpgradableOutput(upgOutput.toUtf8());
            for (auto &app : apps) {
                if (upgradableIds.contains(app.appId))
                    app.upgradable = true;
            }
        }

        return apps;
    }));
}

void FlatpakBackend::loadRemotes()
{
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher]() {
        Q_EMIT remotesLoaded(watcher->result());
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        bool ok;
        QString output = executeSync({QStringLiteral("remotes")}, ok);
        if (!ok) {
            Q_EMIT errorOccurred(output);
            return QStringList();
        }
        return parseRemotesOutput(output.toUtf8());
    }));
}

void FlatpakBackend::searchApps(const QString &keyword)
{
    auto *watcher = new QFutureWatcher<QList<FlatpakApp>>(this);
    connect(watcher, &QFutureWatcher<QList<FlatpakApp>>::finished, this, [this, watcher]() {
        Q_EMIT searchCompleted(watcher->result());
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this, keyword]() {
        bool ok;
        QString output = executeSync(
            {QStringLiteral("search"),
             QStringLiteral("--columns=") + QString::fromLatin1(kSearchColumns),
             keyword},
            ok);
        if (!ok) {
            Q_EMIT errorOccurred(output);
            return QList<FlatpakApp>();
        }
        return parseSearchOutput(output.toUtf8());
    }));
}

// ---------------------------------------------------------------------------
// Async package operations (install / uninstall / update / run)
// ---------------------------------------------------------------------------

void FlatpakBackend::executeAsync(const QString &appId, const QString &operation, const QStringList &args)
{
    Q_EMIT operationStarted(appId, operation);

    auto *proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    proc->setProcessEnvironment(env);

    // Use a shared pointer so the lambda captures remain valid for the
    // lifetime of the QProcess.  A raw local QString would dangle once
    // executeAsync() returns, causing use-after-free in the async handlers.
    auto lastMessage = std::make_shared<QString>();

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, appId, proc, lastMessage]() {
        QByteArray data = proc->readAllStandardOutput();
        // flatpak outputs progress as plain text lines; emit each non-empty
        // line as a progress message with an indeterminate percentage (-1).
        for (const QByteArray &line : data.split('\n')) {
            QString text = QString::fromUtf8(line).trimmed();
            if (text.isEmpty())
                continue;

            // Try to extract a percentage from lines like "Downloading: 45%"
            int percent = -1;
            QRegularExpression rx(QStringLiteral("(\\d+)%"));
            QRegularExpressionMatch match = rx.match(text);
            if (match.hasMatch())
                percent = match.captured(1).toInt();

            Q_EMIT operationProgress(appId, text, percent);
            *lastMessage = text;
        }
    });

    connect(proc, &QProcess::readyReadStandardError, this, [this, appId, proc, lastMessage]() {
        QByteArray data = proc->readAllStandardError();
        for (const QByteArray &line : data.split('\n')) {
            QString text = QString::fromUtf8(line).trimmed();
            if (text.isEmpty())
                continue;
            *lastMessage = text;
        }
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, appId, operation, proc, lastMessage](int exitCode, QProcess::ExitStatus) {
        bool success = (exitCode == 0);
        QString msg = *lastMessage;
        if (!success && msg.isEmpty())
            msg = QStringLiteral("Operation failed with exit code %1").arg(exitCode);
        Q_EMIT operationFinished(appId, operation, success, msg);
        proc->deleteLater();
    });

    connect(proc, &QProcess::errorOccurred, this, [this, appId, operation, proc](QProcess::ProcessError) {
        Q_EMIT operationFinished(appId, operation, false, proc->errorString());
        proc->deleteLater();
    });

    proc->start(QStringLiteral("flatpak"), args);
    if (!proc->waitForStarted(5000)) {
        Q_EMIT operationFinished(appId, operation, false, QStringLiteral("Failed to start flatpak"));
        proc->deleteLater();
    }
}

void FlatpakBackend::installApp(const QString &remote, const QString &appId)
{
    QStringList args{QStringLiteral("install"), QStringLiteral("--assumeyes")};
    if (!remote.isEmpty())
        args.append(remote);
    args.append(appId);
    executeAsync(appId, QStringLiteral("install"), args);
}

void FlatpakBackend::uninstallApp(const QString &appId)
{
    executeAsync(appId, QStringLiteral("uninstall"),
                 {QStringLiteral("uninstall"), QStringLiteral("--assumeyes"), appId});
}

void FlatpakBackend::updateApp(const QString &appId)
{
    executeAsync(appId, QStringLiteral("update"),
                 {QStringLiteral("update"), QStringLiteral("--assumeyes"), appId});
}

void FlatpakBackend::updateAll()
{
    executeAsync(QString(), QStringLiteral("update-all"),
                 {QStringLiteral("update"), QStringLiteral("--assumeyes")});
}

void FlatpakBackend::runApp(const QString &appId)
{
    auto *proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    proc->setProcessEnvironment(env);

    proc->start(QStringLiteral("flatpak"), {QStringLiteral("run"), appId});
    if (!proc->waitForStarted(5000)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to start application: %1").arg(appId));
        proc->deleteLater();
    } else {
        // Don't wait for run to finish — it's a long-lived process
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                proc, &QProcess::deleteLater);
    }
}

}
