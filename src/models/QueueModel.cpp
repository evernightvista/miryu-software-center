#include "QueueModel.h"

namespace Miryu {

QueueModel::QueueModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int QueueModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_packages.size();
}

QVariant QueueModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_packages.size())
        return {};

    const Package &pkg = m_packages.at(index.row());

    switch (role) {
    case NameRole:      return pkg.name;
    case ArchRole:      return pkg.arch;
    case VersionRole:   return pkg.version;
    case ReleaseRole:   return pkg.release;
    case RepoRole:      return pkg.repo;
    case SummaryRole:   return pkg.summary;
    case SizeRole:      return pkg.size;
    case SizeTextRole:  return formatSize(pkg.size);
    case TodoRole:      return static_cast<int>(pkg.todo);
    case TodoTextRole:  return todoText(pkg.todo);
    case IsDepRole:     return pkg.isDep;
    case NevraRole:     return pkg.nevra();
    case ActionRole:    return todoText(pkg.todo);
    case PackageRole:   return QVariant::fromValue(pkg);
    }

    return {};
}

QHash<int, QByteArray> QueueModel::roleNames() const
{
    return {
        {NameRole,     "name"},
        {ArchRole,     "arch"},
        {VersionRole,  "version"},
        {ReleaseRole,  "release"},
        {RepoRole,     "repo"},
        {SummaryRole,  "summary"},
        {SizeRole,     "size"},
        {SizeTextRole, "sizeText"},
        {TodoRole,     "todo"},
        {TodoTextRole, "todoText"},
        {IsDepRole,    "isDep"},
        {NevraRole,    "nevra"},
        {ActionRole,   "action"},
        {PackageRole,  "package"},
    };
}

void QueueModel::addPackage(const Package &pkg)
{
    if (contains(pkg.nevra()))
        return;

    beginInsertRows(QModelIndex(), m_packages.size(), m_packages.size());
    m_packages.append(pkg);
    endInsertRows();
    Q_EMIT countChanged();
}

void QueueModel::removePackage(const QString &nevra)
{
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].nevra() == nevra) {
            beginRemoveRows(QModelIndex(), i, i);
            m_packages.removeAt(i);
            endRemoveRows();
            Q_EMIT countChanged();
            return;
        }
    }
}

void QueueModel::updateTodo(const QString &nevra, PackageTodo todo)
{
    for (int i = 0; i < m_packages.size(); ++i) {
        if (m_packages[i].nevra() == nevra) {
            m_packages[i].todo = todo;
            QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {TodoRole, TodoTextRole, ActionRole});
            return;
        }
    }
}

void QueueModel::clear()
{
    if (m_packages.isEmpty())
        return;
    beginResetModel();
    m_packages.clear();
    endResetModel();
    Q_EMIT countChanged();
}

bool QueueModel::contains(const QString &nevra) const
{
    for (const auto &pkg : m_packages) {
        if (pkg.nevra() == nevra)
            return true;
    }
    return false;
}

QList<Package> QueueModel::userPackages() const
{
    QList<Package> result;
    for (const auto &pkg : m_packages) {
        if (!pkg.isDep)
            result.append(pkg);
    }
    return result;
}

QList<Package> QueueModel::dependencyPackages() const
{
    QList<Package> result;
    for (const auto &pkg : m_packages) {
        if (pkg.isDep)
            result.append(pkg);
    }
    return result;
}

qint64 QueueModel::totalSize() const
{
    qint64 total = 0;
    for (const auto &pkg : m_packages)
        total += pkg.size;
    return total;
}

QString QueueModel::formatSize(qint64 bytes) const
{
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    if (bytes < 1024 * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
    return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
}

QString QueueModel::todoText(PackageTodo todo) const
{
    switch (todo) {
    case PackageTodo::Install:    return QStringLiteral("Install");
    case PackageTodo::Update:     return QStringLiteral("Update");
    case PackageTodo::Remove:     return QStringLiteral("Remove");
    case PackageTodo::Downgrade:  return QStringLiteral("Downgrade");
    case PackageTodo::Reinstall:  return QStringLiteral("Reinstall");
    case PackageTodo::DistroSync: return QStringLiteral("Sync");
    case PackageTodo::None:       return QStringLiteral("");
    }
    return {};
}

}
