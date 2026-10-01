#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <QMetaType>
#include "Package.h"
#include "Enums.h"

namespace Miryu {

struct TransactionItem
{
    QString nevra;
    QString repo;
    qint64 size = 0;
    QString action;
};

struct TransactionResult
{
    bool completed = false;
    QVariantMap data;
    QString error;
    QStringList problems;
    bool keyInstall = false;

    QList<TransactionItem> itemsByAction(const QString &action) const
    {
        QList<TransactionItem> result;
        QVariantList list = data.value(action).toList();
        for (const auto &item : list) {
            QVariantList pair = item.toList();
            if (pair.size() >= 2) {
                TransactionItem ti;
                QVariantList nevraRepo = pair[0].toList();
                if (nevraRepo.size() >= 2) {
                    ti.nevra = nevraRepo[0].toString();
                    ti.repo = nevraRepo[1].toString();
                }
                ti.size = pair[1].toLongLong();
                ti.action = action;
                result.append(ti);
            }
        }
        return result;
    }

    QStringList actionKeys() const
    {
        return data.keys();
    }

    qint64 totalSize() const
    {
        qint64 total = 0;
        for (const auto &key : data.keys()) {
            // Only packages that actually need to be downloaded contribute to
            // the download total. Actions that operate on already-installed
            // packages (remove / replaced / obsoleted) fetch nothing and must
            // be excluded, matching the semantics of "Total Download Size".
            if (key == QStringLiteral("replaced") ||
                key == QStringLiteral("remove") ||
                key == QStringLiteral("obsoleted"))
                continue;
            QVariantList list = data.value(key).toList();
            for (const auto &item : list) {
                QVariantList pair = item.toList();
                if (pair.size() >= 2)
                    total += pair[1].toLongLong();
            }
        }
        return total;
    }
};

struct TransactionOptions
{
    TransactionCommand command = TransactionCommand::None;
    QString parameter;
    bool offline = false;
};

struct DownloadPackage
{
    QString id;
    QString name;
    qint64 toDownload = 0;
    qint64 downloaded = 0;
    DownloadType type = DownloadType::Unknown;

    DownloadPackage() = default;
    DownloadPackage(const QString &id, const QString &name, qint64 total)
        : id(id), name(name), toDownload(total)
    {
        if (id.startsWith(QStringLiteral("repo:")))
            type = DownloadType::Repo;
        else if (id.startsWith(QStringLiteral("package:")))
            type = DownloadType::Package;
        else
            type = DownloadType::Unknown;
    }
};

}

Q_DECLARE_METATYPE(Miryu::TransactionItem)
Q_DECLARE_METATYPE(Miryu::TransactionResult)
Q_DECLARE_METATYPE(Miryu::TransactionOptions)
Q_DECLARE_METATYPE(Miryu::DownloadPackage)
