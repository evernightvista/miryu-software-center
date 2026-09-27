#include "RepoModel.h"

namespace Miryu {

RepoModel::RepoModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RepoModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_repos.size();
}

QVariant RepoModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_repos.size())
        return {};

    const Repository &repo = m_repos.at(index.row());

    switch (role) {
    case IdRole:           return repo.id;
    case NameRole:         return repo.name;
    case EnabledRole:      return repo.enabled;
    case PriorityRole:     return repo.priority;
    case PriorityTextRole: return repo.priority == 99 ? QStringLiteral("Default") : QString::number(repo.priority);
    }

    return {};
}

QHash<int, QByteArray> RepoModel::roleNames() const
{
    return {
        {IdRole,           "id"},
        {NameRole,         "name"},
        {EnabledRole,      "enabled"},
        {PriorityRole,     "priority"},
        {PriorityTextRole, "priorityText"},
    };
}

void RepoModel::setRepositories(const QList<Repository> &repos)
{
    beginResetModel();
    m_repos = repos;
    endResetModel();
}

}
