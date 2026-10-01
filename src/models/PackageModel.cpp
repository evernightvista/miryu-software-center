#include "PackageModel.h"

#include <KLocalizedString>

namespace Miryu {

PackageModel::PackageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int PackageModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_packages.size();
}

QVariant PackageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_packages.size())
        return {};

    const Package &pkg = m_packages.at(index.row());

    switch (role) {
    case NameRole:        return pkg.name;
    case ArchRole:        return pkg.arch;
    case VersionRole:     return pkg.version;
    case ReleaseRole:     return pkg.release;
    case EpochRole:       return pkg.epoch;
    case RepoRole:        return pkg.repo;
    case SummaryRole:     return pkg.summary;
    case DescriptionRole: return pkg.description;
    case UrlRole:         return pkg.url;
    case LicenseRole:     return pkg.license;
    case SizeRole:        return pkg.size;
    case SizeTextRole:    return formatSize(pkg.size);
    case StateRole:       return static_cast<int>(pkg.state);
    case StateTextRole:   return stateText(pkg.state);
    case TodoRole:        return static_cast<int>(pkg.todo);
    case TodoTextRole:    return todoText(pkg.todo);
    case QueuedRole:      return pkg.queued;
    case IsDepRole:       return pkg.isDep;
    case NevraRole:       return pkg.nevra();
    case IsInstalledRole: return pkg.isInstalled();
    case PackageRole:     return QVariant::fromValue(pkg);
    }

    return {};
}

QHash<int, QByteArray> PackageModel::roleNames() const
{
    return {
        {NameRole,        "name"},
        {ArchRole,        "arch"},
        {VersionRole,     "version"},
        {ReleaseRole,     "release"},
        {EpochRole,       "epoch"},
        {RepoRole,        "repo"},
        {SummaryRole,     "summary"},
        {DescriptionRole, "description"},
        {UrlRole,         "url"},
        {LicenseRole,     "license"},
        {SizeRole,        "size"},
        {SizeTextRole,    "sizeText"},
        {StateRole,       "state"},
        {StateTextRole,   "stateText"},
        {TodoRole,        "todo"},
        {TodoTextRole,    "todoText"},
        {QueuedRole,      "queued"},
        {IsDepRole,       "isDep"},
        {NevraRole,       "nevra"},
        {IsInstalledRole, "isInstalled"},
        {PackageRole,     "package"},
    };
}

void PackageModel::setPackages(const QList<Package> &packages)
{
    beginResetModel();
    m_packages = packages;
    endResetModel();
    Q_EMIT countChanged();
}

Package PackageModel::packageAt(int row) const
{
    if (row >= 0 && row < m_packages.size())
        return m_packages.at(row);
    return {};
}

void PackageModel::setQueued(const QString &nevra, bool queued)
{
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].nevra() == nevra) {
            m_packages[i].queued = queued;
            if (!queued)
                m_packages[i].todo = calcTodo(m_packages[i].state); // restore default marker
            QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {QueuedRole, TodoRole, TodoTextRole});
        }
    }
}

void PackageModel::setQueuedWithTodo(const QString &nevra, bool queued, PackageTodo todo)
{
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].nevra() == nevra) {
            m_packages[i].queued = queued;
            // Explicit todo while queued (e.g. Reinstall / Downgrade chosen
            // from the context menu or the info-panel buttons); on unqueue,
            // fall back to the state-derived default.
            m_packages[i].todo = queued ? todo : calcTodo(m_packages[i].state);
            QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {QueuedRole, TodoRole, TodoTextRole});
        }
    }
}

void PackageModel::clearQueued()
{
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].queued) {
            m_packages[i].queued = false;
            // Also reset any explicit todo (Reinstall / Downgrade / …) back
            // to the state-derived default, so the marker never lingers.
            m_packages[i].todo = calcTodo(m_packages[i].state);
            QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {QueuedRole, TodoRole, TodoTextRole});
        }
    }
}

QList<Package> PackageModel::queuedPackages() const
{
    QList<Package> result;
    for (const auto &pkg : m_packages) {
        if (pkg.queued)
            result.append(pkg);
    }
    return result;
}

QString PackageModel::formatSize(qint64 bytes) const
{
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    if (bytes < 1024 * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
}

QString PackageModel::stateText(PackageState state) const
{
    switch (state) {
    case PackageState::Installed:  return QStringLiteral("Installed");
    case PackageState::Available:  return QStringLiteral("Available");
    case PackageState::Update:     return QStringLiteral("Update");
    case PackageState::Downgrade:  return QStringLiteral("Downgrade");
    }
    return {};
}

QString PackageModel::todoText(PackageTodo todo) const
{
    // Localized marker text (the strings are also used by the action buttons
    // and the context menu, so they are already covered by the .po files).
    switch (todo) {
    case PackageTodo::Install:    return i18n("Install");
    case PackageTodo::Update:     return i18n("Update");
    case PackageTodo::Remove:     return i18n("Remove");
    case PackageTodo::Downgrade:  return i18n("Downgrade");
    case PackageTodo::Reinstall:  return i18n("Reinstall");
    case PackageTodo::DistroSync: return i18n("Sync");
    case PackageTodo::None:       return QString();
    }
    return {};
}

}
