#include "PackageCache.h"

namespace Miryu {

PackageCache::PackageCache(QObject *parent)
    : QObject(parent)
{
}

QList<Package> PackageCache::getPackages(PackageFilter filter, bool reset)
{
    if (reset || !m_cache.contains(filter))
        return {};

    return m_cache.value(filter);
}

void PackageCache::setPackages(PackageFilter filter, const QList<Package> &packages)
{
    m_cache[filter] = packages;
    m_byNevra.clear();
    for (const auto &pkg : packages)
        m_byNevra[pkg.nevra()] = pkg;
}

Package PackageCache::getPackage(const QString &nevra) const
{
    return m_byNevra.value(nevra);
}

void PackageCache::updatePackage(const Package &pkg)
{
    m_byNevra[pkg.nevra()] = pkg;
}

void PackageCache::clear()
{
    m_cache.clear();
    m_byNevra.clear();
    m_detailsCache.clear();
}

}
