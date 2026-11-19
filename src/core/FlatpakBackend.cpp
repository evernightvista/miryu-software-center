#include "FlatpakBackend.h"

#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QDebug>
#include <QtConcurrent>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QTimer>
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
    , m_networkManager(new QNetworkAccessManager(this))
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
            arg == QStringLiteral("remotes") || arg == QStringLiteral("remote-ls")) {
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
 * Return the refs that have an update available.
 *
 * \c flatpak list --updates only exists in recent flatpak releases (older
 * ones abort with "Unknown option --updates"), so the refs are parsed from
 * the numbered summary printed by plain \c flatpak update — the exact same
 * output the update-check notifier counts. Without a controlling terminal
 * flatpak answers its own "Proceed?" prompt with "no", so probing never
 * deploys anything.
 *
 * Each summary row looks like
 * \verbatim
 *  1. org.kde.Platform.Locale   6.11   u   flathub   1.6 kB / 1.6 kB
 * \endverbatim
 * i.e. the branch is the first token after the ref ID. It is kept because the
 * same application ID can be installed on several branches.
 */
QList<FlatpakBackend::UpdatableRef> FlatpakBackend::fetchUpdatableRefs()
{
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    proc.setProcessEnvironment(env);
    // The summary is on stdout while the abort notice goes to stderr; merge
    // them so nothing of the table is lost (the exit code is non-zero when
    // flatpak aborts the transaction, so stdout must not be discarded).
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QStringLiteral("flatpak"), {QStringLiteral("update")});
    if (!proc.waitForStarted(5000))
        return {};
    // If flatpak ever decides to wait for an answer instead of detecting the
    // missing TTY, EOF makes the prompt abort.
    proc.closeWriteChannel();
    if (!proc.waitForFinished(600000)) {
        proc.kill();
        proc.waitForFinished(3000);
    }
    const QString output = QString::fromUtf8(proc.readAllStandardOutput());

    static const QRegularExpression rowPattern(QStringLiteral("^\\s*\\d+\\.\\s"));
    // Flatpak ref IDs always have at least three components, so "a.b.c"
    // never matches version numbers or download sizes like "250.4 MB".
    static const QRegularExpression refPattern(
        QStringLiteral("([A-Za-z0-9_-]+(?:\\.[A-Za-z0-9_-]+){2,})"));

    QList<UpdatableRef> refs;
    QSet<QString> seenRefs;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (!rowPattern.match(line).hasMatch())
            continue;
        const auto match = refPattern.match(line);
        if (!match.hasMatch())
            continue;

        UpdatableRef ref;
        ref.appId = match.captured(1);
        // The first token after the ref ID is the branch (the summary prints
        // "ID  Branch  Op  Remote  Download").
        const QString rest = line.mid(match.capturedEnd()).simplified();
        if (!rest.isEmpty())
            ref.branch = rest.section(QLatin1Char(' '), 0, 0);
        if (ref.appId.isEmpty())
            continue;

        const QString key = ref.appId + QLatin1Char('\n') + ref.branch;
        if (seenRefs.contains(key))
            continue;
        seenRefs.insert(key);
        refs.append(ref);
    }

    return refs;
}

/*!
 * Parse the output of \c flatpak remotes --columns=name,title.
 * The map key is the remote name used by Flatpak commands; the value is
 * the human-readable title shown in the UI.
 */
QMap<QString, QString> FlatpakBackend::parseRemotesOutput(const QByteArray &output)
{
    QMap<QString, QString> remotes;

    QString text = QString::fromUtf8(output);
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);

    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().isEmpty())
            continue;

        const QStringList fields = line.split(QLatin1Char('\t'));
        const QString name = fields.value(0).trimmed();
        const QString title = fields.value(1).trimmed();
        if (name.isEmpty())
            continue;
        if (name.compare(QStringLiteral("Name"), Qt::CaseInsensitive) == 0 &&
            title.compare(QStringLiteral("Title"), Qt::CaseInsensitive) == 0)
            continue;

        remotes.insert(name, title.isEmpty() ? name : title);
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

        // Query upgradable apps and merge status. Older flatpak releases do
        // not support `list --updates`, so the summary of `flatpak update` is
        // parsed instead.
        QSet<QString> upgradableIds;
        for (const auto &ref : fetchUpdatableRefs())
            upgradableIds.insert(ref.appId);
        for (auto &app : apps) {
            if (upgradableIds.contains(app.appId))
                app.upgradable = true;
        }

        return apps;
    }));
}

void FlatpakBackend::loadUpdatableApps()
{
    auto *watcher = new QFutureWatcher<QList<FlatpakApp>>(this);
    connect(watcher, &QFutureWatcher<QList<FlatpakApp>>::finished, this, [this, watcher]() {
        Q_EMIT updatableAppsLoaded(watcher->result());
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        const QList<UpdatableRef> refs = fetchUpdatableRefs();
        if (refs.isEmpty())
            return QList<FlatpakApp>();

        // Enrich the refs from the installed listing. --all is required
        // because plain `flatpak list` hides runtime extensions such as
        // org.kde.Platform.Locale, which `flatpak update` does update.
        bool ok = false;
        const QString listOutput = executeSync(
            {QStringLiteral("list"), QStringLiteral("--all"),
             QStringLiteral("--columns=") + QString::fromLatin1(kListColumns)},
            ok);

        QHash<QString, FlatpakApp> installed;         // "appId\nbranch"
        QHash<QString, FlatpakApp> installedById;     // fallback: first match
        if (ok) {
            for (const auto &app : parseListOutput(listOutput.toUtf8())) {
                if (app.appId.isEmpty())
                    continue;
                installed.insert(app.appId + QLatin1Char('\n') + app.branch, app);
                if (!installedById.contains(app.appId))
                    installedById.insert(app.appId, app);
            }
        }

        // The update summary does not print versions; the version that the
        // remotes offer is taken from `flatpak remote-ls`, which every
        // flatpak release supports (unlike `list --updates`).
        QHash<QString, QString> availableVersions;    // "appId\nbranch" -> version
        const QString remoteOutput = executeSync(
            {QStringLiteral("remote-ls"), QStringLiteral("--all"),
             QStringLiteral("--columns=application,version,branch")},
            ok);
        if (ok) {
            const QStringList lines =
                remoteOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &rawLine : lines) {
                QString line = rawLine;
                if (line.endsWith(QLatin1Char('\r')))
                    line.chop(1);
                const QStringList fields = line.split(QLatin1Char('\t'));
                if (fields.size() < 3)
                    continue;
                const QString appId = fields.value(0).trimmed();
                const QString version = fields.value(1).trimmed();
                const QString branch = fields.value(2).trimmed();
                if (!appId.isEmpty() && !version.isEmpty())
                    availableVersions.insert(appId + QLatin1Char('\n') + branch,
                                             version);
            }
        }

        const QString remotesOutput = executeSync(
            {QStringLiteral("remotes"), QStringLiteral("--columns=name,title")}, ok);
        const QMap<QString, QString> remoteTitles = ok
            ? parseRemotesOutput(remotesOutput.toUtf8()) : QMap<QString, QString>();

        QList<FlatpakApp> apps;
        for (const auto &ref : refs) {
            const QString key = ref.appId + QLatin1Char('\n') + ref.branch;
            FlatpakApp app = installed.value(key);
            if (app.appId.isEmpty())
                app = installedById.value(ref.appId);
            app.appId = ref.appId;
            if (!ref.branch.isEmpty())
                app.branch = ref.branch;

            // Keep the installed version so the Updates page can render
            // "installed → available"; expose the version the remote offers
            // as the version of the update.
            app.installedVersion = app.version;
            const QString available = availableVersions.value(key);
            if (!available.isEmpty())
                app.version = available;

            app.remoteTitle = remoteTitles.value(app.remote);
            app.upgradable = true;
            apps.append(app);
        }
        return apps;
    }));
}

void FlatpakBackend::loadRemotes()
{
    auto *watcher = new QFutureWatcher<QMap<QString, QString>>(this);
    connect(watcher, &QFutureWatcher<QMap<QString, QString>>::finished, this, [this, watcher]() {
        Q_EMIT remotesLoaded(watcher->result());
        watcher->deleteLater();
    });

    watcher->setFuture(QtConcurrent::run([this]() {
        bool ok;
        QString output = executeSync(
            {QStringLiteral("remotes"), QStringLiteral("--columns=name,title")}, ok);
        if (!ok) {
            Q_EMIT errorOccurred(output);
            return QMap<QString, QString>();
        }
        return parseRemotesOutput(output.toUtf8());
    }));
}

void FlatpakBackend::loadPopularApps()
{
    QNetworkRequest request(
        QUrl(QStringLiteral("https://flathub.org/api/v2/collection/popular")));
    request.setRawHeader("User-Agent", "miryu-software-center");

    auto *reply = m_networkManager->get(request);
    QTimer::singleShot(30000, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            Q_EMIT popularAppsLoaded({});
            Q_EMIT errorOccurred(QStringLiteral("Failed to load popular Flatpak apps: %1").arg(reply->errorString()));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        const QJsonValue hits = document.object().value(QStringLiteral("hits"));
        if (parseError.error != QJsonParseError::NoError || !hits.isArray()) {
            Q_EMIT popularAppsLoaded({});
            Q_EMIT errorOccurred(QStringLiteral("Flathub returned an invalid popular apps response."));
            return;
        }

        QList<FlatpakApp> apps;
        for (const QJsonValue &value : hits.toArray()) {
            const QJsonObject hit = value.toObject();
            QJsonObject appObject;
            appObject[QStringLiteral("name")] = hit.value(QStringLiteral("name"));
            appObject[QStringLiteral("application")] = hit.value(QStringLiteral("app_id"));
            appObject[QStringLiteral("branch")] = QStringLiteral("stable");
            appObject[QStringLiteral("remote")] = QStringLiteral("flathub");
            appObject[QStringLiteral("description")] = hit.value(QStringLiteral("summary"));

            FlatpakApp app = FlatpakApp::fromSearchJson(appObject);
            if (!app.appId.isEmpty())
                apps.append(app);
            if (apps.size() >= 50)
                break;
        }
        Q_EMIT popularAppsLoaded(apps);
    });
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
    m_pendingOperations.enqueue({appId, operation, args});
    if (!m_operationRunning)
        startNextOperation();
}

void FlatpakBackend::startNextOperation()
{
    if (m_operationRunning || m_pendingOperations.isEmpty())
        return;

    m_operationRunning = true;
    const OperationRequest request = m_pendingOperations.dequeue();
    Q_EMIT operationStarted(request.appId, request.operation);

    auto *proc = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    proc->setProcessEnvironment(env);
    proc->setProcessChannelMode(QProcess::SeparateChannels);

    struct OperationState {
        QString lastMessage;
        bool completed = false;
    };
    auto state = std::make_shared<OperationState>();

    // All terminal paths pass through this idempotent cleanup. Using proc as
    // the connection context releases every lambda capture when it is deleted.
    auto complete = [this, proc, state, request](bool success, const QString &message) {
        if (state->completed)
            return;
        state->completed = true;

        if (proc->state() != QProcess::NotRunning)
            proc->kill();
        proc->deleteLater();

        Q_EMIT operationFinished(request.appId, request.operation, success, message);
        m_operationRunning = false;
        QTimer::singleShot(0, this, [this]() { startNextOperation(); });
    };

    auto handleOutput = [this, state, appId = request.appId](const QByteArray &data) {
        static const QRegularExpression lineSeparator(QStringLiteral("[\\r\\n]+"));
        static const QRegularExpression ansiEscape(QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]"));
        static const QRegularExpression percentRegex(QStringLiteral("(\\d+)%"));
        // When flatpak does not emit percentages (typically without a TTY),
        // its numbered "Installing 2/4" steps still provide real progress.
        static const QRegularExpression stepRegex(
            QStringLiteral("(?:Installing|Updating|Downloading)\\s*:?\\s*(\\d+)\\s*/\\s*(\\d+)"));

        const QString textData = QString::fromUtf8(data);
        const QStringList lines = textData.split(lineSeparator, Qt::SkipEmptyParts);
        for (QString text : lines) {
            text.remove(ansiEscape);
            text = text.trimmed();
            if (text.isEmpty())
                continue;
            if (text.size() > 4096)
                text.truncate(4096);

            int percent = -1;
            QRegularExpressionMatch match;
            auto percentMatches = percentRegex.globalMatch(text);
            while (percentMatches.hasNext())
                match = percentMatches.next();
            if (match.hasMatch()) {
                percent = match.captured(1).toInt();
            } else {
                match = stepRegex.match(text);
                if (match.hasMatch()) {
                    const int current = match.captured(1).toInt();
                    const int total = match.captured(2).toInt();
                    if (total > 0)
                        percent = current * 100 / total;
                }
            }
            if (percent >= 0)
                percent = qMin(percent, 100);

            state->lastMessage = text;
            Q_EMIT operationProgress(appId, text, percent);
        }
    };

    connect(proc, &QProcess::readyReadStandardOutput, proc,
            [proc, handleOutput]() { handleOutput(proc->readAllStandardOutput()); });
    connect(proc, &QProcess::readyReadStandardError, proc,
            [proc, handleOutput]() { handleOutput(proc->readAllStandardError()); });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc,
            [state, complete](int exitCode, QProcess::ExitStatus exitStatus) {
        const bool success = exitStatus == QProcess::NormalExit && exitCode == 0;
        QString message = state->lastMessage;
        if (!success && message.isEmpty())
            message = QStringLiteral("Operation failed with exit code %1").arg(exitCode);
        complete(success, message);
    });

    connect(proc, &QProcess::errorOccurred, proc,
            [proc, complete](QProcess::ProcessError) {
        complete(false, proc->errorString());
    });

    proc->start(QStringLiteral("flatpak"), request.args);
    if (!proc->waitForStarted(5000))
        complete(false, QStringLiteral("Failed to start flatpak"));
}

void FlatpakBackend::installApp(const QString &remote, const QString &appId)
{
    QStringList args{QStringLiteral("install"), QStringLiteral("--assumeyes")};
    if (!remote.isEmpty())
        args.append(remote);
    args.append(appId);
    executeAsync(appId, QStringLiteral("install"), args);
}

void FlatpakBackend::uninstallApp(const QString &appId, bool deleteData)
{
    QStringList args{QStringLiteral("uninstall"), QStringLiteral("--assumeyes")};
    if (deleteData)
        args.append(QStringLiteral("--delete-data"));
    args.append(appId);
    executeAsync(appId, QStringLiteral("uninstall"), args);
}

void FlatpakBackend::updateApp(const QString &appId)
{
    executeAsync(appId, QStringLiteral("update"),
                 {QStringLiteral("update"), QStringLiteral("--assumeyes"), appId});
}

void FlatpakBackend::updateApps(const QStringList &appIds)
{
    QStringList args{QStringLiteral("update"), QStringLiteral("--assumeyes")};
    QStringList ids;
    for (const QString &id : appIds) {
        if (!id.isEmpty()) {
            ids.append(id);
            args.append(id);
        }
    }
    if (ids.isEmpty())
        return;

    executeAsync(ids.join(QLatin1Char(',')), QStringLiteral("update-batch"), args);
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
