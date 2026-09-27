#pragma once

#include <QObject>
#include <QHash>
#include <QVariantMap>
#include "Dnf5DaemonClient.h"
#include "Package.h"
#include "Enums.h"

namespace Miryu {

class PackageCache : public QObject
{
    Q_OBJECT

public:
    explicit PackageCache(QObject *parent = nullptr);

    QList<Package> getPackages(PackageFilter filter, bool reset = false);
    void setPackages(PackageFilter filter, const QList<Package> &packages);
    Package getPackage(const QString &nevra) const;
    void updatePackage(const Package &pkg);
    void clear();

    bool hasFilter(PackageFilter filter) const { return m_cache.contains(filter); }

    // Package details cache (requires, provides, files, changelog, description)
    // Keyed by package name. Cached after first query so switching between
    // packages does not re-run dnf5 repoquery.
    QVariantMap getDetails(const QString &pkgName) const { return m_detailsCache.value(pkgName); }
    void setDetails(const QString &pkgName, const QVariantMap &details) { m_detailsCache[pkgName] = details; }
    bool hasDetails(const QString &pkgName) const { return m_detailsCache.contains(pkgName); }
    void clearDetails() { m_detailsCache.clear(); }

private:
    QHash<PackageFilter, QList<Package>> m_cache;
    QHash<QString, Package> m_byNevra;
    QHash<QString, QVariantMap> m_detailsCache;
};

}
