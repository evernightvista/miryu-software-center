#include "FlatpakAppModel.h"

namespace Miryu {

FlatpakAppModel::FlatpakAppModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int FlatpakAppModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_apps.size();
}

QVariant FlatpakAppModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_apps.size())
        return {};

    const FlatpakApp &app = m_apps.at(index.row());

    switch (role) {
    case AppIdRole:        return app.appId;
    case NameRole:         return app.name;
    case VersionRole:      return app.version;
    case BranchRole:       return app.branch;
    case ArchRole:         return app.arch;
    case RemoteRole:       return app.remote;
    case InstallTypeRole:  return app.installType;
    case InstalledRole:    return app.installed;
    case UpgradableRole:   return app.upgradable;
    case DescriptionRole:  return app.description;
    case SizeRole:         return app.size;
    case SizeTextRole:     return formatSize(app.size);
    case AppRole:          return QVariant::fromValue(app);
    case RefRole:          return app.ref();
    }

    return {};
}

QHash<int, QByteArray> FlatpakAppModel::roleNames() const
{
    return {
        {AppIdRole,       "appId"},
        {NameRole,        "name"},
        {VersionRole,     "version"},
        {BranchRole,      "branch"},
        {ArchRole,        "arch"},
        {RemoteRole,      "remote"},
        {InstallTypeRole, "installType"},
        {InstalledRole,   "installed"},
        {UpgradableRole,  "upgradable"},
        {DescriptionRole,  "description"},
        {SizeRole,        "size"},
        {SizeTextRole,    "sizeText"},
        {AppRole,         "app"},
        {RefRole,         "ref"},
    };
}

void FlatpakAppModel::setApps(const QList<FlatpakApp> &apps)
{
    beginResetModel();
    m_apps = apps;
    endResetModel();
    Q_EMIT countChanged();
}

FlatpakApp FlatpakAppModel::appAt(int row) const
{
    if (row >= 0 && row < m_apps.size())
        return m_apps.at(row);
    return {};
}

void FlatpakAppModel::updateAppStatus(const QString &appId, bool installed, bool upgradable)
{
    for (int i = 0; i < m_apps.size(); ++i) {
        if (m_apps[i].appId == appId) {
            m_apps[i].installed = installed;
            m_apps[i].upgradable = upgradable;
            QModelIndex idx = index(i);
            Q_EMIT dataChanged(idx, idx, {InstalledRole, UpgradableRole});
        }
    }
}

QString FlatpakAppModel::formatSize(qint64 bytes) const
{
    return FlatpakApp::formatSize(bytes);
}

}
