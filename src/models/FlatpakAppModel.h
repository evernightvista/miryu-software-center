#pragma once

#include <QAbstractListModel>
#include "FlatpakApp.h"

namespace Miryu {

class FlatpakAppModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        AppIdRole = Qt::UserRole + 1,
        NameRole,
        VersionRole,
        BranchRole,
        ArchRole,
        RemoteRole,
        InstallTypeRole,
        InstalledRole,
        UpgradableRole,
        DescriptionRole,
        SizeRole,
        SizeTextRole,
        AppRole,
        RefRole
    };
    Q_ENUM(Roles)

    explicit FlatpakAppModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setApps(const QList<FlatpakApp> &apps);
    QList<FlatpakApp> apps() const { return m_apps; }
    FlatpakApp appAt(int row) const;

    void updateAppStatus(const QString &appId, bool installed, bool upgradable);
    int count() const { return m_apps.size(); }

Q_SIGNALS:
    void countChanged();

private:
    QList<FlatpakApp> m_apps;

    QString formatSize(qint64 bytes) const;
};

}
