#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMetaType>
#include <QVariantMap>
#include <QVariantList>
#include "Enums.h"

namespace Miryu {

class Package
{
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString arch MEMBER arch)
    Q_PROPERTY(QString epoch MEMBER epoch)
    Q_PROPERTY(QString version MEMBER version)
    Q_PROPERTY(QString release MEMBER release)
    Q_PROPERTY(QString repo MEMBER repo)
    Q_PROPERTY(QString summary MEMBER summary)
    Q_PROPERTY(QString description MEMBER description)
    Q_PROPERTY(QString url MEMBER url)
    Q_PROPERTY(QString license MEMBER license)
    Q_PROPERTY(qint64 size MEMBER size)
    Q_PROPERTY(PackageState state MEMBER state)
    Q_PROPERTY(PackageTodo todo MEMBER todo)
    Q_PROPERTY(QStringList requiresList MEMBER requiresList)
    Q_PROPERTY(QStringList provides MEMBER provides)
    Q_PROPERTY(QStringList files MEMBER files)
    Q_PROPERTY(QVariantList changelog MEMBER changelog)

public:
    Package() = default;

    QString name;
    QString arch;
    QString epoch;
    QString version;
    QString release;
    QString repo;
    QString summary;
    QString description;
    QString url;
    QString license;
    qint64 size = 0;

    PackageState state = PackageState::Available;
    PackageTodo todo = PackageTodo::None;

    // Extended package details (populated asynchronously via getPackageDetails)
    QStringList requiresList;    // Dependencies (requires) - named to avoid C++20 keyword
    QStringList provides;     // What this package provides
    QStringList files;        // List of files in the package
    QVariantList changelog;   // Changelog entries

    bool queued = false;
    bool isDep = false;

    QString evr() const
    {
        if (epoch.isEmpty() || epoch == QStringLiteral("0"))
            return version + QStringLiteral("-") + release;
        return epoch + QStringLiteral(":") + version + QStringLiteral("-") + release;
    }

    QString nevra() const
    {
        return name + QStringLiteral("-") + evr() + QStringLiteral(".") + arch;
    }

    QString na() const
    {
        return name + QStringLiteral(".") + arch;
    }

    QString id() const
    {
        // dnf5daemon parses the id as a comma-separated nevra-like string.
        // Empty fields produce double commas which the daemon cannot parse.
        QString ep = epoch.isEmpty() ? QStringLiteral("0") : epoch;
        QString ver = version.isEmpty() ? QStringLiteral("0") : version;
        QString rel = release.isEmpty() ? QStringLiteral("0") : release;
        return QStringList{name, ep, ver, rel, arch, repo}.join(QStringLiteral(","));
    }

    bool isInstalled() const
    {
        return state == PackageState::Installed;
    }

    bool operator==(const Package &other) const
    {
        return nevra() == other.nevra();
    }

    bool operator!=(const Package &other) const
    {
        return !(*this == other);
    }

    void calcTodo()
    {
        todo = Miryu::calcTodo(state);
    }

    static Package fromVariantMap(const QVariantMap &map, PackageState state = PackageState::Available)
    {
        Package pkg;
        pkg.name = map.value(QStringLiteral("name")).toString();
        pkg.arch = map.value(QStringLiteral("arch")).toString();
        pkg.epoch = map.value(QStringLiteral("epoch")).toString();
        pkg.version = map.value(QStringLiteral("version")).toString();
        pkg.release = map.value(QStringLiteral("release")).toString();
        if (pkg.version.isEmpty() || pkg.release.isEmpty()) {
            // Parse evr (epoch:version-release) when separate fields are absent.
            QString evrStr = map.value(QStringLiteral("evr")).toString();
            QString vrl = evrStr.section(QStringLiteral(":"), -1); // strip epoch
            if (pkg.version.isEmpty())
                pkg.version = vrl.section(QStringLiteral("-"), 0, -2);
            if (pkg.release.isEmpty())
                pkg.release = vrl.section(QStringLiteral("-"), -1);
        }
        pkg.repo = map.value(QStringLiteral("repo_id")).toString();
        if (pkg.repo.isEmpty())
            pkg.repo = map.value(QStringLiteral("repo")).toString();
        pkg.summary = map.value(QStringLiteral("summary")).toString();
        pkg.description = map.value(QStringLiteral("description")).toString();
        pkg.url = map.value(QStringLiteral("url")).toString();
        pkg.license = map.value(QStringLiteral("license")).toString();
        pkg.size = map.value(QStringLiteral("install_size")).toLongLong();
        if (pkg.size == 0)
            pkg.size = map.value(QStringLiteral("download_size")).toLongLong();

        if (state == PackageState::Available) {
            bool installed = map.value(QStringLiteral("is_installed")).toBool();
            if (installed)
                pkg.state = PackageState::Installed;
            else
                pkg.state = PackageState::Available;
        } else {
            pkg.state = state;
        }

        pkg.calcTodo();
        return pkg;
    }

    QVariantMap toVariantMap() const
    {
        QVariantMap map;
        map[QStringLiteral("name")] = name;
        map[QStringLiteral("arch")] = arch;
        map[QStringLiteral("epoch")] = epoch;
        map[QStringLiteral("version")] = version;
        map[QStringLiteral("release")] = release;
        map[QStringLiteral("repo_id")] = repo;
        map[QStringLiteral("summary")] = summary;
        map[QStringLiteral("description")] = description;
        map[QStringLiteral("url")] = url;
        map[QStringLiteral("license")] = license;
        map[QStringLiteral("install_size")] = size;
        return map;
    }
};

inline uint qHash(const Package &pkg, uint seed = 0)
{
    return qHash(pkg.nevra(), seed);
}

}

Q_DECLARE_METATYPE(Miryu::Package)
