#pragma once

#include <QObject>
#include <QString>
#include <QMetaType>

namespace Miryu {

class Repository
{
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(bool enabled MEMBER enabled)
    Q_PROPERTY(int priority MEMBER priority)

public:
    Repository() = default;

    QString id;
    QString name;
    bool enabled = false;
    int priority = 99;

    static Repository fromVariantMap(const QVariantMap &map)
    {
        Repository repo;
        repo.id = map.value(QStringLiteral("id")).toString();
        repo.name = map.value(QStringLiteral("name")).toString();
        if (repo.name.isEmpty())
            repo.name = repo.id;
        // Clean up display names: @System -> System, @fedora -> Fedora, etc.
        // dnf5daemon returns repo IDs with a leading '@' for some repos.
        if (repo.name.startsWith(QLatin1Char('@')))
            repo.name = repo.name.mid(1);
        repo.enabled = map.value(QStringLiteral("enabled")).toBool();
        repo.priority = map.value(QStringLiteral("priority"), 99).toInt();
        return repo;
    }
};

}

Q_DECLARE_METATYPE(Miryu::Repository)
