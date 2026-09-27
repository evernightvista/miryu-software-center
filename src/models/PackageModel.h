#pragma once

#include <QAbstractListModel>
#include <QList>
#include "Package.h"
#include "Enums.h"

namespace Miryu {

class PackageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        ArchRole,
        VersionRole,
        ReleaseRole,
        EpochRole,
        RepoRole,
        SummaryRole,
        DescriptionRole,
        UrlRole,
        LicenseRole,
        SizeRole,
        SizeTextRole,
        StateRole,
        StateTextRole,
        TodoRole,
        TodoTextRole,
        QueuedRole,
        IsDepRole,
        NevraRole,
        PackageRole,
        IsInstalledRole
    };
    Q_ENUM(Roles)

    explicit PackageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setPackages(const QList<Package> &packages);
    QList<Package> packages() const { return m_packages; }
    Package packageAt(int row) const;

    void setQueued(const QString &nevra, bool queued);
    void clearQueued();
    QList<Package> queuedPackages() const;

    int count() const { return m_packages.size(); }

Q_SIGNALS:
    void countChanged();

private:
    QList<Package> m_packages;

    QString formatSize(qint64 bytes) const;
    QString stateText(PackageState state) const;
    QString todoText(PackageTodo todo) const;
};

}
