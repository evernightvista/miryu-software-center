#pragma once

#include <QAbstractListModel>
#include "Repository.h"

namespace Miryu {

class RepoModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole,
        PriorityRole,
        PriorityTextRole
    };
    Q_ENUM(Roles)

    explicit RepoModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRepositories(const QList<Repository> &repos);
    QList<Repository> repositories() const { return m_repos; }

private:
    QList<Repository> m_repos;
};

}
