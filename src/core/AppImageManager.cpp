#include "AppImageManager.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>

namespace Miryu {

namespace {

// Replace a leading ~ (or ~/) or $HOME (or $HOME/) with the real home directory.
QString expandTilde(QString path)
{
    if ((path.size() == 1 && path.at(0) == QLatin1Char('~')) ||
        (path.size() >= 2 && path.startsWith(QLatin1String("~/")))) {
        path.remove(0, 1);
        path.prepend(QDir::homePath());
    } else if (path == QLatin1String("$HOME")) {
        path = QDir::homePath();
    } else if (path.startsWith(QLatin1String("$HOME/"))) {
        path.replace(0, 5, QDir::homePath());
    }
    return path;
}

// Return the program of an Exec= line, honouring double quotes.
QString firstExecToken(const QString &execLine)
{
    const QString exec = execLine.trimmed();
    if (exec.isEmpty())
        return {};
    if (exec.startsWith(QLatin1Char('"'))) {
        const int end = exec.indexOf(QLatin1Char('"'), 1);
        if (end > 0)
            return exec.mid(1, end - 1);
    }
    const int space = exec.indexOf(QLatin1Char(' '));
    return space > 0 ? exec.left(space) : exec;
}

// Parse the values of the [Desktop Entry] group we need.
bool readDesktopEntry(const QString &desktopPath, IntegratedAppImage *app)
{
    QFile file(desktopPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    bool inEntry = false;
    QString execLine;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('['))) {
            inEntry = (line == QLatin1String("[Desktop Entry]"));
            continue;
        }
        if (!inEntry)
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        if (key == QLatin1String("Name"))
            app->name = value;
        else if (key == QLatin1String("Icon"))
            app->iconName = value;
        else if (key == QLatin1String("TryExec"))
            app->appImagePath = value;
        else if (key == QLatin1String("Exec"))
            execLine = value;
    }

    if (app->appImagePath.isEmpty())
        app->appImagePath = firstExecToken(execLine);
    app->appImagePath = expandTilde(app->appImagePath);
    app->desktopFile = desktopPath;
    return !app->appImagePath.isEmpty();
}

// Locate an icon file installed in the hicolor theme (the launcher extracts
// icons there). Returns an empty string when the icon is theme-only or
// missing.
QString findIconFile(const QString &iconName)
{
    if (iconName.isEmpty())
        return {};

    const QFileInfo direct(iconName);
    if (direct.isAbsolute()) {
        if (direct.exists())
            return direct.absoluteFilePath();
        return {};
    }

    static const QStringList extensions = {
        QStringLiteral(".png"), QStringLiteral(".svg"),
        QStringLiteral(".svgz"), QStringLiteral(".xpm")
    };
    static const QStringList sizes = {
        QStringLiteral("scalable"), QStringLiteral("512x512"),
        QStringLiteral("256x256"), QStringLiteral("128x128"),
        QStringLiteral("96x96"), QStringLiteral("64x64"),
        QStringLiteral("48x48"), QStringLiteral("32x32")
    };

    QStringList roots;
    roots << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                 + QStringLiteral("/icons");
    const QStringList dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &dir : dataDirs)
        roots << dir + QStringLiteral("/icons");
    roots.removeDuplicates();

    for (const QString &root : roots) {
        for (const QString &size : sizes) {
            const QString appsDir = root + QStringLiteral("/hicolor/") + size + QStringLiteral("/apps");
            for (const QString &ext : extensions) {
                const QString candidate = appsDir + QLatin1Char('/') + iconName + ext;
                if (QFileInfo::exists(candidate))
                    return candidate;
            }
        }
    }
    return {};
}

} // namespace

AppImageManager::AppImageManager(QObject *parent)
    : QObject(parent)
{
}

QString AppImageManager::integrationDirectory()
{
    const KSharedConfigPtr config = KSharedConfig::openConfig(QStringLiteral("miryurc"));
    const QString configured =
        KConfigGroup(config, QStringLiteral("Miryu"))
            .readEntry(QStringLiteral("Destination"), QStringLiteral("~/Applications"));
    return expandTilde(configured);
}

QList<IntegratedAppImage> AppImageManager::scan() const
{
    const QString destination = QFileInfo(integrationDirectory()).absoluteFilePath();

    QStringList applicationDirs;
    applicationDirs << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                           + QStringLiteral("/applications");
    const QStringList dataDirs = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &dir : dataDirs)
        applicationDirs << dir + QStringLiteral("/applications");
    applicationDirs.removeDuplicates();

    QList<IntegratedAppImage> apps;
    QSet<QString> seen;

    for (const QString &dir : applicationDirs) {
        QDir appDir(dir);
        if (!appDir.exists())
            continue;

        const QStringList files =
            appDir.entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &file : files) {
            IntegratedAppImage app;
            if (!readDesktopEntry(appDir.filePath(file), &app))
                continue;

            const QFileInfo appImage(app.appImagePath);
            if (!appImage.exists() || !appImage.isFile())
                continue;

            // Only Miryu-integrated AppImages: entries registered by
            // libappimage (appimagekit_*.desktop) or pointing into the
            // launcher's integration directory.
            const QString absolutePath = appImage.absoluteFilePath();
            const bool registered = file.startsWith(QLatin1String("appimagekit_"));
            const bool insideDestination = absolutePath.startsWith(destination + QLatin1Char('/'));
            if (!registered && !insideDestination)
                continue;

            if (seen.contains(absolutePath))
                continue;
            seen.insert(absolutePath);

            if (app.name.isEmpty())
                app.name = appImage.completeBaseName();
            app.appImagePath = absolutePath;
            app.iconPath = findIconFile(app.iconName);
            apps.append(app);
        }
    }

    std::sort(apps.begin(), apps.end(), [](const IntegratedAppImage &a, const IntegratedAppImage &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return apps;
}

bool AppImageManager::run(const IntegratedAppImage &app, QString *error) const
{
    const QFileInfo info(app.appImagePath);
    if (!info.exists() || !info.isFile()) {
        if (error)
            *error = i18n("The AppImage file no longer exists:\n%1").arg(app.appImagePath);
        return false;
    }

    const QString path = info.absoluteFilePath();
    QFile::Permissions permissions = QFile::permissions(path);
    if (!(permissions & QFileDevice::ExeOwner)) {
        permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
        QFile::setPermissions(path, permissions);
    }

    if (!QProcess::startDetached(path, {}, QDir::homePath())) {
        if (error)
            *error = i18n("Failed to launch \"%1\".").arg(app.name);
        return false;
    }
    return true;
}

void AppImageManager::uninstall(const IntegratedAppImage &app)
{
    if (m_uninstallProcess)
        return;

    const QString cli = QStandardPaths::findExecutable(QStringLiteral("miryu-app-launcher-cli"));
    if (cli.isEmpty()) {
        Q_EMIT uninstallFinished(false,
                                 i18n("miryu-app-launcher-cli was not found, so the AppImage "
                                      "integration could not be removed."));
        return;
    }

    m_uninstallProcess = new QProcess(this);
    const QString displayName = app.name;
    connect(m_uninstallProcess, &QProcess::errorOccurred, this,
            [this, displayName](QProcess::ProcessError) {
        if (!m_uninstallProcess)
            return;
        const QString message = m_uninstallProcess->errorString();
        m_uninstallProcess->deleteLater();
        m_uninstallProcess = nullptr;
        Q_EMIT uninstallFinished(false,
                                 i18n("Failed to remove \"%1\": %2", displayName, message));
    });
    connect(m_uninstallProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, displayName](int exitCode, QProcess::ExitStatus status) {
        if (!m_uninstallProcess)
            return;
        const QString stderrText =
            QString::fromUtf8(m_uninstallProcess->readAllStandardError()).trimmed();
        m_uninstallProcess->deleteLater();
        m_uninstallProcess = nullptr;

        const bool success = (status == QProcess::NormalExit && exitCode == 0);
        if (success) {
            Q_EMIT uninstallFinished(true, i18n("Removed \"%1\".", displayName));
        } else {
            Q_EMIT uninstallFinished(false,
                                     stderrText.isEmpty()
                                         ? i18n("Failed to remove \"%1\".").arg(displayName)
                                         : stderrText);
        }
    });

    m_uninstallProcess->setProgram(cli);
    m_uninstallProcess->setArguments({QStringLiteral("unintegrate"), app.appImagePath});
    m_uninstallProcess->start();
}

} // namespace Miryu
