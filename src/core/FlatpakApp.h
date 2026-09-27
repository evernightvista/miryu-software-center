#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QMetaType>
#include <QJsonObject>

namespace Miryu {

class FlatpakApp
{
    Q_GADGET
    Q_PROPERTY(QString appId MEMBER appId)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString version MEMBER version)
    Q_PROPERTY(QString branch MEMBER branch)
    Q_PROPERTY(QString arch MEMBER arch)
    Q_PROPERTY(QString remote MEMBER remote)
    Q_PROPERTY(QString installType MEMBER installType)
    Q_PROPERTY(qint64 size MEMBER size)
    Q_PROPERTY(QString description MEMBER description)
    Q_PROPERTY(bool installed MEMBER installed)
    Q_PROPERTY(bool upgradable MEMBER upgradable)

public:
    FlatpakApp() = default;

    QString appId;
    QString name;
    QString version;
    QString branch;
    QString arch;
    QString remote;
    QString installType; // "user" or "system"
    qint64 size = 0;
    QString description;

    // Runtime state
    bool installed = false;
    bool upgradable = false;

    QString ref() const
    {
        // flatpak ref format: appId/version/arch
        return appId + QStringLiteral("/") + branch + QStringLiteral("/") + arch;
    }

    bool operator==(const FlatpakApp &other) const
    {
        return appId == other.appId && branch == other.branch && arch == other.arch;
    }

    /*!
     * Construct a FlatpakApp from a JSON object produced by parsing
     * \c flatpak list --columns=name,application,version,branch,arch,origin,installation,size,description
     *
     * Expected keys: name, application, version, branch, arch, origin,
     * installation, size, description.
     */
    static FlatpakApp fromListJson(const QJsonObject &obj)
    {
        FlatpakApp app;
        app.name = obj.value(QStringLiteral("name")).toString();
        app.appId = obj.value(QStringLiteral("application")).toString();
        app.version = obj.value(QStringLiteral("version")).toString();
        app.branch = obj.value(QStringLiteral("branch")).toString();
        if (app.branch.isEmpty())
            app.branch = QStringLiteral("stable");
        app.arch = obj.value(QStringLiteral("arch")).toString();
        app.remote = obj.value(QStringLiteral("origin")).toString();
        app.installType = obj.value(QStringLiteral("installation")).toString();
        if (app.installType.isEmpty())
            app.installType = QStringLiteral("system");
        app.size = parseSize(obj.value(QStringLiteral("size")).toString());
        app.description = obj.value(QStringLiteral("description")).toString();
        app.installed = true;
        return app;
    }

    /*!
     * Construct a FlatpakApp from a JSON object produced by parsing
     * \c flatpak search --columns=name,application,version,branch,remote,description
     *
     * Expected keys: name, application, version, branch, remote, description.
     */
    static FlatpakApp fromSearchJson(const QJsonObject &obj)
    {
        FlatpakApp app;
        app.name = obj.value(QStringLiteral("name")).toString();
        app.appId = obj.value(QStringLiteral("application")).toString();
        app.version = obj.value(QStringLiteral("version")).toString();
        app.branch = obj.value(QStringLiteral("branch")).toString();
        if (app.branch.isEmpty())
            app.branch = QStringLiteral("stable");
        app.remote = obj.value(QStringLiteral("remote")).toString();
        app.description = obj.value(QStringLiteral("description")).toString();
        app.installed = false;

        // Derive a readable name from the application ID if none was provided
        if (app.name.isEmpty() && !app.appId.isEmpty()) {
            QStringList parts = app.appId.split(QStringLiteral("."));
            if (!parts.isEmpty())
                app.name = parts.last();
        }

        return app;
    }

    /*!
     * Parse a size string that may be a raw byte count or a human-readable
     * string such as "1.2 GB" / "345.6 MB".
     */
    static qint64 parseSize(const QString &str)
    {
        QString s = str.trimmed();
        if (s.isEmpty())
            return 0;

        // Try pure integer (bytes)
        bool ok = false;
        qint64 bytes = s.toLongLong(&ok);
        if (ok)
            return bytes;

        // Parse "<number> <unit>" form, e.g. "1.2 GB"
        QStringList parts = s.split(QStringLiteral(" "));
        if (parts.size() != 2)
            return 0;

        double value = parts.at(0).toDouble(&ok);
        if (!ok)
            return 0;

        QString unit = parts.at(1).toUpper();
        if (unit == QStringLiteral("B"))
            return static_cast<qint64>(value);
        if (unit == QStringLiteral("KB") || unit == QStringLiteral("K") ||
            unit == QStringLiteral("KIB"))
            return static_cast<qint64>(value * 1024);
        if (unit == QStringLiteral("MB") || unit == QStringLiteral("M") ||
            unit == QStringLiteral("MIB"))
            return static_cast<qint64>(value * 1024 * 1024);
        if (unit == QStringLiteral("GB") || unit == QStringLiteral("G") ||
            unit == QStringLiteral("GIB"))
            return static_cast<qint64>(value * 1024 * 1024 * 1024);
        if (unit == QStringLiteral("TB") || unit == QStringLiteral("T") ||
            unit == QStringLiteral("TIB"))
            return static_cast<qint64>(value * 1024 * 1024 * 1024 * 1024);

        return 0;
    }

    /*!
     * Format a byte count into a human-readable string.
     */
    static QString formatSize(qint64 bytes)
    {
        if (bytes <= 0)
            return QStringLiteral("Unknown");
        if (bytes < 1024)
            return QString::number(bytes) + QStringLiteral(" B");
        if (bytes < 1024 * 1024)
            return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
        if (bytes < 1024 * 1024 * 1024)
            return QString::number(bytes / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
    }
};

inline uint qHash(const FlatpakApp &app, uint seed = 0)
{
    return qHash(app.appId + app.branch + app.arch, seed);
}

}

Q_DECLARE_METATYPE(Miryu::FlatpakApp)
Q_DECLARE_METATYPE(QList<Miryu::FlatpakApp>)
