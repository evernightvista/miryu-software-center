#pragma once

#include <QAbstractListModel>
#include <QList>
#include "Package.h"

namespace Miryu {

class QueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(qint64 totalSize READ totalSize NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        ArchRole,
        VersionRole,
        ReleaseRole,
        RepoRole,
        SummaryRole,
        SizeRole,
        SizeTextRole,
        TodoRole,
        TodoTextRole,
        IsDepRole,
        NevraRole,
        ActionRole,
        PackageRole
    };
    Q_ENUM(Roles)

    explicit QueueModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void addPackage(const Package &pkg);
    void removePackage(const QString &nevra);
    void updateTodo(const QString &nevra, PackageTodo todo);
    void clear();
    bool contains(const QString &nevra) const;

    QList<Package> packages() const { return m_packages; }
    QList<Package> userPackages() const;
    QList<Package> dependencyPackages() const;

    int count() const { return m_packages.size(); }
    qint64 totalSize() const;

Q_SIGNALS:
    void countChanged();

private:
    QList<Package> m_packages;

    QString formatSize(qint64 bytes) const;
    QString todoText(PackageTodo todo) const;
};

}
